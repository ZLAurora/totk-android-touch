/* Moving the installed game data to another volume (NativeSetup.move): everything setup made in <from> (rewritten/,
   extracted/, game-data/, shaders/, companion/, driver.so) is copied into <to>/.move, read back and compared with what was
   read from <from> (the RomFS against its pin, re-cut into the layout the destination needs), fsync'd, renamed into place
   and committed by writing install.json last, with a fresh RomFS verification record for the new files. <from> is only
   read: a failure or a cancellation at any point removes what was put into <to> and leaves the old copy as it was, so
   the app switches its data folder only after {"ok":true} and then removes the old copy (setup_remove_json). While the
   move runs it holds the game lock (<private>/state/game.lock), so the game cannot start from either copy half-way. */
#define _GNU_SOURCE 1
#include "setup_internal.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The game data of one folder; install.json is handled on its own (written last, removed first). Logs, .import staging
   and the runtime's own per-folder state are not game data. */
static const char *const entries[] = { "rewritten", "extracted", "game-data", "shaders", "companion", "driver.so" };
#define NENTRIES (int)(sizeof entries / sizeof *entries)
#define MIB (1ull << 20)
#define BUF (8u << 20)

typedef struct { char *rel; uint64_t size; int dir; char sha[65]; } Item;
typedef struct {
    SetupCtx *c; const SetupMoveHooks *hooks;
    char from[SETUP_PATH], to[SETUP_PATH], stage[SETUP_PATH];
    Item *items; size_t n, cap;
    uint64_t files_bytes, done, total;
    uint8_t *buf;
    int committed[NENTRIES];
} Move;

static void phase(Move *m, const char *name) { if (m->hooks && m->hooks->phase) m->hooks->phase(m->hooks->user, name, m->stage); }
static int cancelled(Move *m) { return setup_cancelled(m->c) ? SETUP_CANCELLED : 0; }
static int image_name(const char *rel) {   /* the RomFS image files, which the image copy handles */
    size_t n = strlen(SETUP_ROMFS_DIR);
    if (strncmp(rel, SETUP_ROMFS_DIR "/", n + 1)) return 0;
    const char *b = rel + n + 1;
    return !strcmp(b, "romfs.bin") || (!strncmp(b, "romfs.bin.", 10) && strlen(b) == 13 && strspn(b + 10, "0123456789") == 3);
}
static int add(Move *m, const char *rel, uint64_t size, int dir) {
    if (m->n == m->cap) {
        Item *p = realloc(m->items, (m->cap = m->cap ? m->cap * 2 : 256) * sizeof *p);
        if (!p) { setup_fail(m->c, "internal", "Out of memory"); return -1; }
        m->items = p;
    }
    if (!(m->items[m->n].rel = strdup(rel))) { setup_fail(m->c, "internal", "Out of memory"); return -1; }
    m->items[m->n].size = size; m->items[m->n].dir = dir; m->items[m->n].sha[0] = 0; m->n++;
    if (!dir) m->files_bytes += size;
    return 0;
}
/* Every directory and regular file under <from>/rel, parents first. Anything else (links, devices) is not setup's. */
static int walk(Move *m, const char *rel) {
    char path[SETUP_PATH]; struct stat st;
    if (st_path(path, sizeof path, "%s/%s", m->from, rel)) return st_io_fail(m->c, "read", rel);
    if (lstat(path, &st)) return errno == ENOENT ? 0 : st_io_fail(m->c, "read", rel);
    if (S_ISREG(st.st_mode)) return image_name(rel) ? 0 : add(m, rel, (uint64_t)st.st_size, 0);
    if (!S_ISDIR(st.st_mode)) { setup_fail(m->c, "internal", "%s in the game data is neither a file nor a folder", rel); return -1; }
    if (add(m, rel, 0, 1)) return -1;
    DIR *d = opendir(path);
    if (!d) return st_io_fail(m->c, "read", rel);
    int rc = 0;
    for (struct dirent *e; !rc && (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char child[SETUP_PATH];
        rc = st_path(child, sizeof child, "%s/%s", rel, e->d_name) ? st_io_fail(m->c, "read", e->d_name) : walk(m, child);
    }
    closedir(d);
    return rc;
}
static void progress(Move *m, uint64_t add_done, const char *detail) { m->done += add_done; setup_progress(m->c, "move", m->done, m->total, detail); }

/* Copies one file, hashing what was read from the source. */
static int copy_file(Move *m, Item *it) {
    char src[SETUP_PATH], dst[SETUP_PATH], detail[SETUP_PATH + 16];
    if (st_path(src, sizeof src, "%s/%s", m->from, it->rel) || st_path(dst, sizeof dst, "%s/%s", m->stage, it->rel)) return st_io_fail(m->c, "copy", it->rel);
    snprintf(detail, sizeof detail, "copying %s", it->rel);
    int in = open(src, O_RDONLY | O_CLOEXEC), out = -1, rc = 0;
    if (in < 0) return st_io_fail(m->c, "open", src);
    struct stat st;
    if (fstat(in, &st) || (uint64_t)st.st_size != it->size) { close(in); setup_fail(m->c, "io", "%s changed while the game data was being moved", it->rel); return -1; }
    if ((out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)) < 0) { rc = st_io_fail(m->c, "create", dst); close(in); return rc; }
    Sha256 sha; sha256_init(&sha); StCache cache = {0, 0, 0};
    for (uint64_t pos = 0; pos < it->size && !rc;) {
        if ((rc = cancelled(m))) break;
        size_t n = it->size - pos < BUF ? (size_t)(it->size - pos) : BUF;
        if (st_pread(in, m->buf, n, pos)) { rc = st_io_fail(m->c, "read", src); break; }
        st_cache_drop(in, pos, n);
        sha256_update(&sha, m->buf, n);
        if (st_write(out, m->buf, n)) { rc = st_io_fail(m->c, "write", dst); break; }
        pos += n; st_cache_written(out, &cache, pos);
        progress(m, n, detail);
    }
    if (!rc && fsync(out)) rc = st_io_fail(m->c, "sync", dst);
    if (!rc) st_cache_drop(out, 0, 0);   /* the verification reads it back from the storage, not from the cache */
    if (close(out) && !rc) rc = st_io_fail(m->c, "write", dst);
    close(in);
    if (!rc) { uint8_t d[32]; sha256_final(&sha, d); sha256_hex(d, it->sha); }
    return rc;
}
/* The image in whichever layout <from> has, re-cut into the destination's (parts of part_bytes, 0 = romfs.bin). */
static int copy_image(Move *m, uint64_t part_bytes, char sha[65]) {
    RomfsImage img; RomfsWriter w; char src[SETUP_PATH], dst[SETUP_PATH]; int rc = 0;
    if (st_path(src, sizeof src, "%s/" SETUP_ROMFS_DIR, m->from) || st_path(dst, sizeof dst, "%s/" SETUP_ROMFS_DIR, m->stage)) return st_io_fail(m->c, "copy", "the RomFS");
    if (romfs_image_open(&img, src)) return st_io_fail(m->c, "open the RomFS image in", src);
    if (romfs_writer_open(m->c, &w, dst, img.size, part_bytes)) { romfs_image_close(&img); return -1; }
    for (uint64_t pos = 0; pos < img.size && !rc;) {
        if ((rc = cancelled(m))) break;
        uint8_t *buf; size_t room;
        if ((rc = romfs_writer_reserve(m->c, &w, &buf, &room))) break;
        size_t n = img.size - pos < room ? (size_t)(img.size - pos) : room;
        if (romfs_image_pread(&img, buf, n, pos)) { rc = st_io_fail(m->c, "read", "the RomFS image"); break; }
        romfs_image_drop(&img, pos, n);
        if ((rc = romfs_writer_commit(m->c, &w, n))) break;
        pos += n;
        progress(m, n, "copying the RomFS");
    }
    romfs_image_close(&img);
    if (!rc) rc = romfs_writer_finish(m->c, &w, sha);
    if (!rc && strcmp(sha, m->c->pins.romfs_sha256)) {
        setup_fail(m->c, "hash_mismatch", "The RomFS in the current game folder no longer matches 1.4.3 (SHA-256 differs); nothing was moved. Set the game up again.");
        rc = -1;
    }
    if (rc) romfs_writer_abort(&w);
    return rc;
}
/* Reads every copied byte back from the destination (its cache dropped) and compares it with what the source held. */
static int verify(Move *m) {
    for (size_t i = 0; i < m->n; i++) {
        Item *it = &m->items[i];
        if (it->dir) continue;
        int rc; char path[SETUP_PATH], hex[65]; int64_t size;
        if ((rc = cancelled(m))) return rc;
        if (st_path(path, sizeof path, "%s/%s", m->stage, it->rel)) return st_io_fail(m->c, "verify", it->rel);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) { st_cache_drop(fd, 0, 0); close(fd); }
        if ((rc = st_sha256_file(m->c, path, hex, &size))) return rc;
        if ((uint64_t)size != it->size || strcmp(hex, it->sha)) {
            setup_fail(m->c, "io", "The copy of %s on the new storage reads back differently; the storage may be failing. Nothing was moved.", it->rel);
            return -1;
        }
        progress(m, it->size, "verifying");
    }
    RomfsImage img; char dir[SETUP_PATH];
    if (st_path(dir, sizeof dir, "%s/" SETUP_ROMFS_DIR, m->stage) || romfs_image_open(&img, dir)) return st_io_fail(m->c, "verify", "the RomFS copy");
    Sha256 sha; sha256_init(&sha); int rc = 0;
    for (int p = 0; p < (img.nparts ? img.nparts : 1); p++) st_cache_drop(img.fd[p], 0, 0);
    for (uint64_t pos = 0; pos < img.size && !rc;) {
        if ((rc = cancelled(m))) break;
        size_t n = img.size - pos < BUF ? (size_t)(img.size - pos) : BUF;
        if (romfs_image_pread(&img, m->buf, n, pos)) { rc = st_io_fail(m->c, "read back", "the RomFS copy"); break; }
        romfs_image_drop(&img, pos, n);
        sha256_update(&sha, m->buf, n); pos += n;
        progress(m, n, "verifying the RomFS");
    }
    uint64_t size = img.size;
    romfs_image_close(&img);
    if (rc) return rc;
    uint8_t d[32]; char hex[65]; sha256_final(&sha, d); sha256_hex(d, hex);
    if (size != m->c->pins.romfs_size || strcmp(hex, m->c->pins.romfs_sha256)) {
        setup_fail(m->c, "io", "The RomFS copy on the new storage reads back differently; the storage may be failing. Nothing was moved.");
        return -1;
    }
    return 0;
}
/* The game data of dir, install.json first so that a folder half-way through removal is never playable. */
static int remove_game_data(const char *dir, int with_staging) {
    char p[SETUP_PATH]; int e = 0;
    if (st_path(p, sizeof p, "%s/install.json", dir) || (unlink(p) && errno != ENOENT)) e = errno;
    st_fsync_dir(dir);
    for (int i = 0; i < NENTRIES; i++)
        if (st_path(p, sizeof p, "%s/%s", dir, entries[i]) || st_rmtree(p)) { if (!e) e = errno; }
    if (with_staging) {
        if (st_path(p, sizeof p, "%s/.import", dir) || st_rmtree(p)) { if (!e) e = errno; }
        if (st_path(p, sizeof p, "%s/.move", dir) || st_rmtree(p)) { if (!e) e = errno; }
    }
    if (e) { errno = e; return -1; }
    return 0;
}
/* The game lock (runtime/frontend_android.c holds it while the game runs): an open-file-description lock, so it also
   conflicts with the launcher's FileChannel.tryLock. -1 with errno EAGAIN when the game holds it. */
static int game_lock(const char *priv) {
    char dir[SETUP_PATH], path[SETUP_PATH];
    if (!priv || !*priv) return -2;
    if (st_path(dir, sizeof dir, "%s/state", priv) || st_path(path, sizeof path, "%s/game.lock", dir)) return -1;
    if (st_mkdirs(dir)) return -1;
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    struct flock l = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
    if (fcntl(fd, F_OFD_SETLK, &l)) { int e = errno == EACCES ? EAGAIN : errno; close(fd); errno = e; return -1; }
    return fd;
}
static char *fail_json(SetupCtx *c) {
    Buf b = {0};
    buf_puts(&b, "{\"ok\":false,\"stage\":\"move\",\"code\":"); buf_jstr(&b, c->fail_code[0] ? c->fail_code : "internal");
    buf_puts(&b, ",\"message\":"); buf_jstr(&b, c->fail_msg[0] ? c->fail_msg : "Moving the game data failed"); buf_puts(&b, "}");
    return b.oom ? (buf_free(&b), strdup("{\"ok\":false,\"stage\":\"move\",\"code\":\"internal\",\"message\":\"out of memory\"}")) : b.p;
}
/* 1 when b is a or lies inside it. */
static int inside(const char *a, const char *b) { size_t n = strlen(a); return !strncmp(a, b, n) && (b[n] == '/' || !b[n]); }

char *setup_move_json(const char *request, size_t len, SetupProgressFn progress_fn, void *user, atomic_int *cancel, const SetupPins *pins,
                      const SetupMoveHooks *hooks) {
    Move *m = calloc(1, sizeof *m); SetupCtx *c = calloc(1, sizeof *c);
    char err[256], *res = NULL, priv[SETUP_PATH] = ""; int lock = -1, rc = -1, stage_made = 0;
    Json *j = NULL;
    if (!m || !c) { free(m); free(c); return strdup("{\"ok\":false,\"stage\":\"move\",\"code\":\"internal\",\"message\":\"out of memory\"}"); }
    setup_ctx_init(c, "", "", "", cancel);
    if (pins) c->pins = *pins;
    c->progress = progress_fn; c->progress_user = user;
    m->c = c; m->hooks = hooks;
    if (!(j = json_parse(request, len, err, sizeof err))) { setup_fail(c, "internal", "%s", err); goto fail; }
    const char *from = json_str(j, "from"), *to = json_str(j, "to"), *pv = json_str(j, "privateDir");
    int64_t part_req = 0;
    if (j->type != JSON_OBJ || !from || from[0] != '/' || !to || to[0] != '/' || strlen(from) >= SETUP_PATH - 256 || strlen(to) >= SETUP_PATH - 256 ||
        (json_get(j, "romfsPartBytes") && (json_int(j, "romfsPartBytes", &part_req) || part_req < 0))) {
        setup_fail(c, "internal", "The move request needs absolute from and to folders (and a non-negative romfsPartBytes)"); goto fail;
    }
    snprintf(m->from, sizeof m->from, "%s", from); snprintf(m->to, sizeof m->to, "%s", to);
    for (char *s = m->from + strlen(m->from) - 1; s > m->from && *s == '/'; s--) *s = 0;
    for (char *s = m->to + strlen(m->to) - 1; s > m->to && *s == '/'; s--) *s = 0;
    if (pv) snprintf(priv, sizeof priv, "%s", pv);
    if (inside(m->from, m->to) || inside(m->to, m->from)) { setup_fail(c, "internal", "The game data cannot be moved into or out of itself"); goto fail; }
    snprintf(c->game, sizeof c->game, "%s", m->to);
    st_path(m->stage, sizeof m->stage, "%s/.move", m->to);
    snprintf(c->staging, sizeof c->staging, "%s", m->stage);
    setup_progress(c, "move", 0, 0, "checking");

    /* Android SetupService holds the JVM's shared record-lock lease throughout this call and the folder switch.
       Do not even open this inode in that case: closing any descriptor here would release its POSIX lock. Standalone
       callers (including host tests) still own an OFD lock for the entire move. */
    lock = json_bool(j, "gameLockHeld") ? -2 : game_lock(priv);
    if (lock == -1) {
        if (errno == EAGAIN) setup_fail(c, "busy", "The game is running; close it before moving the game data");
        else st_io_fail(c, "open", "state/game.lock");
        goto fail;
    }
    SetupInstall st;
    if (setup_install_load(m->from, &st) || !st.done[ST_ROMFS]) {
        setup_fail(c, "missing_input", "There is no installed game in the current game folder to move"); goto fail;
    }
    char src_romfs[SETUP_PATH]; RomfsImage img;
    if (st_path(src_romfs, sizeof src_romfs, "%s/" SETUP_ROMFS_DIR, m->from) || romfs_image_open(&img, src_romfs)) {
        setup_fail(c, "missing_input", "The RomFS image of the current game folder is missing; set the game up again"); goto fail;
    }
    uint64_t image_size = img.size;
    romfs_image_close(&img);
    if (image_size != c->pins.romfs_size) { setup_fail(c, "hash_mismatch", "The RomFS in the current game folder is not the 1.4.3 image; set the game up again"); goto fail; }
    uint64_t part_bytes = setup_part_bytes(m->to, (uint64_t)part_req);
    int nparts = romfs_parts_for(image_size, part_bytes);
    if (nparts < 0) { setup_fail(c, "internal", "romfsPartBytes %llu splits the RomFS into more than %d parts", (unsigned long long)part_bytes, SETUP_MAX_ROMFS_PARTS); goto fail; }

    for (int i = 0; i < NENTRIES; i++) if (walk(m, entries[i])) goto fail;
    /* An older copy at the destination (an earlier move back and forth) is not the game the app uses: its space counts. */
    if (st_mkdirs(m->to)) { st_io_fail(c, "create", m->to); goto fail; }
    if (remove_game_data(m->to, 1)) { st_io_fail(c, "remove the older game data in", m->to); goto fail; }
    uint64_t need = m->files_bytes + image_size + (uint64_t)m->n * (256u << 10) + 64 * MIB;   /* cluster slack per file */
    uint64_t have = hooks && hooks->free_bytes ? hooks->free_bytes(hooks->user, m->to) : st_free_bytes(m->to);
    if (have < need) {
        setup_fail(c, "no_space", "Moving the game data needs %.1f GB of free space there; %.1f GB is available.", need / 1e9, have / 1e9); goto fail;
    }
    if (!(m->buf = malloc(BUF))) { setup_fail(c, "internal", "Out of memory"); goto fail; }
    m->total = 2 * (m->files_bytes + image_size);
    if (mkdir(m->stage, 0775)) { st_io_fail(c, "create", m->stage); goto fail; }
    stage_made = 1;

    phase(m, "copy");
    for (size_t i = 0; i < m->n; i++) {
        Item *it = &m->items[i]; char p[SETUP_PATH];
        if ((rc = cancelled(m))) goto fail;
        if (it->dir) { if (st_path(p, sizeof p, "%s/%s", m->stage, it->rel) || mkdir(p, 0775)) { rc = st_io_fail(c, "create", it->rel); goto fail; } continue; }
        if ((rc = copy_file(m, it))) goto fail;
    }
    phase(m, "romfs");
    char sha[65];
    if ((rc = cancelled(m)) || (rc = copy_image(m, part_bytes, sha))) goto fail;
    phase(m, "verify");
    if ((rc = verify(m))) goto fail;
    phase(m, "commit");
    if ((rc = cancelled(m))) goto fail;
    rc = -1;
    setup_progress(c, "move", m->done, m->total, "finishing");
    if (st_fsync_dir(m->stage)) { st_io_fail(c, "sync", m->stage); goto fail; }
    /* Past this point the copy is complete and verified: no more cancellation, only renames. */
    for (int i = 0; i < NENTRIES; i++) {
        char a[SETUP_PATH], b[SETUP_PATH];
        if (st_path(a, sizeof a, "%s/%s", m->stage, entries[i]) || st_path(b, sizeof b, "%s/%s", m->to, entries[i])) { st_io_fail(c, "move", entries[i]); goto fail; }
        if (!st_exists(a)) continue;
        if (rename(a, b)) { st_io_fail(c, "move into place", entries[i]); goto fail; }
        m->committed[i] = 1;
    }
    if (st_fsync_dir(m->to)) { st_io_fail(c, "sync", m->to); goto fail; }
    st.romfs_parts = nparts;
    if (setup_install_write(c, &st)) goto fail;
    rmdir(m->stage);
    if (priv[0]) { char dir[SETUP_PATH]; if (!st_path(dir, sizeof dir, "%s/" SETUP_ROMFS_DIR, m->to)) setup_proof_write(priv, dir, c->pins.romfs_sha256); }
    setup_progress(c, "move", m->total, m->total, "done");
    Buf b = {0};
    buf_puts(&b, "{\"ok\":true,\"install\":"); setup_install_json(&b, &st); buf_puts(&b, "}");
    res = b.oom ? (buf_free(&b), strdup("{\"ok\":true}")) : b.p;
    goto done;
fail:
    if (rc == SETUP_CANCELLED || (!c->failed && setup_cancelled(c))) { setup_clear_fail(c); setup_fail(c, "cancelled", "Moving the game data was cancelled; nothing was changed"); }
    /* Only what this move put into <to>: the verified copies already renamed into place, and the staging folder. */
    if (stage_made) {
        char p[SETUP_PATH];
        for (int i = 0; i < NENTRIES; i++) if (m->committed[i] && !st_path(p, sizeof p, "%s/%s", m->to, entries[i])) st_rmtree(p);
        st_rmtree(m->stage);
    }
    res = fail_json(c);
done:
    if (lock >= 0) close(lock);
    for (size_t i = 0; i < m->n; i++) free(m->items[i].rel);
    free(m->items); free(m->buf); json_free(j); free(j); free(m); free(c);
    return res;
}

char *setup_remove_json(const char *dir) {
    if (!dir || dir[0] != '/' || strstr(dir, "/../") || strlen(dir) >= SETUP_PATH - 64)
        return strdup("{\"ok\":false,\"stage\":\"remove\",\"code\":\"internal\",\"message\":\"remove needs an absolute folder\"}");
    if (remove_game_data(dir, 1)) {
        Buf b = {0}; char msg[SETUP_PATH + 64];
        snprintf(msg, sizeof msg, "Could not remove all game data from %s: %s", dir, strerror(errno));
        buf_puts(&b, "{\"ok\":false,\"stage\":\"remove\",\"code\":\"io\",\"message\":"); buf_jstr(&b, msg); buf_puts(&b, "}");
        return b.oom ? (buf_free(&b), strdup("{\"ok\":false,\"stage\":\"remove\",\"code\":\"io\",\"message\":\"\"}")) : b.p;
    }
    return strdup("{\"ok\":true}");
}
