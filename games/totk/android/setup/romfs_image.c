/* The RomFS image as it lies on disk: <dir>/romfs.bin, or equal-size parts <dir>/romfs.bin.000, .001, ... (the last one
   shorter) exactly as runtime/romfs.c open_chunks reads them. FAT32 volumes (SDHC cards up to 32 GB) cannot hold the
   17 GB image as one file, so the romfs stage and the move write parts there; every reader in the setup library (index,
   shader seed, companion, move) goes through RomfsImage so it does not care which layout it got. */
#define _GNU_SOURCE 1
#include "setup_internal.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

static void image_init(RomfsImage *m) { memset(m, 0, sizeof *m); for (int i = 0; i < SETUP_MAX_ROMFS_PARTS; i++) m->fd[i] = -1; }

int romfs_image_open_file(RomfsImage *m, const char *path) {
    image_init(m);
    struct stat st;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    if (fstat(fd, &st)) { int e = errno; close(fd); errno = e; return -1; }
    if (!S_ISREG(st.st_mode)) { close(fd); errno = EINVAL; return -1; }
    m->fd[0] = fd; m->size = m->part_size = (uint64_t)st.st_size;
    return 0;
}
int romfs_image_open(RomfsImage *m, const char *dir) {
    char path[SETUP_PATH];
    if (st_path(path, sizeof path, "%s/romfs.bin", dir)) return -1;
    /* The runtime takes romfs.bin over parts when both exist; so does every setup reader. */
    if (!romfs_image_open_file(m, path)) return 0;
    if (errno != ENOENT) return -1;
    for (int i = 0; i < SETUP_MAX_ROMFS_PARTS; i++) {
        struct stat st;
        if (st_path(path, sizeof path, "%s/romfs.bin.%03d", dir, i)) { int e = errno; romfs_image_close(m); errno = e; return -1; }
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) { if (errno == ENOENT) break; int e = errno; romfs_image_close(m); errno = e; return -1; }
        m->fd[i] = fd; m->nparts = i + 1;
        if (fstat(fd, &st)) { int e = errno; romfs_image_close(m); errno = e; return -1; }
        if (!S_ISREG(st.st_mode) || !st.st_size) { romfs_image_close(m); errno = EINVAL; return -1; }
        /* Every part but the last has the first part's size (open_chunks' rule): a part's offset is index * part_size. */
        if (!i) m->part_size = (uint64_t)st.st_size;
        else if (m->size != (uint64_t)i * m->part_size || (uint64_t)st.st_size > m->part_size) { romfs_image_close(m); errno = EINVAL; return -1; }
        m->size += (uint64_t)st.st_size;
    }
    if (!m->nparts) { errno = ENOENT; return -1; }
    /* romfs.bin.064 would be a part the runtime never opens: the image is not what it reads. */
    if (st_path(path, sizeof path, "%s/romfs.bin.%03d", dir, SETUP_MAX_ROMFS_PARTS)) { int e = errno; romfs_image_close(m); errno = e; return -1; }
    if (st_exists(path)) { romfs_image_close(m); errno = EINVAL; return -1; }
    return 0;
}
void romfs_image_close(RomfsImage *m) {
    for (int i = 0; i < SETUP_MAX_ROMFS_PARTS; i++) if (m->fd[i] >= 0) close(m->fd[i]);
    image_init(m);
}
int romfs_image_pread(const RomfsImage *m, void *buf, size_t n, uint64_t off) {
    if (!m->nparts) return st_pread(m->fd[0], buf, n, off);
    uint8_t *p = buf;
    while (n) {
        uint64_t part = off / m->part_size, in = off % m->part_size;
        if (part >= (uint64_t)m->nparts) { errno = EIO; return -1; }
        size_t k = m->part_size - in < n ? (size_t)(m->part_size - in) : n;
        if (st_pread(m->fd[part], p, k, in)) return -1;
        p += k; n -= k; off += k;
    }
    return 0;
}
void romfs_image_drop(const RomfsImage *m, uint64_t off, uint64_t len) {
    if (!m->nparts) { st_cache_drop(m->fd[0], off, len); return; }
    while (len) {
        uint64_t part = off / m->part_size, in = off % m->part_size;
        if (part >= (uint64_t)m->nparts) return;
        uint64_t k = m->part_size - in < len ? m->part_size - in : len;
        st_cache_drop(m->fd[part], in, k);
        off += k; len -= k;
    }
}
int romfs_image_stat(const RomfsImage *m, int part, struct stat *st) { return fstat(m->fd[m->nparts ? part : 0], st); }

int romfs_parts_for(uint64_t size, uint64_t part_bytes) {
    if (!part_bytes) return 0;
    uint64_t n = size / part_bytes + (size % part_bytes != 0);
    return n && n <= SETUP_MAX_ROMFS_PARTS ? (int)n : -1;
}
/* 1 for "romfs.bin" (*part = -1) and "romfs.bin.NNN" (*part = NNN). */
static int romfs_name(const char *name, int *part) {
    if (!strcmp(name, "romfs.bin")) { *part = -1; return 1; }
    if (strncmp(name, "romfs.bin.", 10) || strlen(name) != 13 || strspn(name + 10, "0123456789") != 3) return 0;
    *part = atoi(name + 10);
    return 1;
}
int romfs_remove_stale(const char *dir, int keep_parts) {
    DIR *d = opendir(dir);
    if (!d) return errno == ENOENT ? 0 : -1;
    int rc = 0;
    for (struct dirent *e; (e = readdir(d));) {
        int part; char p[SETUP_PATH];
        if (!romfs_name(e->d_name, &part)) continue;
        if (keep_parts == 0 && part < 0) continue;
        if (keep_parts > 0 && part >= 0 && part < keep_parts) continue;
        if (st_path(p, sizeof p, "%s/%s", dir, e->d_name) || (unlink(p) && errno != ENOENT)) { rc = -1; break; }
    }
    int e = errno;
    closedir(d);
    if (rc) { errno = e; return -1; }
    return st_fsync_dir(dir);
}
int romfs_part_name(char *out, size_t cap, const char *dir, int nparts, int part) {
    return nparts ? st_path(out, cap, "%s/romfs.bin.%03d", dir, part) : st_path(out, cap, "%s/romfs.bin", dir);
}

/* ---- writer ----
   A ring of large buffers between three threads: the caller fills buffers in order (pread from the source, or the
   hactool FIFO), a hash thread runs SHA-256 over each one and a write thread stores it in the parts layout, so reading,
   hashing and writing overlap. A buffer is reused once both have passed it, so the digest covers exactly the bytes that
   were written, in order, and nothing is read back. Only the caller reports failures (setup_fail is not thread safe):
   the write thread records errno and the operation, the caller turns it into the stage failure at its next call. */
struct RomfsRing {
    pthread_mutex_t mu; pthread_cond_t cv;
    uint8_t *mem; size_t slot_bytes, *len; int nslots;
    uint64_t queued, hashed, stored;    /* buffers handed over / hashed / written, counted from the start */
    size_t fill;                        /* bytes the caller has put in buffer queued % nslots */
    int closing, stop, threads;         /* closing: no more buffers; stop: abort now */
    int err_errno; const char *err_what; char err_path[SETUP_PATH];
    pthread_t hasher, storer;
};
#define RING_SLOTS 4
#define RING_SLOT_BYTES (16u << 20)
/* TOTK_SETUP_RING=<count>x<bytes> replaces the ring geometry (host tests: tiny buffers so a small image crosses many). */
static void ring_geometry(int *n, size_t *bytes) {
    *n = RING_SLOTS; *bytes = RING_SLOT_BYTES;
    const char *e = getenv("TOTK_SETUP_RING");
    unsigned long long k, b;
    if (e && sscanf(e, "%llux%llu", &k, &b) == 2 && k >= 2 && k <= 64 && b >= 1 && b <= (256u << 20)) { *n = (int)k; *bytes = (size_t)b; }
}

/* Write thread: the parts layout, the 128 MiB write-back windows and a fsync + cache drop per finished part. */
static int store_fail(RomfsWriter *w, const char *what) {
    int e = errno ? errno : EIO;
    romfs_part_name(w->ring->err_path, sizeof w->ring->err_path, w->dir, w->nparts, w->part);
    w->ring->err_what = what; w->ring->err_errno = e; errno = e;
    return -1;
}
static int close_part(RomfsWriter *w) {
    int rc = 0;
    if (fsync(w->fd)) rc = store_fail(w, "sync");
    if (!rc) st_cache_drop(w->fd, 0, 0);   /* the part is on disk; nothing it caches is needed again soon */
    if (close(w->fd) && !rc) rc = store_fail(w, "write");
    w->fd = -1;
    if (!rc) w->part++;
    return rc;
}
static int store(RomfsWriter *w, const uint8_t *p, size_t n) {
    while (n) {
        if (w->fd < 0) {
            char path[SETUP_PATH];
            if (romfs_part_name(path, sizeof path, w->dir, w->nparts, w->part)) return store_fail(w, "create");
            w->fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            if (w->fd < 0) return store_fail(w, "create");
            w->in_part = 0; memset(&w->cache, 0, sizeof w->cache);
        }
        size_t k = n;
        if (w->nparts && w->part_bytes - w->in_part < k) k = (size_t)(w->part_bytes - w->in_part);
        if (st_write(w->fd, p, k)) return store_fail(w, "write");
        p += k; n -= k; w->in_part += k; w->written += k;
        st_cache_written(w->fd, &w->cache, w->in_part);
        if (w->nparts && w->in_part == w->part_bytes && close_part(w)) return -1;
    }
    return 0;
}
/* Both workers: take buffer *seq when the caller has handed it over, run it, advance. */
static void *ring_worker(RomfsWriter *w, uint64_t *seq, int hashing) {
    RomfsRing *r = w->ring;
    pthread_mutex_lock(&r->mu);
    for (;;) {
        while (!r->stop && *seq == r->queued && !r->closing) pthread_cond_wait(&r->cv, &r->mu);
        if (r->stop || *seq == r->queued) break;
        int slot = (int)(*seq % (uint64_t)r->nslots); size_t n = r->len[slot];
        pthread_mutex_unlock(&r->mu);
        const uint8_t *p = r->mem + (size_t)slot * r->slot_bytes;
        int rc = 0;
        if (hashing) sha256_update(&w->sha, p, n); else rc = store(w, p, n);
        pthread_mutex_lock(&r->mu);
        if (rc) { r->stop = 1; pthread_cond_broadcast(&r->cv); break; }   /* store_fail recorded what went wrong */
        (*seq)++;
        pthread_cond_broadcast(&r->cv);
    }
    pthread_mutex_unlock(&r->mu);
    return NULL;
}
static void *hash_main(void *arg) { RomfsWriter *w = arg; return ring_worker(w, &w->ring->hashed, 1); }
static void *store_main(void *arg) { RomfsWriter *w = arg; return ring_worker(w, &w->ring->stored, 0); }

static void ring_join(RomfsWriter *w) {
    RomfsRing *r = w->ring;
    if (!r || !r->threads) return;
    pthread_join(r->hasher, NULL);
    if (r->threads > 1) pthread_join(r->storer, NULL);
    r->threads = 0;
}
static void ring_free(RomfsWriter *w) {
    RomfsRing *r = w->ring;
    if (!r) return;
    pthread_mutex_lock(&r->mu); r->stop = 1; pthread_cond_broadcast(&r->cv); pthread_mutex_unlock(&r->mu);
    ring_join(w);
    pthread_mutex_destroy(&r->mu); pthread_cond_destroy(&r->cv);
    free(r->mem); free(r->len); free(r);
    w->ring = NULL;
}
/* The write thread's failure as the stage failure; 0 when it has none. Called with the lock held or after the join. */
static int ring_failed(SetupCtx *c, RomfsWriter *w) {
    RomfsRing *r = w->ring;
    if (!r->err_what) return 0;
    errno = r->err_errno;
    return st_io_fail(c, r->err_what, r->err_path);
}
/* Waits (in 100 ms steps, so a cancel is seen) until done(r) holds or the ring has failed; called with the lock held. */
static int ring_wait(SetupCtx *c, RomfsWriter *w, int (*done)(const RomfsRing *)) {
    RomfsRing *r = w->ring;
    while (!done(r)) {
        if (r->stop) { int rc = ring_failed(c, w); return rc ? rc : (setup_fail(c, "internal", "The RomFS writer stopped"), -1); }
        if (setup_cancelled(c)) return SETUP_CANCELLED;
        struct timespec t; clock_gettime(CLOCK_REALTIME, &t);
        t.tv_nsec += 100000000; if (t.tv_nsec >= 1000000000) { t.tv_sec++; t.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&r->cv, &r->mu, &t);
    }
    return 0;
}
static int slot_free(const RomfsRing *r) { return r->queued - (r->hashed < r->stored ? r->hashed : r->stored) < (uint64_t)r->nslots; }
static int drained(const RomfsRing *r) { return r->hashed == r->queued && r->stored == r->queued; }

int romfs_writer_open(SetupCtx *c, RomfsWriter *w, const char *dir, uint64_t total, uint64_t part_bytes) {
    memset(w, 0, sizeof *w); w->fd = -1;
    int n = romfs_parts_for(total, part_bytes);
    if (n < 0) { setup_fail(c, "internal", "RomFS part size %llu gives more than %d parts", (unsigned long long)part_bytes, SETUP_MAX_ROMFS_PARTS); return -1; }
    if (st_path(w->dir, sizeof w->dir, "%s", dir) || st_mkdirs(dir)) return st_io_fail(c, "create", dir);
    if (romfs_remove_stale(dir, -1)) return st_io_fail(c, "clear", dir);
    w->total = total; w->part_bytes = part_bytes; w->nparts = n;
    sha256_init(&w->sha);
    RomfsRing *r = calloc(1, sizeof *r);
    if (!r) { setup_fail(c, "internal", "Out of memory"); return -1; }
    w->ring = r;
    ring_geometry(&r->nslots, &r->slot_bytes);
    pthread_mutex_init(&r->mu, NULL); pthread_cond_init(&r->cv, NULL);
    r->mem = malloc((size_t)r->nslots * r->slot_bytes); r->len = calloc((size_t)r->nslots, sizeof *r->len);
    if (!r->mem || !r->len) { ring_free(w); setup_fail(c, "internal", "Out of memory"); return -1; }
    if (pthread_create(&r->hasher, NULL, hash_main, w)) { ring_free(w); setup_fail(c, "internal", "Could not start the RomFS hash thread"); return -1; }
    r->threads = 1;
    if (pthread_create(&r->storer, NULL, store_main, w)) { ring_free(w); setup_fail(c, "internal", "Could not start the RomFS write thread"); return -1; }
    r->threads = 2;
    return 0;
}
int romfs_writer_reserve(SetupCtx *c, RomfsWriter *w, uint8_t **p, size_t *room) {
    RomfsRing *r = w->ring; int rc = 0;
    if (!r->fill) {   /* a fresh buffer: wait until both workers are done with its previous contents */
        pthread_mutex_lock(&r->mu);
        rc = ring_wait(c, w, slot_free);
        pthread_mutex_unlock(&r->mu);
        if (rc) return rc;
    }
    *p = r->mem + (size_t)(r->queued % (uint64_t)r->nslots) * r->slot_bytes + r->fill;
    *room = r->slot_bytes - r->fill;
    return 0;
}
static void ring_hand_over(RomfsRing *r) {
    pthread_mutex_lock(&r->mu);
    r->len[r->queued % (uint64_t)r->nslots] = r->fill; r->queued++; r->fill = 0;
    pthread_cond_broadcast(&r->cv);
    pthread_mutex_unlock(&r->mu);
}
int romfs_writer_commit(SetupCtx *c, RomfsWriter *w, size_t n) {
    RomfsRing *r = w->ring;
    if (n > w->total - w->submitted) { setup_fail(c, "hash_mismatch", "The RomFS is longer than the %llu bytes of 1.4.3", (unsigned long long)w->total); return -1; }
    r->fill += n; w->submitted += n;
    if (r->fill == r->slot_bytes || (w->submitted == w->total && r->fill)) ring_hand_over(r);
    return 0;
}
int romfs_writer_finish(SetupCtx *c, RomfsWriter *w, char sha_hex[65]) {
    RomfsRing *r = w->ring;
    if (r->fill) ring_hand_over(r);
    pthread_mutex_lock(&r->mu);
    r->closing = 1; pthread_cond_broadcast(&r->cv);
    int rc = ring_wait(c, w, drained);
    pthread_mutex_unlock(&r->mu);
    if (rc) return rc;
    ring_join(w);
    if (!rc && w->fd >= 0 && close_part(w)) rc = ring_failed(c, w);
    if (rc) return rc;
    if (w->written != w->total) {
        setup_fail(c, "hash_mismatch", "The RomFS has %llu bytes; the 1.4.3 RomFS has %llu", (unsigned long long)w->written, (unsigned long long)w->total);
        return -1;
    }
    if (st_fsync_dir(w->dir)) return st_io_fail(c, "sync", w->dir);
    uint8_t d[32]; sha256_final(&w->sha, d); sha256_hex(d, sha_hex);
    ring_free(w);
    return 0;
}
void romfs_writer_abort(RomfsWriter *w) {
    ring_free(w);
    if (w->fd >= 0) close(w->fd);
    w->fd = -1;
    if (w->dir[0]) romfs_remove_stale(w->dir, -1);
}

/* ---- volumes ---- */
/* The filesystem type of the volume path lies on, from a mounts table (/proc/self/mounts format). Android apps see
   removable cards through FUSE at /storage/<uuid>; the card's own filesystem is the vold mount /mnt/media_rw/<uuid>.
   Elsewhere the longest mount point that contains path decides. 0 and an empty type when nothing matched. */
static void unescape(char *s) {   /* the table escapes blanks as \040 */
    char *w = s;
    for (char *r = s; *r; r++) {
        if (r[0] == '\\' && r[1] >= '0' && r[1] <= '7' && r[2] >= '0' && r[2] <= '7' && r[3] >= '0' && r[3] <= '7') { *w++ = (char)((r[1] - '0') * 64 + (r[2] - '0') * 8 + (r[3] - '0')); r += 3; }
        else *w++ = *r;
    }
    *w = 0;
}
int setup_volume_type(const char *mounts, const char *path, char *type, size_t cap) {
    char want[SETUP_PATH] = "";
    type[0] = 0;
    if (!strncmp(path, "/storage/", 9)) {
        const char *uuid = path + 9, *end = strchr(uuid, '/');
        size_t n = end ? (size_t)(end - uuid) : strlen(uuid);
        if (n && !(n == 8 && !strncmp(uuid, "emulated", 8)) && !(n == 4 && !strncmp(uuid, "self", 4)))
            snprintf(want, sizeof want, "/mnt/media_rw/%.*s", (int)n, uuid);
    }
    FILE *f = fopen(mounts ? mounts : "/proc/self/mounts", "r");
    if (!f) return 0;
    char line[4096]; size_t best = 0; int found = 0;
    while (fgets(line, sizeof line, f)) {
        char dev[1024], dir[SETUP_PATH], fs[64];
        if (sscanf(line, "%1023s %1023s %63s", dev, dir, fs) != 3) continue;
        unescape(dir);
        size_t n = strlen(dir);
        int hit = want[0] ? !strcmp(dir, want)
                          : (!strcmp(dir, "/") || (!strncmp(path, dir, n) && (path[n] == '/' || !path[n])));
        if (hit && (want[0] || n >= best)) { best = n; found = 1; snprintf(type, cap, "%s", fs); }
    }
    fclose(f);
    return found;
}
uint64_t setup_part_bytes(const char *path, uint64_t requested) {
    char type[64];
    const char *mounts = getenv("TOTK_SETUP_MOUNTS");   /* host tests: a synthetic table */
    if (setup_volume_type(mounts, path, type, sizeof type) && (!strcmp(type, "vfat") || !strcmp(type, "msdos") || !strcmp(type, "fat")) &&
        (!requested || requested > ROMFS_FAT_PART_BYTES))
        return ROMFS_FAT_PART_BYTES;
    return requested;
}
