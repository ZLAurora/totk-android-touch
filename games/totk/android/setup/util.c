/* File helpers for the importer: bounded pread/write loops, directory creation and removal, fsync'd atomic writes and
   commits (rename into place, then fsync the directory so a power loss cannot leave a renamed-but-empty file). */
#define _GNU_SOURCE 1
#include "setup_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

int st_path(char *out, size_t cap, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); int n = vsnprintf(out, cap, fmt, ap); va_end(ap);
    if (n < 0 || (size_t)n >= cap) { if (cap) out[0] = 0; errno = ENAMETOOLONG; return -1; }
    return 0;
}
int st_pread(int fd, void *buf, size_t n, uint64_t off) {
    uint8_t *p = buf;
    while (n) {
        ssize_t r = pread(fd, p, n > (1u << 30) ? (1u << 30) : n, (off_t)off);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) return -1;
        if (r == 0) { errno = EIO; return -1; }
        p += r; n -= (size_t)r; off += (uint64_t)r;
    }
    return 0;
}
int st_write(int fd, const void *buf, size_t n) {
    const uint8_t *p = buf;
    while (n) {
        ssize_t r = write(fd, p, n > (1u << 30) ? (1u << 30) : n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { if (!r) errno = EIO; return -1; }
        p += r; n -= (size_t)r;
    }
    return 0;
}
#define ST_CACHE_WINDOW (128ull << 20)
void st_cache_written(int fd, StCache *w, uint64_t written_end) {
    if (written_end < w->mark + ST_CACHE_WINDOW) return;
    /* Start write-back of the window just filled, then wait for and drop the one before it, so the device keeps writing
       while we copy. Outputs without sync_file_range (some FUSE mounts) fall back to fdatasync. */
    if (sync_file_range(fd, (off_t)w->mark, (off_t)(written_end - w->mark), SYNC_FILE_RANGE_WRITE)) fdatasync(fd);
    if (w->prev_end > w->prev_start) st_cache_flush_range(fd, w->prev_start, w->prev_end - w->prev_start);
    w->prev_start = w->mark; w->prev_end = written_end; w->mark = written_end;
}
void st_cache_flush_range(int fd, uint64_t off, uint64_t len) {
    if (sync_file_range(fd, (off_t)off, (off_t)len, SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE | SYNC_FILE_RANGE_WAIT_AFTER)) fdatasync(fd);
    posix_fadvise(fd, (off_t)off, (off_t)len, POSIX_FADV_DONTNEED);
}
void st_cache_drop(int fd, uint64_t off, uint64_t len) { posix_fadvise(fd, (off_t)off, (off_t)len, POSIX_FADV_DONTNEED); }
int st_mkdirs(const char *path) {
    char tmp[SETUP_PATH];
    if (st_path(tmp, sizeof tmp, "%s", path)) return -1;
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; if (mkdir(tmp, 0775) && errno != EEXIST) return -1; *p = '/'; }
    if (mkdir(tmp, 0775) && errno != EEXIST) return -1;
    struct stat st;
    if (stat(tmp, &st)) return -1;
    if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
    return 0;
}
static int rm_one(const char *path, const struct stat *st, int type, struct FTW *f) {
    (void)st; (void)f;
    return (type == FTW_DP ? rmdir(path) : unlink(path)) && errno != ENOENT ? -1 : 0;
}
int st_rmtree(const char *path) {
    struct stat st;
    if (lstat(path, &st)) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode)) return unlink(path);
    return nftw(path, rm_one, 16, FTW_DEPTH | FTW_PHYS);
}
int st_fsync_dir(const char *path) {
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    /* Some FUSE-backed storage refuses fsync on directories; the rename itself is still ordered after the file fsync. */
    if (fsync(fd) && errno != EINVAL && errno != EROFS && errno != ENOSYS && errno != EPERM && errno != EACCES) { int e = errno; close(fd); errno = e; return -1; }
    close(fd); return 0;
}
int st_write_file(const char *path, const void *data, size_t n, int mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) return -1;
    if (st_write(fd, data, n) || fsync(fd)) { int e = errno; close(fd); unlink(path); errno = e; return -1; }
    return close(fd);
}
static void dir_of(const char *path, char *out, size_t cap) {
    snprintf(out, cap, "%s", path);
    char *s = strrchr(out, '/');
    if (s && s != out) *s = 0; else snprintf(out, cap, "%s", s ? "/" : ".");
}
int st_write_atomic(const char *path, const void *data, size_t n) {
    char tmp[SETUP_PATH], dir[SETUP_PATH];
    if (st_path(tmp, sizeof tmp, "%s.tmp", path)) return -1;
    if (st_write_file(tmp, data, n, 0644)) return -1;
    if (rename(tmp, path)) { int e = errno; unlink(tmp); errno = e; return -1; }
    dir_of(path, dir, sizeof dir);
    return st_fsync_dir(dir);
}
int st_commit(SetupCtx *c, const char *from, const char *to) {
    char dir[SETUP_PATH];
    dir_of(to, dir, sizeof dir);
    if (st_mkdirs(dir)) return st_io_fail(c, "create", dir);
    if (rename(from, to)) return st_io_fail(c, "move into place", to);
    if (st_fsync_dir(dir)) return st_io_fail(c, "sync", dir);
    return 0;
}
int st_exists(const char *path) { struct stat st; return !stat(path, &st); }
int64_t st_file_size(const char *path) { struct stat st; return stat(path, &st) || !S_ISREG(st.st_mode) ? -1 : (int64_t)st.st_size; }
int st_read_file(const char *path, char **out, size_t *len, size_t max) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st)) { int e = errno; close(fd); errno = e; return -1; }
    if (!S_ISREG(st.st_mode) || (uint64_t)st.st_size > max) { close(fd); errno = EFBIG; return -1; }
    char *p = malloc((size_t)st.st_size + 1);
    if (!p || st_pread(fd, p, (size_t)st.st_size, 0)) { int e = errno; free(p); close(fd); errno = e; return -1; }
    close(fd); p[st.st_size] = 0; *out = p; *len = (size_t)st.st_size;
    return 0;
}
uint64_t st_free_bytes(const char *path) {
    struct statvfs v;
    return statvfs(path, &v) ? 0 : (uint64_t)v.f_bavail * v.f_frsize;
}
uint64_t st_now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000; }
int st_io_fail(SetupCtx *c, const char *what, const char *path) {
    int e = errno;
    const char *slash = path ? strrchr(path, '/') : NULL;
    setup_fail(c, e == ENOSPC || e == EDQUOT ? "no_space" : "io", "Could not %s %s: %s", what, slash ? slash + 1 : path ? path : "", strerror(e));
    errno = e;
    return -1;
}
/* Streams [off, off+len) of fd through SHA-256 with progress and cancellation. */
int st_sha256_fd(SetupCtx *c, int fd, uint64_t off, uint64_t len, uint8_t out[32], const char *stage, uint64_t base_done, uint64_t total) {
    size_t cap = 8u << 20; uint8_t *buf = malloc(cap);
    if (!buf) { setup_fail(c, "internal", "Out of memory"); return -1; }
    Sha256 s; sha256_init(&s);
    for (uint64_t done = 0; done < len;) {
        if (setup_cancelled(c)) { free(buf); return SETUP_CANCELLED; }
        size_t n = len - done < cap ? (size_t)(len - done) : cap;
        if (st_pread(fd, buf, n, off + done)) { int rc = st_io_fail(c, "read", "input"); free(buf); return rc; }
        sha256_update(&s, buf, n); st_cache_drop(fd, off + done, n); done += n;
        if (stage) setup_progress(c, stage, base_done + done, total, "verifying");
    }
    free(buf); sha256_final(&s, out);
    return 0;
}
int st_sha256_file(SetupCtx *c, const char *path, char hex[65], int64_t *size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return st_io_fail(c, "open", path);
    struct stat st; uint8_t d[32];
    if (fstat(fd, &st)) { int r = st_io_fail(c, "stat", path); close(fd); return r; }
    int r = st_sha256_fd(c, fd, 0, (uint64_t)st.st_size, d, NULL, 0, 0);
    close(fd);
    if (r) return r;
    sha256_hex(d, hex);
    if (size) *size = st.st_size;
    return 0;
}
