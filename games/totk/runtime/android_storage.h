/* Where the Android app's game data lives and who may use it (runtime/frontend_android.c start_game), as a header so the
   host tests compile exactly this code (tests/test_android_storage.py).
   - The game data folder is the one the launcher chose (settings.env TOTK_DATA_DIR), one of three kinds
     (android/java/.../setup/StoragePaths.Kind): the app's private storage <internalDataPath>/game (the default), the app
     folder <volume>/Android/data/<package>/files (an SD card, or the internal one of installs made before private
     storage), or a folder the player picked on shared storage (/storage/emulated/<user>/... or /storage/<uuid>/...),
     which the app may use only while it holds All files access (MANAGE_EXTERNAL_STORAGE), re-checked at every start.
     A card can be missing, unmounted or replaced since the launcher checked, and the permission can have been revoked;
     the runtime then must not create an empty folder somewhere and fail later on half-found files, so the value is
     accepted only as an existing, link-free folder of one of these kinds on a mounted volume.
   - The game lock: <private>/state/game.lock, held with an open-file-description write lock for the whole game process.
     OFD locks conflict with the classic POSIX record locks Java's FileChannel.tryLock takes, so the launcher (another
     process) sees "game running" and can hold the lock itself for a moment while it swaps the save folder. */
#pragma once
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#ifndef F_OFD_SETLK
#define F_OFD_SETLK 37   /* Linux 3.15+; older C libraries lack the name */
#endif

/* What a game folder is checked against. */
typedef struct {
    const char *package;        /* the app's package: <volume>/Android/data/<package>/files */
    const char *private_files;  /* ANativeActivity.internalDataPath; <private_files>/game is private storage */
    const char *storage_root;   /* "/storage": shared volumes are <root>/emulated/<user> and <root>/<uuid> */
    int all_files;              /* All files access is granted now (Environment.isExternalStorageManager) */
} AndroidDataDirs;
enum { ANDROID_DATA_PRIVATE = 1, ANDROID_DATA_APP = 2, ANDROID_DATA_FOLDER = 3 };

/* 0 when the longest mount point containing volume is a real filesystem (not the root, nor the tmpfs that /storage
   is: an unmounted card leaves its mount point there); else -1 and a reason. mounts: a /proc/self/mounts format table
   (NULL = that file). */
static int android_volume_mounted(const char *volume, const char *mounts, char *why, size_t n) {
    FILE *f = fopen(mounts ? mounts : "/proc/self/mounts", "r");
    if (!f) { snprintf(why, n, "cannot be checked: no mount table (%s)", strerror(errno)); return -1; }
    char line[4096], best_dir[1000] = "", best_type[64] = ""; size_t best = 0;
    while (fgets(line, sizeof line, f)) {
        char dev[1024], dir[1000], type[64];
        if (sscanf(line, "%1023s %999s %63s", dev, dir, type) != 3) continue;
        size_t k = strlen(dir);   /* mount points with blanks (\040) never contain a game folder */
        int contains = !strcmp(dir, "/") || (!strncmp(volume, dir, k) && (volume[k] == '/' || !volume[k]));
        if (contains && k >= best) { best = k; snprintf(best_dir, sizeof best_dir, "%s", dir); snprintf(best_type, sizeof best_type, "%s", type); }
    }
    fclose(f);
    if (!best_dir[0] || !strcmp(best_dir, "/") || !strcmp(best_type, "tmpfs") || !strcmp(best_type, "rootfs")) {
        snprintf(why, n, "is not on a mounted storage volume (%s is %s)", volume, best_dir[0] ? best_type : "in no mount");
        return -1;
    }
    return 0;
}
/* An existing folder reached without links: its real path is itself. */
static int android_plain_dir(const char *path, char *why, size_t n) {
    char real[PATH_MAX]; struct stat st;
    if (!realpath(path, real)) { snprintf(why, n, "is not available (%s): the storage is missing or not mounted", strerror(errno)); return -1; }
    if (strcmp(real, path)) { snprintf(why, n, "is reached through a link or an indirect path (%s)", real); return -1; }
    if (stat(path, &st) || !S_ISDIR(st.st_mode)) { snprintf(why, n, "is not a folder"); return -1; }
    return 0;
}

/* The kind of game folder path is (ANDROID_DATA_*), or -1 and a reason for the log ("... is ..."). */
static int android_data_dir_check(const AndroidDataDirs *d, const char *path, const char *mounts, char *why, size_t n) {
    size_t len = path ? strlen(path) : 0;
    char suffix[512], own[1100];
    if (!len || path[0] != '/' || len >= 1000) { snprintf(why, n, "is not an absolute folder path"); return -1; }
    if (strstr(path, "//") || strstr(path, "/./") || strstr(path, "/../") || (len > 1 && path[len - 1] == '/')) {
        snprintf(why, n, "is reached through a link or an indirect path"); return -1;
    }
    /* Private storage: <internalDataPath>/game. internalDataPath itself may be reached through a link (/data/user/0 is
       one on some versions), so the folder's real path is compared with the real path of internalDataPath. */
    snprintf(own, sizeof own, "%s/game", d->private_files ? d->private_files : "");
    if (d->private_files && d->private_files[0] == '/' && !strcmp(path, own)) {
        char real[PATH_MAX], base[PATH_MAX], want[PATH_MAX + 8]; struct stat st;
        if (!realpath(d->private_files, base) || !realpath(path, real)) { snprintf(why, n, "is not available (%s)", strerror(errno)); return -1; }
        snprintf(want, sizeof want, "%s/game", base);
        if (strcmp(real, want)) { snprintf(why, n, "is reached through a link (%s)", real); return -1; }
        if (stat(path, &st) || !S_ISDIR(st.st_mode)) { snprintf(why, n, "is not a folder"); return -1; }
        return ANDROID_DATA_PRIVATE;
    }
    /* The app folder on a volume: <volume>/Android/data/<package>/files. */
    snprintf(suffix, sizeof suffix, "/Android/data/%s/files", d->package ? d->package : "");
    size_t sl = strlen(suffix);
    if (d->package && d->package[0] && len > sl + 1 && !strcmp(path + len - sl, suffix)) {
        if (android_plain_dir(path, why, n)) return -1;
        char volume[1000]; memcpy(volume, path, len - sl); volume[len - sl] = 0;
        return android_volume_mounted(volume, mounts, why, n) ? -1 : ANDROID_DATA_APP;
    }
    /* A picked folder below a shared volume: <root>/emulated/<user>/... or <root>/<uuid>/..., not the volume itself and
       not inside its Android/ folder (other apps' data; our own app folder is the kind above). */
    const char *root = d->storage_root ? d->storage_root : "/storage";
    size_t rl = strlen(root);
    if (rl < 2 || strncmp(path, root, rl) || path[rl] != '/') {
        snprintf(why, n, "is not private storage, an app data folder (*%s) or a folder on shared storage", suffix); return -1;
    }
    const char *rest = path + rl + 1, *slash;
    if (!*rest || *rest == '/' || !strncmp(rest, "self/", 5) || !strcmp(rest, "self")) { snprintf(why, n, "is not inside a storage volume"); return -1; }
    if (!strncmp(rest, "emulated/", 9) || !strcmp(rest, "emulated")) {
        const char *user = rest[8] ? rest + 9 : rest + 8; size_t digits = strspn(user, "0123456789");
        if (!digits || (user[digits] != '/' && user[digits])) { snprintf(why, n, "is not inside a storage volume"); return -1; }
        slash = user + digits;
    } else if (!(slash = strchr(rest, '/'))) slash = rest + strlen(rest);
    if (!slash[0] || !slash[1]) { snprintf(why, n, "is a whole storage volume"); return -1; }
    if (!strncasecmp(slash + 1, "Android", 7) && (slash[8] == '/' || !slash[8])) { snprintf(why, n, "is inside the volume's Android folder"); return -1; }
    if (!d->all_files) {
        snprintf(why, n, "needs All files access, which is turned off for TotK Native: allow it again in the launcher (Game data), or move the game data");
        return -1;
    }
    if (android_plain_dir(path, why, n)) return -1;
    char volume[1000]; size_t vl = (size_t)(slash - path); memcpy(volume, path, vl); volume[vl] = 0;
    return android_volume_mounted(volume, mounts, why, n) ? -1 : ANDROID_DATA_FOLDER;
}

/* The game lock, waiting up to wait_ms for a brief hold by the launcher. The descriptor stays open for the life of the
   process (the kernel releases the lock when it ends, however it ends). -1 and a reason when it cannot be taken. */
static int android_game_lock(const char *path, int wait_ms, char *why, size_t n) {
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) { snprintf(why, n, "cannot open %s: %s", path, strerror(errno)); return -1; }
    for (int waited = 0;; waited += 20) {
        struct flock l; memset(&l, 0, sizeof l); l.l_type = F_WRLCK; l.l_whence = SEEK_SET;   /* the whole file */
        if (!fcntl(fd, F_OFD_SETLK, &l)) return fd;
        if ((errno != EAGAIN && errno != EACCES) || waited >= wait_ms) {
            if (errno == EAGAIN || errno == EACCES) snprintf(why, n, "%s is held by another TotK Native process (the game, or the launcher changing saves)", path);
            else snprintf(why, n, "cannot lock %s: %s", path, strerror(errno));
            close(fd); return -1;
        }
        struct timespec t = { 0, 20 * 1000000L }; nanosleep(&t, NULL);
    }
}

/* Complete both successful and refused launch handshakes. Never unlink game.lock itself (its inode is the protocol). */
__attribute__((unused)) static int android_game_start_finished(const char *private_files) {
    char path[PATH_MAX];
    if (snprintf(path, sizeof path, "%s/state/game-starting", private_files) >= (int)sizeof path) { errno = ENAMETOOLONG; return -1; }
    return unlink(path) && errno != ENOENT ? -1 : 0;
}

/* A close command must not start SINGLE_TASK TotkActivity: Android would pop the menu, losing its pending operation.
   These private markers preserve the existing task/recents behavior. Paths are prepared once; an idle poll is one stat. */
typedef struct { char request[PATH_MAX], response[PATH_MAX], temporary[PATH_MAX]; } AndroidClosePaths;
__attribute__((unused)) static int android_close_paths(AndroidClosePaths *p, const char *private_files) {
    if (snprintf(p->request, sizeof p->request, "%s/state/close-request", private_files) >= (int)sizeof p->request ||
        snprintf(p->response, sizeof p->response, "%s/state/close-response", private_files) >= (int)sizeof p->response ||
        snprintf(p->temporary, sizeof p->temporary, "%s/state/close-response.tmp", private_files) >= (int)sizeof p->temporary) {
        errno = ENAMETOOLONG; return -1;
    }
    return 0;
}
static int android_close_reply(const AndroidClosePaths *p, const char *id, const char *reason) {
    char text[512]; int n = snprintf(text, sizeof text, "%s\n%s\n", id, reason);
    if (n < 0 || n >= (int)sizeof text) { errno = EOVERFLOW; return -1; }
    int fd = open(p->temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    ssize_t written = write(fd, text, (size_t)n);
    int bad = written != n, saved = bad ? (written < 0 ? errno : EIO) : 0;
    if (!bad && fsync(fd)) { bad = 1; saved = errno; }
    if (close(fd) && !bad) { bad = 1; saved = errno; }
    if (!bad && rename(p->temporary, p->response)) { bad = 1; saved = errno; }
    if (bad) { unlink(p->temporary); errno = saved; return -1; }
    return 0;
}
/* 0 absent or accepted, 1 consumed/refused with saving response, -1 I/O error (response includes errno when writable).
   The Android accepted close exits; accepting/queued frontends may instead return 1. */
__attribute__((unused)) static int android_close_poll(const AndroidClosePaths *p, int (*close_game)(void)) {
    struct stat st;
    if (stat(p->request, &st)) return errno == ENOENT ? 0 : -1;
    char id[96] = "", why[256]; int fd = open(p->request, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        int saved = errno;
        snprintf(why, sizeof why, "error: open close-request errno=%d (%s)", saved, strerror(saved));
        android_close_reply(p, "", why); errno = saved; return -1;
    }
    ssize_t n = read(fd, id, sizeof id - 1); int saved = errno;
    if (close(fd) && n >= 0) { n = -1; saved = errno; }
    if (n <= 0) {
        if (!n) saved = EINVAL;
        snprintf(why, sizeof why, "error: read close-request errno=%d (%s)", saved, strerror(saved));
        android_close_reply(p, "", why); errno = saved; return -1;
    }
    id[n] = 0; id[strcspn(id, "\r\n")] = 0;
    if (unlink(p->request)) {
        saved = errno;
        snprintf(why, sizeof why, "error: consume close-request errno=%d (%s)", saved, strerror(saved));
        android_close_reply(p, id, why); errno = saved; return -1;
    }
    if (close_game()) return 0;
    return android_close_reply(p, id, "saving") ? -1 : 1;
}

/* A refused start (no data folder, the lock held, nothing installed) finishes the activity, but Android keeps the
   process: the next "Start game" would then reuse it, where start_game has already run once and the game never
   starts (a black window), and the launcher would see a game process and refuse save changes and moves. So the process
   ends delay_ms after the refusal, once the activity's finish has been handled; the next start gets a fresh one. */
static void *android_refused_exit_thread(void *arg) {
    unsigned ms = (unsigned)(uintptr_t)arg;
    struct timespec t = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    while (nanosleep(&t, &t) && errno == EINTR) { }
    fprintf(stderr, "[totk-app] the start was refused: this process ends so that the next start is a fresh one\n");
    fflush(stderr);
    _exit(0);
}
__attribute__((unused)) static void android_refused_exit(unsigned delay_ms) {
    pthread_t thread; pthread_attr_t attr;
    pthread_attr_init(&attr); pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&thread, &attr, android_refused_exit_thread, (void *)(uintptr_t)delay_ms)) _exit(0);
    pthread_attr_destroy(&attr);
}
