/* Container route: base 1.0.0 XCI/NSP + 1.4.3 update NSP + prod.keys. The importer identifies the Program NCAs itself
   (container.c) and hands the heavy work to the pinned hactool, as scripts/extract_update_romfs.py does: the update's
   ExeFS (--exefsdir) and the base + update BKTR RomFS (--basenca with the bounded base range, --sectionN). Key files for
   hactool exist only while it runs, mode 0600 in <nobackup>/import-keys. */
#define _GNU_SOURCE 1   /* F_SETPIPE_SZ */
#include "setup_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BASE_CHILD_FD 3

int containers_prepare(SetupCtx *c, const SetupRequest *r, SetupContainers *s, int need_base) {
    if (s->have_update && (!need_base || s->have_base)) return 0;
    setup_progress(c, "inspect", 0, 0, "reading the game files");
    if (!s->have_keys) { if (keys_load_fd(c, &r->keys, &s->keys)) return -1; s->have_keys = 1; }
    if (!s->have_update) {
        if (container_open(c, &r->update, &s->update, "update") || container_inventory(c, &s->update, &s->keys) ||
            container_select(c, &s->update, 1)) return -1;
        s->have_update = 1;
    }
    if (need_base && !s->have_base) {
        if (container_open(c, &r->base, &s->base, "base game") || container_inventory(c, &s->base, &s->keys) ||
            container_select(c, &s->base, 0)) return -1;
        s->have_base = 1;
    }
    /* One title-key list for hactool: tickets from both packages, which must agree. */
    free(s->tickets); s->tickets = NULL; s->ntickets = 0;
    Container *both[2] = { &s->update, s->have_base ? &s->base : NULL };
    for (int b = 0; b < 2; b++) for (int i = 0; both[b] && i < both[b]->ntickets; i++) {
        const Ticket *t = &both[b]->tickets[i]; int dup = 0;
        for (int j = 0; j < s->ntickets; j++) if (!memcmp(s->tickets[j].rights, t->rights, 16)) {
            if (memcmp(s->tickets[j].key, t->key, 16)) { setup_fail(c, "missing_input", "The base game and update carry conflicting common tickets"); return -1; }
            dup = 1;
        }
        if (dup) continue;
        Ticket *n = realloc(s->tickets, (size_t)(s->ntickets + 1) * sizeof *n);
        if (!n) { setup_fail(c, "internal", "Out of memory"); return -1; }
        s->tickets = n; s->tickets[s->ntickets++] = *t;
    }
    if (container_check_key(c, &s->update, &s->keys, s->tickets, s->ntickets)) return -1;
    if (s->have_base && container_check_key(c, &s->base, &s->keys, s->tickets, s->ntickets)) return -1;
    return 0;
}

void containers_free(SetupContainers *s) {
    keys_wipe(&s->keys);
    container_free(&s->base); container_free(&s->update);
    if (s->tickets) memset(s->tickets, 0, (size_t)s->ntickets * sizeof *s->tickets);
    free(s->tickets);
    if (s->update_nca_ready) unlink(s->update_nca);
    memset(s, 0, sizeof *s);
}

/* hactool reads the update Program NCA as a plain file: copy just that NCA (hundreds of MB, not the whole NSP). */
static int update_nca(SetupCtx *c, SetupContainers *s, const char *stage) {
    if (s->update_nca_ready) return 0;
    const NcaRecord *r = &s->update.ncas[s->update.program];
    if (st_path(s->update_nca, sizeof s->update_nca, "%s/update.nca", c->staging)) return st_io_fail(c, "create", "update.nca");
    int out = open(s->update_nca, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (out < 0) return st_io_fail(c, "create", s->update_nca);
    size_t cap = 8u << 20; uint8_t *buf = malloc(cap); int rc = 0; StCache cache = {0, 0, 0};
    if (!buf) { close(out); setup_fail(c, "internal", "Out of memory"); return -1; }
    for (uint64_t done = 0; done < r->size && !rc;) {
        if (setup_cancelled(c)) { rc = SETUP_CANCELLED; break; }
        size_t n = r->size - done < cap ? (size_t)(r->size - done) : cap;
        if (st_pread(s->update.fd, buf, n, r->off + done)) rc = st_io_fail(c, "read", "the update");
        else if (st_write(out, buf, n)) rc = st_io_fail(c, "write", s->update_nca);
        else { st_cache_drop(s->update.fd, r->off + done, n); st_cache_written(out, &cache, done + n); }
        done += n;
        setup_progress(c, stage, done, r->size, "copying the update program");
    }
    free(buf);
    if (close(out) && !rc) rc = st_io_fail(c, "write", s->update_nca);
    if (rc) { unlink(s->update_nca); return rc; }
    s->update_nca_ready = 1;
    return 0;
}

static int write_title_keys(SetupCtx *c, const SetupContainers *s, const char *path) {
    static const char x[] = "0123456789abcdef";
    size_t cap = (size_t)s->ntickets * 70 + 1; char *text = calloc(1, cap); size_t n = 0;
    if (!text) { setup_fail(c, "internal", "Out of memory"); return -1; }
    for (int i = 0; i < s->ntickets; i++) {
        for (int j = 0; j < 16; j++) { text[n++] = x[s->tickets[i].rights[j] >> 4]; text[n++] = x[s->tickets[i].rights[j] & 15]; }
        memcpy(text + n, " = ", 3); n += 3;
        for (int j = 0; j < 16; j++) { text[n++] = x[s->tickets[i].key[j] >> 4]; text[n++] = x[s->tickets[i].key[j] & 15]; }
        text[n++] = '\n';
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600), rc = 0;
    if (fd < 0) rc = st_io_fail(c, "open the temporary title-key file", path);
    else {
        if (st_write(fd, text, n)) rc = st_io_fail(c, "write the temporary title-key file", path);
        if (close(fd) && !rc) rc = st_io_fail(c, "close the temporary title-key file", path);
    }
    if (rc) unlink(path);
    memset(text, 0, cap); free(text);
    return rc;
}

/* Writes both key files, runs hactool, and removes the key files whatever happened. */
static int run_with_keys(SetupCtx *c, SetupContainers *s, const char *stage, const char **args, int nargs_fixed, const SetupFdMap *fds, int nfds,
                         const char **env, int nenv, const char *log_name, uint64_t total, SetupFollow *follow,
                         char **log, int *exit_code) {
    char keyfile[SETUP_PATH], titlefile[SETUP_PATH], log_path[SETUP_PATH], t_env[SETUP_PATH + 16], home[SETUP_PATH + 8], xdg[SETUP_PATH + 24];
    if (st_path(keyfile, sizeof keyfile, "%s/hactool.keys", c->keydir) || st_path(titlefile, sizeof titlefile, "%s/title.keys", c->keydir) ||
        st_path(log_path, sizeof log_path, "%s/%s", c->staging, log_name) || st_path(t_env, sizeof t_env, "MNG_TITLE_KEYS=%s", titlefile) ||
        st_path(home, sizeof home, "HOME=%s", c->keydir) || st_path(xdg, sizeof xdg, "XDG_CONFIG_HOME=%s", c->keydir))
        return st_io_fail(c, "create", "a temporary key file");
    unlink(keyfile); unlink(titlefile);
    int rc = keys_write_hactool(c, &s->keys, keyfile);
    if (!rc) rc = write_title_keys(c, s, titlefile);
    if (!rc) {
        const char *argv[24]; int n = 0;
        argv[n++] = "-x"; argv[n++] = "-y"; argv[n++] = "--disablekeywarns"; argv[n++] = "--suppresskeys";
        argv[n++] = "-k"; argv[n++] = keyfile;
        for (int i = 0; i < nargs_fixed && n < 22; i++) argv[n++] = args[i];
        argv[n] = NULL;
        /* hactool also merges $HOME/.switch/prod.keys: point it at the key directory so only our file is read. */
        const char *envp[8]; int m = 0;
        envp[m++] = t_env; envp[m++] = home; envp[m++] = xdg;
        for (int i = 0; i < nenv && m < 7; i++) envp[m++] = env[i];
        envp[m] = NULL;
        rc = hactool_run(c, stage, argv, fds, nfds, envp, log_path, total, follow, log, exit_code);
    }
    unlink(keyfile); unlink(titlefile);
    return rc;
}

int containers_exefs(SetupCtx *c, SetupContainers *s, const char *out_dir) {
    int rc = update_nca(c, s, "exefs");
    if (rc) return rc;
    if (st_rmtree(out_dir) || st_mkdirs(out_dir)) return st_io_fail(c, "create", out_dir);
    char dir_arg[SETUP_PATH + 16];
    if (st_path(dir_arg, sizeof dir_arg, "--exefsdir=%s", out_dir)) return st_io_fail(c, "create", out_dir);
    const char *args[] = { dir_arg, s->update_nca };
    char *log = NULL; int code = -1;
    setup_progress(c, "exefs", 0, 0, "extracting the update ExeFS");
    rc = run_with_keys(c, s, "exefs", args, 2, NULL, 0, NULL, 0, "hactool-exefs.log", 0, NULL, &log, &code);
    if (!rc && (code || !hactool_log_ok(log, 0))) {
        setup_fail(c, "internal", "hactool could not extract the update ExeFS (exit %d; use Share details for the log)", code);
        rc = -1;
    }
    free(log);
    return rc;
}

/* hactool writes the merged image into an app-private FIFO that the follow loop drains into a RomfsWriter (one
   romfs.bin, or parts where a FAT volume cannot hold the 17 GB file). Shared storage, including exFAT and some OEM
   FUSE mounts, cannot create FIFOs. hactool writes sequentially, so the output itself stays on the game volume. */
int containers_romfs(SetupCtx *c, SetupContainers *s, const char *out_dir, uint64_t part_bytes, char sha_hex[65]) {
    int rc = update_nca(c, s, "romfs");
    if (rc) return rc;
    const NcaRecord *base = &s->base.ncas[s->base.program];
    char fifo_dir[SETUP_PATH], fifo_path[SETUP_PATH], sec_arg[SETUP_PATH + 16], off_env[64], size_env[64];
    RomfsWriter w; int fifo = -1, temporary_dir = !c->nobackup[0];
    SetupFollow follow = { .pipe = -1, .sink = &w };
    if (temporary_dir) {
        if (st_path(fifo_dir, sizeof fifo_dir, "/tmp/totk-romfs-XXXXXX")) return st_io_fail(c, "create", "the FIFO directory");
        if (!mkdtemp(fifo_dir)) return st_io_fail(c, "create", "the FIFO directory");
    } else if (st_path(fifo_dir, sizeof fifo_dir, "%s/setup-work", c->nobackup) || st_mkdirs(fifo_dir))
        return st_io_fail(c, "create", "the FIFO directory");
    if (st_path(fifo_path, sizeof fifo_path, "%s/romfs-%ld.fifo", fifo_dir, (long)getpid())) {
        rc = st_io_fail(c, "create", "romfs.fifo"); goto directory_out;
    }
    if (unlink(fifo_path) && errno != ENOENT) { rc = st_io_fail(c, "remove", fifo_path); goto directory_out; }
    if (romfs_writer_open(c, &w, out_dir, c->pins.romfs_size, part_bytes)) { rc = -1; goto directory_out; }
    /* st_io_fail snapshots errno before any close, unlink or writer-thread cleanup can change it. */
    if (mkfifo(fifo_path, 0600)) { rc = st_io_fail(c, "mkfifo", fifo_path); goto out; }
    if ((fifo = open(fifo_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC)) < 0) { rc = st_io_fail(c, "open", fifo_path); goto out; }
    fcntl(fifo, F_SETPIPE_SZ, 1 << 20);   /* fewer wake-ups per GB; the default 64 KiB also works */
    follow.pipe = fifo;
    if (st_path(sec_arg, sizeof sec_arg, "--section%d=%s", s->update.bktr_section, fifo_path)) { rc = st_io_fail(c, "create", fifo_path); goto out; }
    snprintf(off_env, sizeof off_env, "MNG_BASE_RANGE_OFFSET=%llu", (unsigned long long)base->off);
    snprintf(size_env, sizeof size_env, "MNG_BASE_RANGE_SIZE=%llu", (unsigned long long)base->size);
    char base_arg[64]; snprintf(base_arg, sizeof base_arg, "--basenca=/proc/self/fd/%d", BASE_CHILD_FD);
    const char *args[] = { base_arg, sec_arg, s->update_nca };
    const char *env[] = { off_env, size_env };
    SetupFdMap fds[] = { { s->base.fd, BASE_CHILD_FD } };
    char *log = NULL; int code = -1;
    setup_progress(c, "romfs", 0, c->pins.romfs_size, "merging base + update");
    rc = run_with_keys(c, s, "romfs", args, 3, fds, 1, env, 2, "hactool-romfs.log", c->pins.romfs_size, &follow, &log, &code);
    if (!rc && (code || !hactool_log_ok(log, 1))) {
        setup_fail(c, "internal", "The base + update merge did not verify (exit %d; use Share details for the log)", code);
        rc = -1;
    }
    free(log);
    if (rc) goto out;
    if (follow.bytes != c->pins.romfs_size) {
        setup_fail(c, "hash_mismatch", "The merged RomFS has %llu bytes, 1.4.3 has %llu: the base game or update is not the supported version",
                   (unsigned long long)follow.bytes, (unsigned long long)c->pins.romfs_size);
        rc = -1; goto out;
    }
    if ((rc = romfs_writer_finish(c, &w, sha_hex))) goto out;   /* every byte written was hashed: definitive */
    if (strcmp(sha_hex, c->pins.romfs_sha256)) {
        setup_fail(c, "hash_mismatch", "The merged RomFS does not match 1.4.3 (SHA-256 differs): use the 1.0.0 base game and the 1.4.3 update");
        rc = -1;
    }
out:
    close(fifo); unlink(fifo_path);
    if (rc) romfs_writer_abort(&w);
directory_out:
    if (temporary_dir) rmdir(fifo_dir);
    return rc;
}
