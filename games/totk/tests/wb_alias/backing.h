/* MIT. Private host resources only; real shared mappings and Linux DRM UAPI.
 * i915 is a nonmerging graphics-VMA oracle, not a Qualcomm exporter/cache test. */
#pragma once
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static unsigned checks, failures;
#define CHECK(condition, message) do { ++checks; if (!(condition)) { ++failures; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, message); } } while (0)
static void required(int condition, const char *message) {
    if (!condition) { perror(message); exit(1); }
}
/* Kept local so the fixture needs neither libdrm nor its development headers. */
struct fixture_drm_version {
    int major, minor, patch;
    size_t name_len; char *name;
    size_t date_len; char *date;
    size_t desc_len; char *desc;
};
struct fixture_dumb_create { uint32_t height, width, bpp, flags, handle, pitch; uint64_t size; };
struct fixture_dumb_map { uint32_t handle, pad; uint64_t offset; };
struct fixture_dumb_destroy { uint32_t handle; };
#define FIXTURE_DRM_VERSION _IOWR('d', 0x00, struct fixture_drm_version)
#define FIXTURE_DRM_CREATE_DUMB _IOWR('d', 0xB2, struct fixture_dumb_create)
#define FIXTURE_DRM_MAP_DUMB _IOWR('d', 0xB3, struct fixture_dumb_map)
#define FIXTURE_DRM_DESTROY_DUMB _IOW('d', 0xB4, struct fixture_dumb_destroy)

typedef struct {
    int fd, gem;
    uint32_t handle;
    uint64_t offset;
    size_t bytes;
    uint8_t *guest, *alias;
} backing;
static void backing_close(backing *b) {
    if (b->alias) required(!munmap(b->alias, b->bytes), "unmap private alias");
    if (b->guest) required(!munmap(b->guest, b->bytes), "unmap guest");
    if (b->handle) {
        struct fixture_dumb_destroy destroy = {.handle = b->handle};
        required(!ioctl(b->fd, FIXTURE_DRM_DESTROY_DUMB, &destroy), "destroy i915 GEM");
    }
    if (b->fd >= 0) required(!close(b->fd), "close backing fd");
    memset(b, 0, sizeof *b); b->fd = -1;
}
/* Only absent nodes/non-i915 drivers return 77. An existing i915 device's
 * permission/allocation/query/mapping failure is a test failure, never a skip. */
static int backing_open(backing *b, size_t bytes, int gem, int alias) {
    memset(b, 0, sizeof *b); b->fd = -1; b->bytes = bytes; b->gem = gem;
    if (gem) {
        const char *node = getenv("WB_ALIAS_DRM_NODE"); if (!node) node = "/dev/dri/card0";
        b->fd = open(node, O_RDWR | O_CLOEXEC);
        if (b->fd < 0) {
            if (errno == ENOENT) { printf("UNAVAILABLE: no i915 node %s\n", node); return 77; }
            perror("open i915 node"); return 1;
        }
        char driver[32] = {0};
        struct fixture_drm_version version = {.name_len = sizeof driver - 1, .name = driver};
        if (ioctl(b->fd, FIXTURE_DRM_VERSION, &version)) { perror("DRM version"); return 1; }
        if (version.name_len != 4 || strcmp(driver, "i915")) {
            printf("UNAVAILABLE: DRM driver is not i915 (%s)\n", driver); return 77;
        }
        struct fixture_dumb_create create = {.height = (uint32_t)((bytes + 16383) / 16384), .width = 4096, .bpp = 32};
        if (ioctl(b->fd, FIXTURE_DRM_CREATE_DUMB, &create)) { perror("i915 GEM allocation"); return 1; }
        b->handle = create.handle;
        if (create.size < bytes) { fputs("i915 GEM allocation too small\n", stderr); return 1; }
        struct fixture_dumb_map map = {.handle = create.handle};
        if (ioctl(b->fd, FIXTURE_DRM_MAP_DUMB, &map)) { perror("i915 GEM mmap offset"); return 1; }
        b->offset = map.offset;
    } else {
        b->fd = memfd_create("totk-wb-alias-fixture", MFD_CLOEXEC);
        if (b->fd < 0 || ftruncate(b->fd, (off_t)bytes)) { perror("memfd backing"); return 1; }
    }
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, b->fd, (off_t)b->offset);
    if (p == MAP_FAILED) { perror("shared guest mmap"); return 1; } b->guest = p;
    if (alias) {
        p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, b->fd, (off_t)b->offset);
        if (p == MAP_FAILED) { perror("shared alias mmap"); return 1; } b->alias = p;
    }
    return 0;
}
static uint64_t guest_rows(const backing *b) {
    FILE *file = fopen("/proc/self/maps", "re"); required(file != NULL, "open maps");
    char line[1024]; unsigned long lo, hi; uint64_t rows = 0, covered = 0;
    while (fgets(line, sizeof line, file)) {
        if (sscanf(line, "%lx-%lx", &lo, &hi) != 2 || hi <= (uintptr_t)b->guest || lo >= (uintptr_t)b->guest + b->bytes) continue;
        uintptr_t first = lo > (uintptr_t)b->guest ? lo : (uintptr_t)b->guest;
        uintptr_t last = hi < (uintptr_t)b->guest + b->bytes ? hi : (uintptr_t)b->guest + b->bytes;
        ++rows; covered += last - first;
    }
    required(!ferror(file), "read maps"); fclose(file);
    CHECK(covered == b->bytes, "all guest bytes accounted by actual kernel rows"); return rows;
}
static int guest_permissions(const void *address) {
    FILE *file = fopen("/proc/self/maps", "re"); required(file != NULL, "open maps permissions");
    char line[1024], permissions[5]; unsigned long lo, hi; int protection = -1;
    while (fgets(line, sizeof line, file)) {
        if (sscanf(line, "%lx-%lx %4s", &lo, &hi, permissions) != 3 ||
            (uintptr_t)address < lo || (uintptr_t)address >= hi) continue;
        protection = (permissions[0] == 'r' ? PROT_READ : 0) | (permissions[1] == 'w' ? PROT_WRITE : 0); break;
    }
    required(!ferror(file), "read maps permissions"); fclose(file); return protection;
}
