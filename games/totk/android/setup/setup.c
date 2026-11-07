/* Importer orchestration: the request, the stage order code -> romfs -> index -> fonts -> companion + shaders (those two
   side by side, run_companion_and_shaders), staging under <game>/.import, verification against the 1.4.3 pins, rename into
   place, and install.json (format 1), rewritten atomically after every stage so a killed run keeps what it committed. The
   install is playable once code, romfs, index, fonts and shaders are done (without the shader seed the game draws nothing);
   the companion stage is optional: its failure is recorded in install.json and never makes the install unplayable. */
#define _GNU_SOURCE 1
#include "setup_internal.h"
#include "totk_pins.h"
#include "../../runtime/romfs_proof.h"   /* the runtime's RomFS verification record, written after the romfs stage */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define V "v143"
#define GAME_DATA "game-data/" V
#define REWRITTEN "rewritten/" V
#define ROMFS_DIR SETUP_ROMFS_DIR
#define FONTS_DIR "extracted/firmware/shared-fonts"
#define SHADERS_DIR "shaders/" V
#define COMPANION_DIR "companion"
static const char *const stage_names[ST_COUNT] = { "code", "romfs", "index", "fonts", "companion", "shaders" };
static const char *const modules[3] = { "main", "sdk", "subsdk0" };
#define MIB (1ull << 20)

/* ---- stage functions other agents provide; weak stand-ins keep the library linkable and report not_available ---- */
__attribute__((weak)) int setup_stage_code(SetupCtx *c, const char *exefs, const char *rew, const char *gd) {
    (void)exefs; (void)rew; (void)gd;
    setup_fail(c, "not_available", "The code rewriter is not part of this build"); return -1;
}
__attribute__((weak)) int setup_stage_seed(SetupCtx *c, const char *romfs, const char *rew, const char *out) {
    (void)romfs; (void)rew; (void)out;
    setup_fail(c, "not_available", "The shader seed generator is not part of this build"); return -1;
}
__attribute__((weak)) int setup_stage_companion(SetupCtx *c, const char *romfs, const char *out) {
    (void)romfs; (void)out;
    setup_fail(c, "not_available", "The dual-screen companion generator is not part of this build"); return -1;
}

/* ---- context ---- */
typedef struct { char stage[16]; uint64_t last_ms; } Throttle;
static _Thread_local Throttle throttle;
int setup_cancelled(const SetupCtx *c) { return c->cancel && atomic_load(c->cancel); }
void setup_progress(SetupCtx *c, const char *stage, uint64_t done, uint64_t total, const char *detail) {
    if (!c->progress) return;
    /* At most ten updates a second per stage, but always the first one of a stage and the final one. */
    uint64_t now = st_now_ms();
    int fresh = strncmp(throttle.stage, stage, sizeof throttle.stage);
    if (!fresh && !(total && done >= total) && now - throttle.last_ms < 100) return;
    snprintf(throttle.stage, sizeof throttle.stage, "%s", stage); throttle.last_ms = now;
    c->progress(c->progress_user, stage, done, total, detail ? detail : "");
}
void setup_fail(SetupCtx *c, const char *code, const char *fmt, ...) {
    if (c->failed) return;
    c->failed = 1;
    snprintf(c->fail_code, sizeof c->fail_code, "%s", code);
    va_list ap; va_start(ap, fmt); vsnprintf(c->fail_msg, sizeof c->fail_msg, fmt, ap); va_end(ap);
}
void setup_clear_fail(SetupCtx *c) { c->failed = 0; c->fail_code[0] = c->fail_msg[0] = 0; }
const char *setup_game_dir(const SetupCtx *c) { return c->game; }
const char *setup_lib_dir(const SetupCtx *c) { return c->lib; }
const char *setup_staging_dir(const SetupCtx *c) { return c->staging; }
int setup_shader_list(const SetupCtx *c, int *fd, uint64_t *offset, uint64_t *length) {
    if (!c->req || !c->req->shader_list.present) return -1;
    *fd = c->req->shader_list.fd; *offset = c->req->shader_list.offset; *length = c->req->shader_list.size;
    return 0;
}
void setup_ctx_init(SetupCtx *c, const char *game, const char *lib, const char *nobackup, atomic_int *cancel) {
    memset(c, 0, sizeof *c);
    snprintf(c->game, sizeof c->game, "%s", game ? game : "");
    snprintf(c->lib, sizeof c->lib, "%s", lib ? lib : "");
    snprintf(c->nobackup, sizeof c->nobackup, "%s", nobackup ? nobackup : "");
    if (st_path(c->staging, sizeof c->staging, "%s/.import", c->game)) c->staging[0] = 0;
    if (st_path(c->keydir, sizeof c->keydir, "%s/import-keys", c->nobackup)) c->keydir[0] = 0;
    c->cancel = cancel;
    setup_pins_default(&c->pins);
}

/* ---- pins ---- */
void setup_pins_default(SetupPins *p) {
    memset(p, 0, sizeof *p);
    snprintf(p->title_id, sizeof p->title_id, "%s", TOTK_PIN_TITLE_ID);
    p->version = TOTK_PIN_VERSION; p->romfs_size = TOTK_PIN_ROMFS_SIZE;
    snprintf(p->romfs_sha256, 65, "%s", TOTK_PIN_ROMFS_SHA256); snprintf(p->index_sha256, 65, "%s", TOTK_PIN_ROMFS_INDEX_SHA256);
    const char *exefs[EXEFS_COUNT] = { TOTK_PIN_EXEFS_SHA256_MAIN, TOTK_PIN_EXEFS_SHA256_SDK, TOTK_PIN_EXEFS_SHA256_SUBSDK0,
                                       TOTK_PIN_EXEFS_SHA256_MAIN_NPDM, TOTK_PIN_EXEFS_SHA256_RTLD };
    for (int i = 0; i < EXEFS_COUNT; i++) snprintf(p->exefs[i], 65, "%s", exefs[i]);
    snprintf(p->rewritten[0], 65, "%s", TOTK_PIN_ANDROID_REWRITTEN_SHA256_MAIN);
    snprintf(p->rewritten[1], 65, "%s", TOTK_PIN_ANDROID_REWRITTEN_SHA256_SDK);
    snprintf(p->rewritten[2], 65, "%s", TOTK_PIN_ANDROID_REWRITTEN_SHA256_SUBSDK0);
#ifdef TOTK_PIN_ANDROID_RUNTIME_TABLES_SHA256
    snprintf(p->tables, 65, "%s", TOTK_PIN_ANDROID_RUNTIME_TABLES_SHA256);
#endif
}
static int sha_ok(const char *s) { return s && strlen(s) == 64 && strspn(s, "0123456789abcdef") == 64; }
int setup_pins_load(SetupPins *p, const char *path, char *err, size_t errcap) {
    char *text; size_t len;
    if (st_read_file(path, &text, &len, MIB)) { snprintf(err, errcap, "%s: %s", path, strerror(errno)); return -1; }
    Json *j = json_parse(text, len, err, errcap); free(text);
    if (!j) return -1;
    memset(p, 0, sizeof *p);
    int64_t v, size; int ok = 1;
    const Json *romfs = json_get(j, "romfs"), *exefs = json_get(j, "exefs_sha256"), *rew = json_get(j, "android_rewritten_sha256");
    const char *t = json_str(j, "title_id"), *tables = json_str(j, "android_runtime_tables_sha256");
    if (!t || strlen(t) != 16 || json_int(j, "version", &v) || json_int(romfs, "size", &size) || size <= 0 ||
        !sha_ok(json_str(romfs, "sha256")) || !sha_ok(json_str(j, "romfs_index_sha256"))) ok = 0;
    if (ok) {
        snprintf(p->title_id, 17, "%s", t); p->version = (uint64_t)v; p->romfs_size = (uint64_t)size;
        snprintf(p->romfs_sha256, 65, "%s", json_str(romfs, "sha256")); snprintf(p->index_sha256, 65, "%s", json_str(j, "romfs_index_sha256"));
        for (int i = 0; i < EXEFS_COUNT && ok; i++) { const char *s = json_str(exefs, setup_exefs_names[i]); if (sha_ok(s)) snprintf(p->exefs[i], 65, "%s", s); else ok = i > EXEFS_SUBSDK0; }
        for (int i = 0; i < 3 && ok; i++) { char k[32]; snprintf(k, sizeof k, "%s.patched.bin", modules[i]); const char *s = json_str(rew, k); if (sha_ok(s)) snprintf(p->rewritten[i], 65, "%s", s); else ok = 0; }
        if (tables) { if (sha_ok(tables)) snprintf(p->tables, 65, "%s", tables); else ok = 0; }
    }
    json_free(j); free(j);
    if (!ok) snprintf(err, errcap, "%s: not an expected-v*.json pin file", path);
    return ok ? 0 : -1;
}

/* ---- request ---- */
/* An input is an inherited descriptor ("fd", the caller keeps and closes it) or an absolute "path" this library opens
   itself (All-files access: no provider in between) and closes in setup_request_close. */
static int parse_input(const Json *o, SetupInput *in, int with_range, char *err, size_t errcap) {
    int64_t fd = -1, size = -1, off = 0, len = -1;
    const char *path = o && o->type == JSON_OBJ ? json_str(o, "path") : NULL;
    if (!o || o->type != JSON_OBJ || (!path && (json_int(o, "fd", &fd) || fd < 0 || fd > 65535)) || (path && path[0] != '/')) {
        snprintf(err, errcap, "input without a valid fd or absolute path"); return -1;
    }
    json_int(o, "size", &size);
    if (with_range) { json_int(o, "offset", &off); json_int(o, "length", &len); }
    const char *name = json_str(o, "name");
    snprintf(in->name, sizeof in->name, "%s", name && *name ? name : "file");
    if (path) {
        in->fd = open(path, O_RDONLY | O_CLOEXEC);
        if (in->fd < 0) { snprintf(err, errcap, "cannot open %s: %s", in->name, strerror(errno)); return -1; }
        in->owned = 1;
    } else in->fd = (int)fd;
    in->present = 1;
    struct stat st;
    if (fcntl(in->fd, F_GETFD) < 0 || fstat(in->fd, &st)) { snprintf(err, errcap, "descriptor %d for %s is not open", in->fd, in->name); return -1; }
    if (path && !S_ISREG(st.st_mode)) { snprintf(err, errcap, "cannot open %s: not a regular file", in->name); return -1; }
    if (off < 0) { snprintf(err, errcap, "negative offset for %s", in->name); return -1; }
    in->offset = (uint64_t)off;
    /* The descriptor's own size wins for regular files; a provider's reported size can be stale or missing. */
    if (S_ISREG(st.st_mode)) {
        uint64_t avail = (uint64_t)st.st_size > in->offset ? (uint64_t)st.st_size - in->offset : 0;
        in->size = len >= 0 ? (uint64_t)len : avail;
        if (in->size > avail) { snprintf(err, errcap, "%s: range beyond the end of the file", in->name); return -1; }
    } else if (len >= 0 || size >= 0) in->size = (uint64_t)(len >= 0 ? len : size);
    else { snprintf(err, errcap, "%s: unknown size", in->name); return -1; }
    return 0;
}
static int parse_list(const Json *a, SetupInput *out, int max, int *n, int with_range, char *err, size_t errcap) {
    *n = 0;
    if (!a) return 0;
    if (a->type == JSON_OBJ) { *n = 1; return parse_input(a, out, with_range, err, errcap); }
    if (a->type != JSON_ARR || (int)a->n > max) { snprintf(err, errcap, "too many or malformed inputs"); return -1; }
    for (size_t i = 0; i < a->n; i++) if (parse_input(&a->items[i], &out[i], with_range, err, errcap)) return -1;
    *n = (int)a->n;
    return 0;
}
void setup_request_close(SetupRequest *r) {
    SetupInput *all[] = { &r->base, &r->update, &r->keys, &r->shader_list };
    for (int i = 0; i < 4; i++) if (all[i]->owned) { close(all[i]->fd); all[i]->owned = 0; }
    SetupInput *lists[] = { r->romfs, r->exefs, r->fonts }; int max[] = { SETUP_MAX_ROMFS_PARTS, SETUP_MAX_EXEFS, SETUP_MAX_FONTS };
    for (int l = 0; l < 3; l++) for (int i = 0; i < max[l]; i++) if (lists[l][i].owned) { close(lists[l][i].fd); lists[l][i].owned = 0; }
}
int setup_request_parse(SetupRequest *r, const char *json, size_t len, char *err, size_t errcap) {
    memset(r, 0, sizeof *r);
    Json *j = json_parse(json, len, err, errcap);
    if (!j) return -1;
    int rc = -1;
    const char *game = json_str(j, "gameDir"), *priv = json_str(j, "privateDir"), *nb = json_str(j, "noBackupDir"), *lib = json_str(j, "nativeLibDir");
    if (j->type != JSON_OBJ || !game || game[0] != '/' || !nb || nb[0] != '/' || !lib) { snprintf(err, errcap, "request needs absolute gameDir and noBackupDir, and nativeLibDir"); goto out; }
    if (strlen(game) >= SETUP_PATH - 128 || strlen(nb) >= SETUP_PATH - 128 || strlen(lib) >= SETUP_PATH - 64 || (priv && strlen(priv) >= SETUP_PATH)) { snprintf(err, errcap, "directory path too long"); goto out; }
    snprintf(r->game, sizeof r->game, "%s", game); snprintf(r->nobackup, sizeof r->nobackup, "%s", nb);
    snprintf(r->lib, sizeof r->lib, "%s", lib); snprintf(r->priv, sizeof r->priv, "%s", priv ? priv : "");
    const Json *in = json_get(j, "inputs");
    if (in && in->type != JSON_OBJ) { snprintf(err, errcap, "inputs must be an object"); goto out; }
    int n;
    if (parse_list(json_get(in, "romfs"), r->romfs, SETUP_MAX_ROMFS_PARTS, &r->nromfs, 0, err, errcap) ||
        parse_list(json_get(in, "exefs"), r->exefs, SETUP_MAX_EXEFS, &r->nexefs, 0, err, errcap) ||
        parse_list(json_get(j, "fonts"), r->fonts, SETUP_MAX_FONTS, &r->nfonts, 1, err, errcap)) goto out;
    if ((json_get(in, "base") && parse_list(json_get(in, "base"), &r->base, 1, &n, 0, err, errcap)) ||
        (json_get(in, "update") && parse_list(json_get(in, "update"), &r->update, 1, &n, 0, err, errcap)) ||
        (json_get(in, "keys") && parse_list(json_get(in, "keys"), &r->keys, 1, &n, 0, err, errcap)) ||
        (json_get(j, "shaderList") && parse_list(json_get(j, "shaderList"), &r->shader_list, 1, &n, 1, err, errcap))) goto out;
    int64_t part_bytes = 0;
    if (json_get(j, "romfsPartBytes") && (json_int(j, "romfsPartBytes", &part_bytes) || part_bytes < 0)) { snprintf(err, errcap, "romfsPartBytes must be a non-negative integer"); goto out; }
    r->romfs_part_bytes = (uint64_t)part_bytes;
    r->skip_companion = json_bool(j, "skipCompanion");
    const Json *st = json_get(j, "stages");
    if (st && st->type != JSON_NULL) {
        if (st->type != JSON_ARR) { snprintf(err, errcap, "stages must be a list"); goto out; }
        for (size_t i = 0; i < st->n; i++) {
            int k = -1;
            for (int s = 0; s < ST_COUNT; s++) if (st->items[i].type == JSON_STR && !strcmp(st->items[i].s, stage_names[s])) k = s;
            if (k < 0) { snprintf(err, errcap, "unknown stage in request"); goto out; }
            r->stage_mask |= 1 << k;
        }
        r->stages_explicit = 1;
    } else r->stage_mask = (1 << ST_COUNT) - 1;
    rc = 0;
out:
    json_free(j); free(j);
    if (rc) setup_request_close(r);   /* what a failed parse opened itself */
    return rc;
}

/* ---- install.json ---- */
static void copy_sha(char *dst, const char *src) { if (sha_ok(src)) snprintf(dst, 65, "%s", src); }
int setup_install_load(const char *game, SetupInstall *s) {
    memset(s, 0, sizeof *s);
    char path[SETUP_PATH], *text; size_t len;
    if (st_path(path, sizeof path, "%s/install.json", game) || st_read_file(path, &text, &len, MIB)) return -1;
    char err[64]; Json *j = json_parse(text, len, err, sizeof err); free(text);
    if (!j) return -1;
    int64_t format; int rc = -1;
    const char *g = json_str(j, "game"), *v = json_str(j, "version"), *route = json_str(j, "route");
    const Json *st = json_get(j, "stages");
    if (!json_int(j, "format", &format) && format == 1 && g && !strcmp(g, "totk") && v && !strcmp(v, V) && st) {
        if (route) snprintf(s->route, sizeof s->route, "%s", route);
        for (int i = 0; i < ST_COUNT; i++) {
            const Json *o = json_get(st, stage_names[i]);
            s->done[i] = json_bool(o, "done");
            const char *code = json_str(o, "code"), *error = json_str(o, "error");
            if (code) snprintf(s->opt_code[i], sizeof s->opt_code[i], "%s", code);
            if (error) snprintf(s->opt_error[i], sizeof s->opt_error[i], "%s", error);
        }
        s->companion_skipped = !s->done[ST_COMPANION] && json_bool(json_get(st, "companion"), "skipped");
        const Json *code = json_get(st, "code"), *rew = json_get(code, "rewritten_sha256");
        for (int i = 0; i < 3; i++) { char k[32]; snprintf(k, sizeof k, "%s.patched.bin", modules[i]); copy_sha(s->rewritten[i], json_str(rew, k)); }
        copy_sha(s->tables, json_str(code, "runtime_tables_sha256"));
        const Json *romfs = json_get(st, "romfs"); int64_t size = 0, parts = 0;
        if (!json_int(romfs, "size", &size) && size > 0) s->romfs_size = (uint64_t)size;
        if (!json_int(romfs, "parts", &parts) && parts > 0 && parts <= SETUP_MAX_ROMFS_PARTS) s->romfs_parts = (int)parts;
        copy_sha(s->romfs_sha, json_str(romfs, "sha256"));
        copy_sha(s->index_sha, json_str(json_get(st, "index"), "sha256"));
        const Json *fonts = json_get(st, "fonts"); const char *src = json_str(fonts, "source");
        if (src) snprintf(s->font_source, sizeof s->font_source, "%s", src);
        copy_sha(s->font_sha, json_str(fonts, "sha256"));
        json_int(json_get(st, "shaders"), "variants", &s->variants);
        rc = 0;
    }
    json_free(j); free(j);
    return rc;
}
/* The game needs the shader seed (an empty seed draws nothing); the companion art is optional. */
static int playable(const SetupInstall *s) {
    return s->done[ST_CODE] && s->done[ST_ROMFS] && s->done[ST_INDEX] && s->done[ST_FONTS] && s->done[ST_SHADERS];
}
static void opt_fields(Buf *b, const SetupInstall *s, int i) {
    if (!s->done[i] && s->opt_code[i][0]) { buf_puts(b, ",\"code\":"); buf_jstr(b, s->opt_code[i]); buf_puts(b, ",\"error\":"); buf_jstr(b, s->opt_error[i]); }
}
void setup_install_json(Buf *b, const SetupInstall *s) {
    buf_puts(b, "{\"format\":1,\"game\":\"totk\",\"version\":\"" V "\",\"route\":");
    if (s->route[0]) buf_jstr(b, s->route); else buf_puts(b, "null");
    buf_printf(b, ",\"stages\":{\"code\":{\"done\":%s", s->done[ST_CODE] ? "true" : "false");
    if (s->done[ST_CODE]) {
        buf_puts(b, ",\"rewritten_sha256\":{");
        for (int i = 0; i < 3; i++) buf_printf(b, "%s\"%s.patched.bin\":\"%s\"", i ? "," : "", modules[i], s->rewritten[i]);
        buf_printf(b, "},\"runtime_tables_sha256\":\"%s\"", s->tables);
    }
    opt_fields(b, s, ST_CODE);
    buf_printf(b, "},\"romfs\":{\"done\":%s", s->done[ST_ROMFS] ? "true" : "false");
    /* parts: how the image lies in extracted/<version>/romfs (0 = one romfs.bin); the runtime finds the layout itself. */
    if (s->done[ST_ROMFS]) buf_printf(b, ",\"size\":%llu,\"sha256\":\"%s\",\"parts\":%d", (unsigned long long)s->romfs_size, s->romfs_sha, s->romfs_parts);
    opt_fields(b, s, ST_ROMFS);
    buf_printf(b, "},\"index\":{\"done\":%s", s->done[ST_INDEX] ? "true" : "false");
    if (s->done[ST_INDEX]) buf_printf(b, ",\"sha256\":\"%s\"", s->index_sha);
    opt_fields(b, s, ST_INDEX);
    buf_printf(b, "},\"fonts\":{\"done\":%s", s->done[ST_FONTS] ? "true" : "false");
    if (s->done[ST_FONTS]) { buf_puts(b, ",\"source\":"); buf_jstr(b, s->font_source); buf_printf(b, ",\"sha256\":\"%s\"", s->font_sha); }
    opt_fields(b, s, ST_FONTS);
    buf_printf(b, "},\"companion\":{\"done\":%s", s->done[ST_COMPANION] ? "true" : "false");
    if (s->companion_skipped && !s->done[ST_COMPANION]) buf_puts(b, ",\"skipped\":true");
    opt_fields(b, s, ST_COMPANION);
    buf_printf(b, "},\"shaders\":{\"done\":%s,\"variants\":%lld", s->done[ST_SHADERS] ? "true" : "false", (long long)(s->done[ST_SHADERS] ? s->variants : 0));
    opt_fields(b, s, ST_SHADERS);
    buf_printf(b, "}},\"playable\":%s,\"setup_version\":1}", playable(s) ? "true" : "false");
}
int setup_install_write(SetupCtx *c, const SetupInstall *s) {
    Buf b = {0}; char path[SETUP_PATH];
    setup_install_json(&b, s); buf_puts(&b, "\n");
    if (b.oom) { buf_free(&b); setup_fail(c, "internal", "Out of memory"); return -1; }
    int rc = st_path(path, sizeof path, "%s/install.json", c->game) || st_write_atomic(path, b.p, b.n) ? st_io_fail(c, "write", "install.json") : 0;
    buf_free(&b);
    return rc;
}
/* A done stage whose outputs are gone (cleared storage, manual deletion) is pending again. Cheap checks only. */
static int outputs_present(const SetupCtx *c, const SetupInstall *s, int stage) {
    char p[SETUP_PATH];
    switch (stage) {
    case ST_CODE:
        for (int i = 0; i < 3; i++) if (st_path(p, sizeof p, "%s/" REWRITTEN "/%s.patched.bin", c->game, modules[i]) || st_file_size(p) <= 0) return 0;
        return !st_path(p, sizeof p, "%s/" GAME_DATA "/runtime-tables.bin", c->game) && st_file_size(p) > 0;
    case ST_ROMFS: {
        RomfsImage img;
        if (st_path(p, sizeof p, "%s/" ROMFS_DIR, c->game) || romfs_image_open(&img, p)) return 0;
        int ok = img.size == c->pins.romfs_size && img.nparts == s->romfs_parts && s->romfs_size == c->pins.romfs_size;
        romfs_image_close(&img);
        return ok;
    }
    case ST_INDEX:
        return !st_path(p, sizeof p, "%s/" ROMFS_DIR "/runtime-index.bin", c->game) && st_file_size(p) > 0 &&
               !st_path(p, sizeof p, "%s/" GAME_DATA "/romfs-config.txt", c->game) && st_file_size(p) > 0;
    case ST_FONTS:
        for (int i = 0; i < 6; i++) if (st_path(p, sizeof p, "%s/" FONTS_DIR "/%s.bfttf", c->game, setup_font_names[i]) || st_file_size(p) <= 0) return 0;
        return !st_path(p, sizeof p, "%s/" GAME_DATA "/shared-fonts.txt", c->game) && st_file_size(p) > 0;
    case ST_COMPANION:
        return !st_path(p, sizeof p, "%s/" COMPANION_DIR, c->game) && st_exists(p);
    default:
        return !st_path(p, sizeof p, "%s/" SHADERS_DIR "/seed.vkpack", c->game) && st_file_size(p) > 0;
    }
}

/* ---- stage drivers ---- */
typedef struct { SetupCtx *c; const SetupRequest *r; const SetupRunHooks *hooks; SetupContainers sc; SetupInstall st; } Run;
static int dir_in(SetupCtx *c, char *out, const char *sub) {
    if (st_path(out, SETUP_PATH, "%s/%s", c->staging, sub) || st_rmtree(out) || st_mkdirs(out)) return st_io_fail(c, "create", sub);
    return 0;
}
static int commit_file(SetupCtx *c, const char *from_dir, const char *name, const char *to_rel) {
    char from[SETUP_PATH], to[SETUP_PATH];
    if (st_path(from, sizeof from, "%s/%s", from_dir, name) || st_path(to, sizeof to, "%s/%s/%s", c->game, to_rel, name)) return st_io_fail(c, "move", name);
    return st_commit(c, from, to);
}
static int use_decrypted_code(const SetupRequest *r) { return r->nexefs > 0; }
static int use_decrypted_romfs(const SetupRequest *r) { return r->nromfs > 0; }

static int run_code(Run *u) {
    SetupCtx *c = u->c; char exefs[SETUP_PATH], rew[SETUP_PATH], gd[SETUP_PATH]; int rc;
    if ((rc = dir_in(c, exefs, "exefs"))) return rc;
    if (use_decrypted_code(u->r)) rc = setup_exefs_from_files(c, u->r, exefs);
    else if (!(rc = containers_prepare(c, u->r, &u->sc, 0)) && !(rc = containers_exefs(c, &u->sc, exefs))) rc = setup_exefs_check(c, exefs, 1);
    if (rc) return rc;
    if ((rc = dir_in(c, rew, "code/rewritten")) || (rc = dir_in(c, gd, "code/game-data"))) return rc;
    setup_progress(c, "code", 0, 0, "rewriting the game code for Android");
    rc = setup_stage_code(c, exefs, rew, gd);
    if (rc) { if (rc == -1 && !c->failed) setup_fail(c, "internal", "The code stage failed without a message"); return rc; }
    char hex[3][65], tables[65], p[SETUP_PATH];
    for (int i = 0; i < 3; i++) {
        if (st_path(p, sizeof p, "%s/%s.patched.bin", rew, modules[i])) return st_io_fail(c, "open", p);
        if ((rc = st_sha256_file(c, p, hex[i], NULL))) return rc;
        if (strcmp(hex[i], c->pins.rewritten[i])) { setup_fail(c, "hash_mismatch", "The rewritten %s does not match the pinned Android build", modules[i]); return -1; }
    }
    if (st_path(p, sizeof p, "%s/runtime-tables.bin", gd)) return st_io_fail(c, "open", p);
    if ((rc = st_sha256_file(c, p, tables, NULL))) return rc;
    if (c->pins.tables[0] && strcmp(tables, c->pins.tables)) { setup_fail(c, "hash_mismatch", "runtime-tables.bin does not match the pinned tables"); return -1; }
    for (int i = 0; i < 3; i++) { char n[32]; snprintf(n, sizeof n, "%s.patched.bin", modules[i]); if ((rc = commit_file(c, rew, n, REWRITTEN))) return rc; }
    if ((rc = commit_file(c, gd, "runtime-tables.bin", GAME_DATA))) return rc;
    for (int i = 0; i < 3; i++) memcpy(u->st.rewritten[i], hex[i], 65);
    memcpy(u->st.tables, tables, 65);
    return 0;
}
/* The runtime's RomFS verification records (runtime/romfs_proof.h) in <private>/state, the TOTK_ROMFS_PROOF_DIR the Android
   frontend uses, so the first game start does not hash the 17 GB image setup has just verified. Best effort: without them
   the runtime hashes once and writes them itself. */
static int proof_paths(const char *priv, char *dir, char *single, char *parts) {
    return !priv || !*priv || st_path(dir, SETUP_PATH, "%s/state", priv) || st_path(single, SETUP_PATH, "%s/" ROMFS_PROOF_FILE, dir) ||
           st_path(parts, SETUP_PATH, "%s/" ROMFS_PROOF_PARTS_FILE, dir) ? -1 : 0;
}
void setup_proof_remove(const char *priv) {
    char dir[SETUP_PATH], single[SETUP_PATH], parts[SETUP_PATH], samples[SETUP_PATH];
    if (!proof_paths(priv, dir, single, parts) && !st_path(samples, sizeof samples, "%s/" ROMFS_SAMPLES_FILE, dir)) { unlink(single); unlink(parts); unlink(samples); }
}
static int sample_read(void *user, int part, uint64_t off, void *buf, size_t n) { return romfs_image_pread(user, buf, n, (uint64_t)part * ((RomfsImage *)user)->part_size + off); }
static void sample_digest(const void *data, size_t n, uint8_t out[32]) { Sha256 s; sha256_init(&s); sha256_update(&s, data, n); sha256_final(&s, out); }
void setup_proof_write(const char *priv, const char *romfs_dir, const char *sha256) {
    char dir[SETUP_PATH], single[SETUP_PATH], parts[SETUP_PATH], samples[SETUP_PATH], tmp[SETUP_PATH + 8];
    RomfsProof p[SETUP_MAX_ROMFS_PARTS]; RomfsImage img; RomfsSamples smp; uint64_t sizes[SETUP_MAX_ROMFS_PARTS];
    if (proof_paths(priv, dir, single, parts) || st_path(samples, sizeof samples, "%s/" ROMFS_SAMPLES_FILE, dir) || romfs_image_open(&img, romfs_dir)) return;
    /* Stat after the final rename: the rename itself changes ctime, which the one-file record includes. */
    int n = img.nparts ? img.nparts : 1, ok = 1;
    for (int i = 0; i < n && ok; i++) {
        struct stat st;
        ok = !romfs_image_stat(&img, i, &st);
        if (ok) { p[i] = img.nparts ? romfs_proof_part_for(&st, sha256) : romfs_proof_for(&st, sha256); sizes[i] = (uint64_t)st.st_size; }
    }
    /* The sampled-content sidecar (romfs_proof.h), so a remounted card does not cost a full hash: a fresh secret salt. */
    uint8_t salt[32]; int rnd = open("/dev/urandom", O_RDONLY | O_CLOEXEC), have_samples = 0;
    uint8_t *scratch = malloc(ROMFS_SAMPLE_SCRATCH);
    if (ok && rnd >= 0 && scratch && read(rnd, salt, 32) == 32)
        have_samples = !romfs_samples_make(&smp, salt, n, sizes, sample_read, &img, sample_digest, scratch);
    if (rnd >= 0) close(rnd);
    free(scratch);
    const char *path = img.nparts ? parts : single, *other = img.nparts ? single : parts;
    romfs_image_close(&img);
    unlink(samples);
    if (!ok || st_path(tmp, sizeof tmp, "%s.tmp", path) || st_mkdirs(dir)) return;
    unlink(other);   /* the other layout's record describes files that are gone */
    if (st_write_file(tmp, p, (size_t)n * sizeof *p, 0600) || rename(tmp, path)) { unlink(tmp); return; }
    if (have_samples && !st_path(tmp, sizeof tmp, "%s.tmp", samples) && (st_write_file(tmp, &smp, sizeof smp, 0600) || rename(tmp, samples))) unlink(tmp);
    st_fsync_dir(dir);
}
static int run_romfs(Run *u) {
    SetupCtx *c = u->c; char dir[SETUP_PATH], final[SETUP_PATH], sha[65]; int rc;
    setup_proof_remove(u->r->priv);   /* a record for the image this stage replaces must never vouch for the new one */
    if ((rc = dir_in(c, dir, "romfs"))) return rc;
    if (st_path(final, sizeof final, "%s/" ROMFS_DIR, c->game)) return st_io_fail(c, "create", final);
    uint64_t part_bytes = setup_part_bytes(c->game, u->r->romfs_part_bytes);
    int nparts = romfs_parts_for(c->pins.romfs_size, part_bytes);
    if (nparts < 0) { setup_fail(c, "internal", "romfsPartBytes %llu splits the RomFS into more than %d parts", (unsigned long long)part_bytes, SETUP_MAX_ROMFS_PARTS); return -1; }
    if (use_decrypted_romfs(u->r)) rc = setup_romfs_copy(c, u->r->romfs, u->r->nromfs, dir, part_bytes, sha);
    else if (!(rc = containers_prepare(c, u->r, &u->sc, 1))) rc = containers_romfs(c, &u->sc, dir, part_bytes, sha);
    if (rc) return rc;
    /* Same names are replaced in one rename each; then the old layout's other files go (a leftover romfs.bin would shadow
       new parts, a leftover higher part would be read as one). The old and new files hold the same pinned bytes. */
    for (int i = 0; i < (nparts ? nparts : 1); i++) {
        char name[32]; snprintf(name, sizeof name, nparts ? "romfs.bin.%03d" : "romfs.bin", i);
        if ((rc = commit_file(c, dir, name, ROMFS_DIR))) return rc;
    }
    if (romfs_remove_stale(final, nparts)) return st_io_fail(c, "remove the old RomFS from", final);
    u->st.romfs_size = c->pins.romfs_size; u->st.romfs_parts = nparts; memcpy(u->st.romfs_sha, sha, 65);
    setup_proof_write(u->r->priv, final, sha);
    return 0;
}
static int run_index(Run *u) {
    SetupCtx *c = u->c; char dir[SETUP_PATH], romfs[SETUP_PATH], idx[SETUP_PATH], cfg[SETUP_PATH], sha[65]; int rc;
    if (!u->st.done[ST_ROMFS]) { setup_fail(c, "missing_input", "The RomFS has not been imported yet"); return -1; }
    if ((rc = dir_in(c, dir, "index"))) return rc;
    if (st_path(romfs, sizeof romfs, "%s/" ROMFS_DIR, c->game) || st_path(idx, sizeof idx, "%s/runtime-index.bin", dir) ||
        st_path(cfg, sizeof cfg, "%s/romfs-config.txt", dir)) return st_io_fail(c, "create", dir);
    RomfsImage img;
    if (romfs_image_open(&img, romfs)) return st_io_fail(c, "open the RomFS image in", romfs);
    rc = setup_romfs_index(c, &img, idx, sha);
    romfs_image_close(&img);
    if (rc || (rc = setup_romfs_config(c, cfg, u->st.romfs_size, u->st.romfs_sha, sha))) return rc;
    if ((rc = commit_file(c, dir, "runtime-index.bin", ROMFS_DIR)) || (rc = commit_file(c, dir, "romfs-config.txt", GAME_DATA))) return rc;
    memcpy(u->st.index_sha, sha, 65);
    return 0;
}
static int run_fonts(Run *u) {
    SetupCtx *c = u->c; char dir[SETUP_PATH], fonts[SETUP_PATH], txt[SETUP_PATH], sha[65]; int rc;
    if ((rc = dir_in(c, dir, "fonts"))) return rc;
    if (st_path(fonts, sizeof fonts, "%s/shared-fonts", dir) || st_path(txt, sizeof txt, "%s/shared-fonts.txt", dir)) return st_io_fail(c, "create", dir);
    if ((rc = setup_fonts_write(c, u->r->fonts, u->r->nfonts, fonts, txt, sha))) return rc;
    for (int i = 0; i < 6; i++) { char n[64]; snprintf(n, sizeof n, "%s.bfttf", setup_font_names[i]); if ((rc = commit_file(c, fonts, n, FONTS_DIR))) return rc; }
    if ((rc = commit_file(c, dir, "shared-fonts.txt", GAME_DATA))) return rc;
    snprintf(u->st.font_source, sizeof u->st.font_source, "%s", u->r->fonts[0].name); memcpy(u->st.font_sha, sha, 65);
    return 0;
}
static int need_core(SetupCtx *c, const SetupInstall *s, int companion) {
    if (!s->done[ST_ROMFS] || !s->done[ST_INDEX] || (!companion && !s->done[ST_CODE])) {
        setup_fail(c, "missing_input", "Import the game first (%s)", companion ? "RomFS" : "code and RomFS"); return -1; }
    return 0;
}
static int run_companion(Run *u) {
    SetupCtx *c = u->c; char out[SETUP_PATH], romfs[SETUP_PATH], final[SETUP_PATH], old[SETUP_PATH]; int rc;
    if ((rc = need_core(c, &u->st, 1)) || (rc = dir_in(c, out, "companion"))) return rc;
    if (st_path(romfs, sizeof romfs, "%s/" ROMFS_DIR, c->game) || st_path(final, sizeof final, "%s/" COMPANION_DIR, c->game) ||
        st_path(old, sizeof old, "%s/companion-old", c->staging)) return st_io_fail(c, "create", out);
    setup_progress(c, "companion", 0, 0, "generating dual-screen assets");
    rc = u->hooks && u->hooks->companion ? u->hooks->companion(u->hooks->user, c, romfs, out) : setup_stage_companion(c, romfs, out);
    if (rc) return rc;
    /* Swap the whole tree: the previous one moves aside first, so the runtime never sees a mix of both. */
    st_rmtree(old);
    if (st_exists(final) && rename(final, old)) return st_io_fail(c, "replace", final);
    if (rename(out, final)) { int e = errno; rename(old, final); errno = e; return st_io_fail(c, "move into place", final); }
    st_fsync_dir(c->game); st_rmtree(old);
    return 0;
}
static int run_shaders(Run *u) {
    SetupCtx *c = u->c; char out[SETUP_PATH], romfs[SETUP_PATH], rew[SETUP_PATH]; int rc;
    if ((rc = need_core(c, &u->st, 0)) || (rc = dir_in(c, out, "shaders"))) return rc;
    if (st_path(romfs, sizeof romfs, "%s/" ROMFS_DIR, c->game) || st_path(rew, sizeof rew, "%s/" REWRITTEN, c->game)) return st_io_fail(c, "create", out);
    setup_progress(c, "shaders", 0, 0, "building the shader seed");
    rc = u->hooks && u->hooks->seed ? u->hooks->seed(u->hooks->user, c, romfs, rew, out) : setup_stage_seed(c, romfs, rew, out);
    if (rc) return rc;
    char pack[SETUP_PATH];
    if (st_path(pack, sizeof pack, "%s/seed.vkpack", out) || st_file_size(pack) < 32) { setup_fail(c, "internal", "The shader stage wrote no seed.vkpack"); return -1; }
    /* TOTKVKP1 (scripts/pack_shader_seed.py): <8sIIQQ> header (count = files), then <84sIQQ> records; a variant is one
       translated SPIR-V, i.e. one record named *.spv. */
    uint8_t h[32], rec[104]; int fd = open(pack, O_RDONLY | O_CLOEXEC); int64_t count = 0;
    if (fd >= 0 && !st_pread(fd, h, 32, 0) && !memcmp(h, "TOTKVKP1", 8)) {
        uint64_t files = 0; for (int i = 0; i < 8; i++) files |= (uint64_t)h[16 + i] << 8 * i;
        for (uint64_t i = 0; i < files && i < (1u << 24) && !st_pread(fd, rec, sizeof rec, 32 + i * 104); i++) {
            size_t n = strnlen((const char *)rec, 84);
            count += n > 4 && !memcmp(rec + n - 4, ".spv", 4);
        }
    }
    if (fd >= 0) close(fd);
    DIR *d = opendir(out);
    if (!d) return st_io_fail(c, "open", out);
    for (struct dirent *e; (e = readdir(d));) {
        if (e->d_name[0] == '.') continue;
        if ((rc = commit_file(c, out, e->d_name, SHADERS_DIR))) break;
    }
    closedir(d);
    if (!rc) u->st.variants = count;
    return rc;
}
typedef int (*StageFn)(Run *);
static const StageFn stage_fns[ST_COUNT] = { run_code, run_romfs, run_index, run_fonts, run_companion, run_shaders };

/* Unthrottled progress from the calling (JNI listener) thread. */
static void progress_now(SetupCtx *c, const char *stage, uint64_t done, uint64_t total, const char *detail) {
    if (c->progress) c->progress(c->progress_user, stage, done, total, detail ? detail : "");
}
/* Records stage i's outcome (rc; sc: the context it ran with) in install.json. Nonzero when the run must stop - a required
   stage failed (everything but companion), a cancel, or install.json could not be written - with the reason in c. A failed
   shader stage is also recorded in install.json (its code and error, for Game data) and its progress ends with "failed".
   The companion stage's failure keeps the run going: install.json remembers why it is missing, and its progress ends with
   "skipped". */
static int stage_outcome(Run *u, const SetupCtx *sc, int i, int rc) {
    SetupCtx *c = u->c; const SetupRequest *r = u->r;
    if (i == ST_COMPANION) u->st.companion_skipped = 0;
    if (!rc) {
        u->st.done[i] = 1; u->st.opt_code[i][0] = u->st.opt_error[i][0] = 0;
        if (i == ST_CODE || i == ST_ROMFS) {
            const char *route = (i == ST_ROMFS ? use_decrypted_romfs(r) : use_decrypted_code(r)) ? "decrypted" : "containers";
            if (i == ST_ROMFS || !u->st.route[0]) snprintf(u->st.route, sizeof u->st.route, "%s", route);
        }
        if (setup_install_write(c, &u->st)) return -1;
        progress_now(c, stage_names[i], 1, 1, "done");
        return 0;
    }
    if (sc != c) { setup_clear_fail(c); if (sc->failed) setup_fail(c, sc->fail_code, "%s", sc->fail_msg); }
    if (rc == SETUP_CANCELLED) { setup_clear_fail(c); setup_fail(c, "cancelled", "Setup was cancelled"); return -1; }
    if (i < ST_COMPANION) return -1;
    snprintf(u->st.opt_code[i], sizeof u->st.opt_code[i], "%s", c->fail_code[0] ? c->fail_code : "internal");
    snprintf(u->st.opt_error[i], sizeof u->st.opt_error[i], "%.511s", c->fail_msg);
    if (i == ST_SHADERS) {   /* c keeps the stage's failure */
        setup_install_write(c, &u->st);
        progress_now(c, stage_names[i], 0, 0, "failed");
        return -1;
    }
    setup_clear_fail(c);
    if (setup_install_write(c, &u->st)) return -1;
    progress_now(c, stage_names[i], 0, 0, "skipped");
    return 0;
}

/* ---- companion and shaders side by side ----
   Both only read the committed RomFS and write their own staging folders, so they run on two threads at once. Each has
   its own SetupCtx (failures) and Run (install state copy); the calling thread forwards both progress streams (the JNI
   listener belongs to it) and records each outcome in install.json as soon as that stage ends, one write at a time, so
   a run killed while one of them still works keeps the other. A failed shader stage fails the run once the companion
   lane has ended too (its outcome is still recorded). A cancel reaches both through the shared flag. */
typedef struct Pair Pair;
typedef struct {
    Pair *pair; int stage, rc, finished, recorded, started, dirty;
    SetupCtx c; Run u; pthread_t thread;
    char pstage[16], detail[256]; uint64_t done, total;
} Lane;
struct Pair { pthread_mutex_t mu; pthread_cond_t cv; Lane lane[2]; };
static void lane_progress(void *user, const char *stage, uint64_t done, uint64_t total, const char *detail) {
    Lane *l = user;
    pthread_mutex_lock(&l->pair->mu);
    snprintf(l->pstage, sizeof l->pstage, "%s", stage); snprintf(l->detail, sizeof l->detail, "%s", detail);
    l->done = done; l->total = total; l->dirty = 1;
    pthread_cond_signal(&l->pair->cv);
    pthread_mutex_unlock(&l->pair->mu);
}
static void *lane_main(void *arg) {
    Lane *l = arg;
    int rc = stage_fns[l->stage](&l->u);
    if (rc == -1 && !l->c.failed) setup_fail(&l->c, "internal", "The %s stage failed without a message", stage_names[l->stage]);
    pthread_mutex_lock(&l->pair->mu);
    l->rc = rc; l->finished = 1;
    pthread_cond_signal(&l->pair->cv);
    pthread_mutex_unlock(&l->pair->mu);
    return NULL;
}
/* 0 when both outcomes are recorded and the run goes on; -1 with *failed_stage and c's failure otherwise (the shader
   stage failed, a cancel, or install.json could not be written). */
static int run_companion_and_shaders(Run *u, int *failed_stage) {
    SetupCtx *c = u->c;
    Pair *p = calloc(1, sizeof *p);
    if (!p) { *failed_stage = ST_COMPANION; setup_fail(c, "internal", "Out of memory"); return -1; }
    pthread_mutex_init(&p->mu, NULL); pthread_cond_init(&p->cv, NULL);
    const int stages[2] = { ST_COMPANION, ST_SHADERS };
    int fatal = -1; char fatal_code[32] = "", fatal_msg[1024] = "";
    for (int k = 0; k < 2; k++) {
        Lane *l = &p->lane[k];
        l->pair = p; l->stage = stages[k];
        l->c = *c; setup_clear_fail(&l->c); l->c.progress = lane_progress; l->c.progress_user = l;
        l->u.c = &l->c; l->u.r = u->r; l->u.hooks = u->hooks; l->u.st = u->st;
        l->started = !pthread_create(&l->thread, NULL, lane_main, l);
        if (!l->started) { setup_fail(&l->c, "internal", "Could not start the %s stage", stage_names[l->stage]); l->rc = -1; l->finished = 1; }
    }
    pthread_mutex_lock(&p->mu);
    for (;;) {
        int busy = 0, did = 0;
        for (int k = 0; k < 2; k++) {
            Lane *l = &p->lane[k];
            if (l->dirty) {
                char stage[16], detail[256]; uint64_t done = l->done, total = l->total;
                memcpy(stage, l->pstage, sizeof stage); memcpy(detail, l->detail, sizeof detail); l->dirty = 0;
                pthread_mutex_unlock(&p->mu);
                progress_now(c, stage, done, total, detail);
                pthread_mutex_lock(&p->mu); did = 1;
            }
            if (l->finished && !l->recorded) {
                l->recorded = 1;
                pthread_mutex_unlock(&p->mu);
                if (l->started) pthread_join(l->thread, NULL);
                if (l->stage == ST_SHADERS) u->st.variants = l->u.st.variants;
                /* When both stop, the result names the shader stage: that is the one the game needs. */
                if (stage_outcome(u, &l->c, l->stage, l->rc) && (fatal < 0 || l->stage == ST_SHADERS)) {
                    fatal = l->stage; snprintf(fatal_code, sizeof fatal_code, "%s", c->fail_code); snprintf(fatal_msg, sizeof fatal_msg, "%s", c->fail_msg);
                }
                setup_clear_fail(c);
                pthread_mutex_lock(&p->mu); did = 1;
            }
            busy |= !l->recorded || l->dirty;
        }
        if (!busy) break;
        if (!did) pthread_cond_wait(&p->cv, &p->mu);
    }
    pthread_mutex_unlock(&p->mu);
    pthread_mutex_destroy(&p->mu); pthread_cond_destroy(&p->cv);
    free(p);
    if (fatal < 0) return 0;
    setup_fail(c, fatal_code[0] ? fatal_code : "internal", "%s", fatal_msg);
    *failed_stage = fatal;
    return -1;
}

/* Leaves only *.log files in staging (they explain a failure); everything else there is partial output. */
static void staging_cleanup(SetupCtx *c) {
    DIR *d = opendir(c->staging);
    if (!d) return;
    for (struct dirent *e; (e = readdir(d));) {
        size_t n = strlen(e->d_name); char p[SETUP_PATH];
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || (n > 4 && !strcmp(e->d_name + n - 4, ".log"))) continue;
        if (!st_path(p, sizeof p, "%s/%s", c->staging, e->d_name)) st_rmtree(p);
    }
    closedir(d);
}
static void keys_cleanup(SetupCtx *c) { if (c->keydir[0]) st_rmtree(c->keydir); }
/* The merge pipe is outside .import: reclaim pipes left by a killed process only while setup.lock is held.
   Do not follow links or remove shader scratch, regular files, or other users of setup-work. */
static int romfs_fifo_cleanup(SetupCtx *c) {
    char dir[SETUP_PATH];
    if (!c->nobackup[0]) return 0;
    if (st_path(dir, sizeof dir, "%s/setup-work", c->nobackup)) return st_io_fail(c, "open", "setup-work");
    DIR *d = opendir(dir);
    if (!d) return errno == ENOENT ? 0 : st_io_fail(c, "open", dir);
    int rc = 0;
    for (;;) {
        errno = 0;
        struct dirent *e = readdir(d);
        if (!e) { if (errno) rc = st_io_fail(c, "read", dir); break; }
        size_t n = strlen(e->d_name);
        if (n <= 11 || strncmp(e->d_name, "romfs-", 6) || strcmp(e->d_name + n - 5, ".fifo")) continue;
        char path[SETUP_PATH]; struct stat st;
        if (st_path(path, sizeof path, "%s/%s", dir, e->d_name)) { rc = st_io_fail(c, "remove", e->d_name); break; }
        if (lstat(path, &st)) {
            if (errno == ENOENT) continue;
            rc = st_io_fail(c, "stat", path); break;
        }
        if (S_ISFIFO(st.st_mode) && unlink(path) && errno != ENOENT) { rc = st_io_fail(c, "remove", path); break; }
    }
    closedir(d);
    return rc;
}

static char *result_fail(const char *stage, const char *code, const char *msg) {
    Buf b = {0};
    buf_puts(&b, "{\"ok\":false,\"stage\":"); buf_jstr(&b, stage); buf_puts(&b, ",\"code\":"); buf_jstr(&b, code);
    buf_puts(&b, ",\"message\":"); buf_jstr(&b, msg); buf_puts(&b, "}");
    return b.oom ? (buf_free(&b), strdup("{\"ok\":false,\"stage\":\"setup\",\"code\":\"internal\",\"message\":\"out of memory\"}")) : b.p;
}
static char *result_ok(const SetupInstall *s) {
    Buf b = {0};
    buf_puts(&b, "{\"ok\":true,\"install\":"); setup_install_json(&b, s); buf_puts(&b, "}");
    return b.oom ? (buf_free(&b), result_fail("setup", "internal", "out of memory")) : b.p;
}

/* Bytes a stage may add while its old outputs still exist. The 17 GB RomFS dominates; the container route also keeps a
   copy of the update Program NCA in staging for hactool. */
static uint64_t stage_need(const Run *u, int stage) {
    const SetupRequest *r = u->r;
    switch (stage) {
    case ST_CODE: return 768 * MIB;
    case ST_ROMFS: return u->c->pins.romfs_size + (use_decrypted_romfs(r) ? 0 : (r->update.size < 4096 * MIB ? r->update.size : 4096 * MIB));
    case ST_INDEX: return 64 * MIB;
    case ST_FONTS: { uint64_t n = 0; for (int i = 0; i < 6 && r->nfonts; i++) n += r->fonts[i < r->nfonts ? i : r->nfonts - 1].size + 8; return n + MIB; }
    default: return 0;
    }
}

char *setup_run_json(const char *request, size_t len, SetupProgressFn progress, void *user, atomic_int *cancel, const SetupPins *pins,
                     const SetupRunHooks *hooks) {
    SetupRequest *r = calloc(1, sizeof *r);
    Run *u = calloc(1, sizeof *u);
    SetupCtx *c = calloc(1, sizeof *c);
    char err[256], *res = NULL; int lock = -1, failed_stage = -1;
    if (!r || !u || !c) { free(r); free(u); free(c); return result_fail("setup", "internal", "out of memory"); }
    if (setup_request_parse(r, request, len, err, sizeof err)) { res = result_fail("setup", "internal", err); goto done; }
    setup_ctx_init(c, r->game, r->lib, r->nobackup, cancel);
    if (pins) c->pins = *pins;
    c->progress = progress; c->progress_user = user; c->req = r;
    u->c = c; u->r = r; u->hooks = hooks;
    memset(&throttle, 0, sizeof throttle);
    if (st_mkdirs(r->game) || st_mkdirs(r->nobackup)) { st_io_fail(c, "create", r->game); goto fail; }
    char lockpath[SETUP_PATH];
    if (st_path(lockpath, sizeof lockpath, "%s/setup.lock", r->nobackup) || (lock = open(lockpath, O_RDWR | O_CREAT | O_CLOEXEC, 0600)) < 0) { st_io_fail(c, "open", "setup.lock"); goto fail; }
    if (flock(lock, LOCK_EX | LOCK_NB)) { setup_fail(c, "internal", "Setup is already running"); goto fail; }
    /* Stale key files of an interrupted run go first; the directory exists only while hactool needs it. */
    keys_cleanup(c);
    if (romfs_fifo_cleanup(c)) goto fail;
    if (mkdir(c->keydir, 0700) && errno != EEXIST) { st_io_fail(c, "create", "import-keys"); goto fail; }
    setup_install_load(c->game, &u->st);
    int changed = 0;
    for (int i = 0; i < ST_COUNT; i++) if (u->st.done[i] && !outputs_present(c, &u->st, i)) { u->st.done[i] = 0; changed = 1; }
    int pending[ST_COUNT] = {0}, any = 0;
    for (int i = 0; i < ST_COUNT; i++) { pending[i] = (r->stage_mask >> i & 1) && (r->stages_explicit || !u->st.done[i]); any |= pending[i]; }
    if (r->skip_companion && !r->stages_explicit) {
        pending[ST_COMPANION] = 0;
        if (!u->st.done[ST_COMPANION]) {
            u->st.companion_skipped = 1;
            u->st.opt_code[ST_COMPANION][0] = u->st.opt_error[ST_COMPANION][0] = 0;
            changed = 1;
            progress_now(c, "companion", 0, 0, "skipped");
        }
        any = 0;
        for (int i = 0; i < ST_COUNT; i++) any |= pending[i];
    }
    /* Fail before writing anything when a pending core stage has no usable input. */
    if (pending[ST_CODE] && !use_decrypted_code(r) && !(r->update.present && r->keys.present)) { failed_stage = ST_CODE;
        setup_fail(c, "missing_input", "Select the 1.4.3 ExeFS (main, sdk, subsdk0), or the 1.4.3 update NSP with prod.keys"); goto fail; }
    if (pending[ST_ROMFS] && !use_decrypted_romfs(r) && !(r->base.present && r->update.present && r->keys.present)) { failed_stage = ST_ROMFS;
        setup_fail(c, "missing_input", "Select the 1.4.3 RomFS image, or the base game, the 1.4.3 update and prod.keys"); goto fail; }
    if (pending[ST_FONTS] && !r->nfonts) { failed_stage = ST_FONTS; setup_fail(c, "missing_input", "No bundled font was passed to setup"); goto fail; }
    if (!any) {
        char p[SETUP_PATH];
        if (changed || (!st_path(p, sizeof p, "%s/install.json", c->game) && !st_exists(p))) setup_install_write(c, &u->st);
        res = result_ok(&u->st); goto done;
    }
    if (st_rmtree(c->staging) || st_mkdirs(c->staging)) { st_io_fail(c, "prepare", ".import"); goto fail; }
    uint64_t need = 64 * MIB, have = st_free_bytes(c->game);
    for (int i = 0; i < ST_COUNT; i++) if (pending[i]) need += stage_need(u, i);
    if (have < need) {
        setup_fail(c, "no_space", "Setup needs %.1f GB of free storage; %.1f GB is available. Free some space and try again.", need / 1e9, have / 1e9);
        goto fail;
    }
    if (changed) setup_install_write(c, &u->st);
    for (int i = 0; i < ST_COUNT; i++) {
        if (!pending[i]) continue;
        if (setup_cancelled(c)) { failed_stage = i; setup_fail(c, "cancelled", "Setup was cancelled"); goto fail; }
        setup_clear_fail(c);
        if (i == ST_COMPANION && pending[ST_SHADERS]) {
            if (run_companion_and_shaders(u, &failed_stage)) goto fail;
            break;   /* shaders ran too */
        }
        if (stage_outcome(u, c, i, stage_fns[i](u))) { failed_stage = i; goto fail; }
    }
    st_rmtree(c->staging);
    res = result_ok(&u->st);
    goto done;
fail:
    staging_cleanup(c);
    if (!c->failed) setup_fail(c, "internal", "Setup failed");
    res = result_fail(failed_stage >= 0 ? stage_names[failed_stage] : "setup", c->fail_code, c->fail_msg);
done:
    containers_free(&u->sc);
    if (c->keydir[0]) keys_cleanup(c);
    if (lock >= 0) close(lock);
    setup_request_close(r);
    free(r); free(u); free(c);
    return res;
}

/* ---- inspect: identify the picked inputs without writing anything ---- */
static void item(Buf *b, int *first, const char *role, int ok, const char *what, const char *problem) {
    buf_puts(b, *first ? "{\"role\":" : ",{\"role\":"); *first = 0;
    buf_jstr(b, role); buf_printf(b, ",\"ok\":%s,\"what\":", ok ? "true" : "false"); buf_jstr(b, what);
    buf_puts(b, ",\"problem\":"); if (problem) buf_jstr(b, problem); else buf_puts(b, "null");
    buf_puts(b, "}");
}
char *setup_inspect_json(const char *request, size_t len, const SetupPins *pins) {
    SetupRequest *r = calloc(1, sizeof *r); SetupCtx *c = calloc(1, sizeof *c); SetupContainers *sc = calloc(1, sizeof *sc);
    char err[256]; Buf b = {0}; int first = 1;
    if (!r || !c || !sc) { free(r); free(c); free(sc); return result_fail("inspect", "internal", "out of memory"); }
    if (setup_request_parse(r, request, len, err, sizeof err)) { free(r); free(c); free(sc); return result_fail("inspect", "internal", err); }
    setup_ctx_init(c, r->game, r->lib, r->nobackup, NULL);
    if (pins) c->pins = *pins;
    SetupInstall st; setup_install_load(c->game, &st);
    for (int i = 0; i < ST_COUNT; i++) if (st.done[i] && !outputs_present(c, &st, i)) st.done[i] = 0;
    buf_puts(&b, "{\"ok\":true,\"items\":[");
    char what[512];
    if (r->nromfs) {
        uint64_t total = 0; uint8_t h[8] = {1};
        for (int i = 0; i < r->nromfs; i++) total += r->romfs[i].size;
        int header = r->romfs[0].size >= 8 && !st_pread(r->romfs[0].fd, h, 8, r->romfs[0].offset) && h[0] == 0x50 && !h[1] && !h[2] && !h[3] && !h[4] && !h[5] && !h[6] && !h[7];
        /* The same size rule as the menu's Strings.bytes (decimal GB, one decimal), so the tick label and the card's
           sub-line show the same number. */
        snprintf(what, sizeof what, "RomFS image (%d part%s, %.1f GB)", r->nromfs, r->nromfs > 1 ? "s" : "", total / 1e9);
        item(&b, &first, "romfs", header && total == c->pins.romfs_size, what,
             !header ? "Not a raw RomFS image (pick the decrypted RomFS, first part first)" :
             total != c->pins.romfs_size ? "Size differs from the 1.4.3 RomFS (wrong version, update not applied, or parts missing)" : NULL);
    }
    if (r->nexefs) {
        int found = 0; char problem[512] = "";
        uint8_t m[4] = {0};
        if (r->nexefs == 1 && r->exefs[0].size >= 16 && !st_pread(r->exefs[0].fd, m, 4, r->exefs[0].offset) && !memcmp(m, "PFS0", 4)) {
            PEntry *e; int n;
            if (r->exefs[0].offset || part_parse(c, r->exefs[0].fd, 0, r->exefs[0].size, "PFS0", 24, &e, &n)) snprintf(problem, sizeof problem, "Not a valid ExeFS image");
            else {
                for (int i = 0; i < n; i++) for (int k = 0; k < EXEFS_COUNT; k++) if (!strcmp(e[i].name, setup_exefs_names[k]) && e[i].size <= (512ull << 20)) {
                    uint8_t d[32]; char hex[65];
                    if (!st_sha256_fd(c, r->exefs[0].fd, e[i].off, e[i].size, d, NULL, 0, 0)) { sha256_hex(d, hex); if (!strcmp(hex, c->pins.exefs[k])) found |= 1 << k; else snprintf(problem, sizeof problem, "The ExeFS is not the 1.4.3 version"); }
                }
                free(e);
            }
        } else {
            for (int i = 0; i < r->nexefs; i++) {
                uint8_t d[32]; char hex[65]; int k = -1;
                if (r->exefs[i].size <= (512ull << 20) && !st_sha256_fd(c, r->exefs[i].fd, r->exefs[i].offset, r->exefs[i].size, d, NULL, 0, 0)) {
                    sha256_hex(d, hex);
                    for (int j = 0; j < EXEFS_COUNT; j++) if (!strcmp(hex, c->pins.exefs[j])) k = j;
                }
                if (k < 0) { if (!problem[0]) snprintf(problem, sizeof problem, "%s is not a 1.4.3 ExeFS file", r->exefs[i].name); }
                else found |= 1 << k;
            }
        }
        int complete = (found & 7) == 7;
        snprintf(what, sizeof what, "ExeFS:%s%s%s", found & 1 ? " main" : "", found & 2 ? " sdk" : "", found & 4 ? " subsdk0" : "");
        item(&b, &first, "exefs", complete && !problem[0], found ? what : "ExeFS", problem[0] ? problem : complete ? NULL : "Select main, sdk and subsdk0 of 1.4.3");
    }
    int keys_ok = 0;
    if (r->keys.present) {
        setup_clear_fail(c);
        keys_ok = !keys_load_fd(c, &r->keys, &sc->keys);
        int gens = 0; for (int g = 0; g < KEY_GENERATIONS; g++) gens += (int)((sc->keys.kak_mask | sc->keys.titlekek_mask) >> g & 1);
        snprintf(what, sizeof what, keys_ok ? "prod.keys (header key, %d key generations)" : "prod.keys", gens);
        item(&b, &first, "keys", keys_ok, what, keys_ok ? NULL : c->fail_msg);
        sc->have_keys = keys_ok;
    }
    const SetupInput *pick[2] = { &r->update, &r->base }; const char *role[2] = { "update", "base" };
    for (int i = 0; i < 2; i++) {
        if (!pick[i]->present) continue;
        Container *k = i ? &sc->base : &sc->update;
        setup_clear_fail(c);
        int ok = !container_open(c, pick[i], k, role[i]);
        const char *kind = ok ? (k->is_xci ? "XCI" : "NSP") : "file";
        if (ok && !keys_ok) { snprintf(what, sizeof what, "%s game file (prod.keys needed to identify it)", kind); item(&b, &first, role[i], 1, what, NULL); continue; }
        if (ok) ok = !container_inventory(c, k, &sc->keys) && !container_select(c, k, i == 0);
        if (ok) {
            Ticket *all = NULL; int n = 0;
            for (int s = 0; s < 2; s++) { Container *o = s ? &sc->base : &sc->update; Ticket *t = realloc(all, (size_t)(n + o->ntickets + 1) * sizeof *t);
                if (t) { all = t; memcpy(all + n, o->tickets, (size_t)o->ntickets * sizeof *t); n += o->ntickets; } }
            ok = !container_check_key(c, k, &sc->keys, all, n);
            if (all) memset(all, 0, (size_t)n * sizeof *all);
            free(all);
        }
        snprintf(what, sizeof what, i ? "Tears of the Kingdom base game (%s)" : "Tears of the Kingdom update (%s; version checked during setup)", kind);
        item(&b, &first, role[i], ok, ok ? what : kind, ok ? NULL : c->fail_msg);
    }
    /* What is still needed, given the committed stages. */
    int need_code = !st.done[ST_CODE], need_romfs = !st.done[ST_ROMFS];
    const char *route = r->nromfs || r->nexefs ? "decrypted" : r->base.present || r->update.present || r->keys.present ? "containers" : NULL;
    buf_puts(&b, "],\"route\":"); if (route) buf_jstr(&b, route); else buf_puts(&b, "null");
    buf_puts(&b, ",\"need\":[");
    int nf = 1;
#define NEED(x) do { buf_puts(&b, nf ? "\"" x "\"" : ",\"" x "\""); nf = 0; } while (0)
    if (route && !strcmp(route, "decrypted")) { if (need_romfs && !r->nromfs) NEED("romfs"); if (need_code && !r->nexefs) NEED("exefs"); }
    else if (need_code || need_romfs) {
        if (need_romfs && !r->base.present) NEED("base");
        if (!r->update.present) NEED("update");
        if (!r->keys.present) NEED("keys");
    }
#undef NEED
    buf_puts(&b, "]}");
    containers_free(sc);
    setup_request_close(r);
    free(r); free(c); free(sc);
    if (b.oom) { buf_free(&b); return result_fail("inspect", "internal", "out of memory"); }
    return b.p;
}
