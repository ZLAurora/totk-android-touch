/* totk-setup: the setup library on a Linux host, for the importer tests and for running single stages on plain paths.
     totk-setup run --request R.json [--pins P.json] [--fake companion|shaders=ok|fail:MS ...]
                                                            the JNI run() path; the request's fds are inherited descriptors;
                                                            --fake stands in for the companion / shader generator (SetupRunHooks)
     totk-setup inspect --request R.json [--pins P.json]
     totk-setup keys --keys prod.keys                       which needed keys exist (never their values)
     totk-setup identify GAME --keys K [--update]           NCA inventory and the selected Program NCA
     totk-setup exefs --update U --keys K --out D --work W [--libdir L]
     totk-setup merge --base B --update U --keys K --out D --work W [--libdir L] [--part-bytes N] [--nobackup D]
     totk-setup romfs --part F [--part F ...] --out D [--part-bytes N] [--pins P]    D/romfs.bin or D/romfs.bin.000 ...
     totk-setup index --romfs F|D --out-index F --out-config F [--pins P]          one image file, or a folder with either layout
     totk-setup fonts --font F [--font F ...] --out-dir D --out-txt F
     totk-setup code --exefs D --out-rewritten R --out-game-data G [--work W] [--libdir L]
     totk-setup seed --romfs D --rewritten R --out O [--shader-list F] [--work W] [--libdir L]
     totk-setup companion --romfs D --out O [--work W] [--libdir L]
     totk-setup move --request R.json [--pins P.json] [--cancel-at PHASE] [--corrupt REL] [--assume-free BYTES]
                                                            the JNI move() path; the options are test hooks (SetupMoveHooks)
     totk-setup remove --dir D                              the JNI removeGameData() path
   Output is one JSON line on stdout; progress goes to stderr. Exit 0 ok, 1 failure, 3 cancelled (SIGINT / SIGTERM).
   TOTK_SETUP_MOUNTS=F replaces /proc/self/mounts for the FAT volume check (setup_part_bytes); TOTK_SETUP_RING=<n>x<bytes>
   the RomFS writer's ring geometry (romfs_image.c). */
#include "setup_internal.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static atomic_int cancel_flag;
static void on_signal(int sig) { (void)sig; atomic_store(&cancel_flag, 1); }
static void on_progress(void *user, const char *stage, uint64_t done, uint64_t total, const char *detail) {
    (void)user; fprintf(stderr, "[%s] %llu/%llu %s\n", stage, (unsigned long long)done, (unsigned long long)total, detail);
}
/* move test hooks: cancel as a phase starts; flip one byte of a staged copy before it is verified; a free-space figure. */
static const char *cancel_at, *corrupt; static uint64_t assume_free;
static void move_phase(void *user, const char *phase, const char *staging) {
    (void)user;
    fprintf(stderr, "[move] phase %s\n", phase);
    if (cancel_at && !strcmp(cancel_at, phase)) atomic_store(&cancel_flag, 1);
    if (corrupt && !strcmp(phase, "verify")) {
        char path[SETUP_PATH]; uint8_t b;
        int fd = st_path(path, sizeof path, "%s/%s", staging, corrupt) ? -1 : open(path, O_RDWR | O_CLOEXEC);
        struct stat st;
        if (fd < 0 || fstat(fd, &st) || !st.st_size || pread(fd, &b, 1, st.st_size / 2) != 1) { fprintf(stderr, "cannot corrupt %s\n", path); _exit(9); }
        b ^= 0x40;
        if (pwrite(fd, &b, 1, st.st_size / 2) != 1 || fsync(fd)) _exit(9);
        close(fd);
    }
}
static uint64_t move_free(void *user, const char *path) { (void)user; return assume_free ? assume_free : st_free_bytes(path); }
/* run test hooks (SetupRunHooks): --fake companion=ok:MS / shaders=fail:MS replaces that generator by one that reports
   progress for MS milliseconds (honouring cancel), then fails or writes a minimal output (shaders: an empty TOTKVKP1). */
typedef struct { int set, ok; unsigned ms; } Fake;
static Fake fakes[2];   /* companion, shaders */
static int fake_parse(const char *spec) {
    char verdict[8]; unsigned ms; int k = !strncmp(spec, "companion=", 10) ? 0 : !strncmp(spec, "shaders=", 8) ? 1 : -1;
    if (k < 0 || sscanf(strchr(spec, '=') + 1, "%7[a-z]:%u", verdict, &ms) != 2 || (strcmp(verdict, "ok") && strcmp(verdict, "fail"))) return -1;
    fakes[k] = (Fake){ 1, !strcmp(verdict, "ok"), ms };
    return 0;
}
static int fake_run(SetupCtx *c, int k, const char *out) {
    const char *stage = k ? "shaders" : "companion";
    for (unsigned t = 0; t < fakes[k].ms; t += 10) {
        if (setup_cancelled(c)) return SETUP_CANCELLED;
        setup_progress(c, stage, t, fakes[k].ms, "fake");
        usleep(10000);
    }
    if (!fakes[k].ok) { setup_fail(c, "internal", "fake %s failure", stage); return -1; }
    uint8_t pack[32] = "TOTKVKP1\1\0\0\0\x68"; pack[24] = 32;
    char p[SETUP_PATH];
    if (st_path(p, sizeof p, k ? "%s/seed.vkpack" : "%s/fake.txt", out) || st_write_file(p, pack, sizeof pack, 0644)) return st_io_fail(c, "write", p);
    return 0;
}
static int fake_companion(void *user, SetupCtx *c, const char *romfs, const char *out) {
    (void)user; return fakes[0].set ? fake_run(c, 0, out) : setup_stage_companion(c, romfs, out);
}
static int fake_seed(void *user, SetupCtx *c, const char *romfs, const char *rew, const char *out) {
    (void)user; return fakes[1].set ? fake_run(c, 1, out) : setup_stage_seed(c, romfs, rew, out);
}

typedef struct { const char *key; const char *vals[SETUP_MAX_ROMFS_PARTS]; int n; } Opt;
static Opt opts[24]; static int nopts; static const char *positional; static int flag_update;
static const char *opt(const char *k) { for (int i = 0; i < nopts; i++) if (!strcmp(opts[i].key, k)) return opts[i].vals[0]; return NULL; }
static Opt *opt_all(const char *k) { for (int i = 0; i < nopts; i++) if (!strcmp(opts[i].key, k)) return &opts[i]; return NULL; }
static int usage(void) { fprintf(stderr, "usage: see the header of android/setup/setup_cli.c\n"); return 2; }

static int finish(SetupCtx *c, int rc, Buf *extra) {
    Buf b = {0};
    if (!rc) { buf_puts(&b, "{\"ok\":true"); if (extra && extra->p) buf_puts(&b, extra->p); buf_puts(&b, "}"); }
    else {
        if (rc == SETUP_CANCELLED) { setup_clear_fail(c); setup_fail(c, "cancelled", "cancelled"); }
        buf_puts(&b, "{\"ok\":false,\"code\":"); buf_jstr(&b, c->fail_code[0] ? c->fail_code : "internal");
        buf_puts(&b, ",\"message\":"); buf_jstr(&b, c->fail_msg); buf_puts(&b, "}");
    }
    puts(b.p ? b.p : "{\"ok\":false}");
    buf_free(&b); if (extra) buf_free(extra);
    return rc == SETUP_CANCELLED ? 3 : rc ? 1 : 0;
}
static int open_input(SetupCtx *c, const char *path, SetupInput *in) {
    memset(in, 0, sizeof *in); in->fd = -1;
    if (!path) { setup_fail(c, "missing_input", "missing path argument"); return -1; }
    struct stat st;
    in->fd = open(path, O_RDONLY | O_CLOEXEC);
    if (in->fd < 0 || fstat(in->fd, &st)) return st_io_fail(c, "open", path);
    const char *slash = strrchr(path, '/');
    snprintf(in->name, sizeof in->name, "%s", slash ? slash + 1 : path);
    in->size = (uint64_t)st.st_size; in->present = 1;
    return 0;
}
static char *read_request(const char *path, size_t *len) {
    char *t;
    if (!path || st_read_file(path, &t, len, 16u << 20)) { fprintf(stderr, "cannot read request %s\n", path ? path : "(none)"); return NULL; }
    return t;
}

int main(int argc, char **argv) {
    if (argc < 2) return usage();
    const char *cmd = argv[1];
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--update") && strcmp(cmd, "exefs") && strcmp(cmd, "merge")) { flag_update = 1; continue; }
        if (!strncmp(argv[i], "--", 2)) {
            if (i + 1 >= argc) return usage();
            Opt *o = opt_all(argv[i] + 2);
            if (!o) { if (nopts == 24) return usage(); o = &opts[nopts++]; o->key = argv[i] + 2; }
            if (o->n == SETUP_MAX_ROMFS_PARTS) return usage();
            o->vals[o->n++] = argv[++i];
        } else if (!positional) positional = argv[i];
        else return usage();
    }
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL);
    SetupPins pins, *pins_override = NULL; char err[512];
    setup_pins_default(&pins);
    if (opt("pins")) { if (setup_pins_load(&pins, opt("pins"), err, sizeof err)) { fprintf(stderr, "%s\n", err); return 2; } pins_override = &pins; }

    if (!strcmp(cmd, "run") || !strcmp(cmd, "inspect") || !strcmp(cmd, "move")) {
        size_t len; char *req = read_request(opt("request"), &len);
        if (!req) return 2;
        cancel_at = opt("cancel-at"); corrupt = opt("corrupt"); assume_free = opt("assume-free") ? strtoull(opt("assume-free"), NULL, 10) : 0;
        Opt *fake = opt_all("fake");
        for (int i = 0; fake && i < fake->n; i++) if (fake_parse(fake->vals[i])) return usage();
        SetupMoveHooks hooks = { move_phase, move_free, NULL };
        SetupRunHooks run_hooks = { fake_companion, fake_seed, NULL };
        char *res = !strcmp(cmd, "run") ? setup_run_json(req, len, on_progress, NULL, &cancel_flag, pins_override, fake ? &run_hooks : NULL)
                  : !strcmp(cmd, "move") ? setup_move_json(req, len, on_progress, NULL, &cancel_flag, pins_override, &hooks)
                  : setup_inspect_json(req, len, pins_override);
        free(req);
        if (!res) return 1;
        puts(res);
        int ok = !strncmp(res, "{\"ok\":true", 10), cancelled = strstr(res, "\"code\":\"cancelled\"") != NULL;
        free(res);
        return ok ? 0 : cancelled ? 3 : 1;
    }
    if (!strcmp(cmd, "remove")) {
        char *res = setup_remove_json(opt("dir"));
        if (!res) return 1;
        puts(res);
        int ok = !strncmp(res, "{\"ok\":true", 10);
        free(res);
        return ok ? 0 : 1;
    }
    uint64_t part_bytes = opt("part-bytes") ? strtoull(opt("part-bytes"), NULL, 10) : 0;

    /* Single stages: work dir = staging (update.nca, hactool logs); keys go to <work>/import-keys. */
    char libdir[SETUP_PATH + 8] = "", exe[SETUP_PATH];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) { exe[n] = 0; char *s = strrchr(exe, '/'); if (s) { *s = 0; snprintf(libdir, sizeof libdir, "%s/host", exe); } }
    if (opt("libdir")) snprintf(libdir, sizeof libdir, "%s", opt("libdir"));
    const char *work = opt("work") ? opt("work") : ".";
    SetupCtx *c = calloc(1, sizeof *c);
    if (!c) return 1;
    setup_ctx_init(c, work, libdir, work, &cancel_flag);
    snprintf(c->staging, sizeof c->staging, "%s", work);
    c->pins = pins; c->progress = on_progress;
    int rc = 0; Buf extra = {0};

    if (!strcmp(cmd, "keys")) {
        SetupInput in; SetupKeys k;
        if (!(rc = open_input(c, opt("keys"), &in)) && !(rc = keys_load_fd(c, &in, &k))) {
            buf_printf(&extra, ",\"header_key\":%s,\"titlekek\":[", k.has_header ? "true" : "false");
            for (int g = 0, f = 1; g < KEY_GENERATIONS; g++) if (k.titlekek_mask >> g & 1) { buf_printf(&extra, f ? "%d" : ",%d", g); f = 0; }
            buf_puts(&extra, "],\"key_area_key_application\":[");
            for (int g = 0, f = 1; g < KEY_GENERATIONS; g++) if (k.kak_mask >> g & 1) { buf_printf(&extra, f ? "%d" : ",%d", g); f = 0; }
            buf_puts(&extra, "]");
            keys_wipe(&k);
        }
    } else if (!strcmp(cmd, "identify")) {
        SetupInput game, kin; SetupKeys k; Container ct;
        memset(&ct, 0, sizeof ct);
        if (!(rc = open_input(c, positional, &game)) && !(rc = open_input(c, opt("keys"), &kin)) && !(rc = keys_load_fd(c, &kin, &k))) {
            if (!(rc = container_open(c, &game, &ct, flag_update ? "update" : "base game")) && !(rc = container_inventory(c, &ct, &k))) {
                buf_printf(&extra, ",\"kind\":\"%s\",\"tickets\":%d,\"ncas\":[", ct.is_xci ? "xci" : "nsp", ct.ntickets);
                for (int i = 0; i < ct.nncas; i++) {
                    int bktr = 0; for (int s = 0; s < 4; s++) bktr |= ct.ncas[i].hdr[0x400 + s * 512 + 4] == 4;
                    buf_printf(&extra, "%s{\"title_id\":\"%s\",\"content_type\":%d,\"size\":%llu,\"bktr\":%s}", i ? "," : "", ct.ncas[i].title_id,
                               ct.ncas[i].content_type, (unsigned long long)ct.ncas[i].size, bktr ? "true" : "false");
                }
                buf_puts(&extra, "]");
                if (!(rc = container_select(c, &ct, flag_update)) && !(rc = container_check_key(c, &ct, &k, NULL, 0)))
                    buf_printf(&extra, ",\"program\":{\"title_id\":\"%s\",\"size\":%llu,\"bktr_section\":%d}", ct.ncas[ct.program].title_id,
                               (unsigned long long)ct.ncas[ct.program].size, ct.bktr_section);
            }
            keys_wipe(&k); container_free(&ct);
        }
    } else if (!strcmp(cmd, "exefs") || !strcmp(cmd, "merge")) {
        SetupRequest *r = calloc(1, sizeof *r); SetupContainers *s = calloc(1, sizeof *s);
        int merge = !strcmp(cmd, "merge");
        if (!r || !s || !opt("out") || !opt("work")) { free(r); free(s); free(c); return usage(); }
        if (merge) snprintf(c->nobackup, sizeof c->nobackup, "%s", opt("nobackup") ? opt("nobackup") : "");
        snprintf(c->keydir, sizeof c->keydir, "%s/import-keys", work);
        mkdir(c->keydir, 0700);
        if (!(rc = open_input(c, opt("update"), &r->update)) && !(rc = open_input(c, opt("keys"), &r->keys)) &&
            (!merge || !(rc = open_input(c, opt("base"), &r->base))) && !(rc = containers_prepare(c, r, s, merge))) {
            if (merge) { char sha[65]; if (!(rc = containers_romfs(c, s, opt("out"), part_bytes, sha))) buf_printf(&extra, ",\"sha256\":\"%s\"", sha); }
            else if (!(rc = containers_exefs(c, s, opt("out")))) rc = setup_exefs_check(c, opt("out"), 1);
        }
        containers_free(s); st_rmtree(c->keydir); free(r); free(s);
    } else if (!strcmp(cmd, "romfs")) {
        Opt *o = opt_all("part"); SetupInput *parts = calloc(SETUP_MAX_ROMFS_PARTS, sizeof *parts); char sha[65];
        if (!o || !opt("out") || !parts) { free(parts); free(c); return usage(); }
        for (int i = 0; i < o->n && !rc; i++) rc = open_input(c, o->vals[i], &parts[i]);
        if (!rc && !(rc = setup_romfs_copy(c, parts, o->n, opt("out"), part_bytes, sha))) buf_printf(&extra, ",\"sha256\":\"%s\"", sha);
        free(parts);
    } else if (!strcmp(cmd, "index")) {
        char sha[65], image[65];
        if (!opt("romfs") || !opt("out-index") || !opt("out-config")) { free(c); return usage(); }
        struct stat st; RomfsImage img;
        if (stat(opt("romfs"), &st) || (S_ISDIR(st.st_mode) ? romfs_image_open(&img, opt("romfs")) : romfs_image_open_file(&img, opt("romfs")))) rc = st_io_fail(c, "open", opt("romfs"));
        /* The image hash is the pinned one: re-hashing 17 GB here would only repeat the romfs stage's check. */
        snprintf(image, sizeof image, "%s", c->pins.romfs_sha256);
        if (!rc) {
            if (!(rc = setup_romfs_index(c, &img, opt("out-index"), sha)) && !(rc = setup_romfs_config(c, opt("out-config"), img.size, image, sha)))
                buf_printf(&extra, ",\"sha256\":\"%s\",\"parts\":%d", sha, img.nparts);
            romfs_image_close(&img);
        }
    } else if (!strcmp(cmd, "fonts")) {
        Opt *o = opt_all("font"); SetupInput fonts[SETUP_MAX_FONTS]; char sha[65];
        if (!o || o->n > SETUP_MAX_FONTS || !opt("out-dir") || !opt("out-txt")) { free(c); return usage(); }
        for (int i = 0; i < o->n && !rc; i++) rc = open_input(c, o->vals[i], &fonts[i]);
        if (!rc && !(rc = setup_fonts_write(c, fonts, o->n, opt("out-dir"), opt("out-txt"), sha))) buf_printf(&extra, ",\"source_sha256\":\"%s\"", sha);
    } else if (!strcmp(cmd, "code")) {
        if (!opt("exefs") || !opt("out-rewritten") || !opt("out-game-data")) { free(c); return usage(); }
        rc = setup_stage_code(c, opt("exefs"), opt("out-rewritten"), opt("out-game-data"));
    } else if (!strcmp(cmd, "seed")) {
        if (!opt("romfs") || !opt("rewritten") || !opt("out")) { free(c); return usage(); }
        SetupRequest *r = calloc(1, sizeof *r);
        if (!r) { free(c); return 1; }
        c->req = r;
        if (!opt("shader-list") || !(rc = open_input(c, opt("shader-list"), &r->shader_list))) rc = setup_stage_seed(c, opt("romfs"), opt("rewritten"), opt("out"));
        c->req = NULL; free(r);
    } else if (!strcmp(cmd, "companion")) {
        if (!opt("romfs") || !opt("out")) { free(c); return usage(); }
        rc = setup_stage_companion(c, opt("romfs"), opt("out"));
    } else { free(c); return usage(); }
    int code = finish(c, rc, &extra);
    free(c);
    return code;
}
