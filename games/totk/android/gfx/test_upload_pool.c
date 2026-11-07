/* Parallel upload deswizzle (vk_upload_pool.c) against the serial decoder: every staging byte, including row padding
   and guard bytes, must equal nvn_deswizzle's for 0, 1 and 3 helpers, forced splits and the default threshold, and
   while another thread reconfigures the helpers as a live TOTK_VK_UPLOAD_THREADS does. No Vulkan or game files.
   Run under CC='clang -fsanitize=thread' for the pool's race check.
   --bench: serial vs 1/3 helper wall time for large uploads (host numbers, not a device measurement). */
#include "vk_upload_pool.h"
#include "../../third_party/d2r_gfx/gfx/tex/nvn_deswizzle.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static unsigned checks, failures, cases;
static uint32_t seed = 0x5a17c0de;
static uint32_t random_u32(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }
#define CHECK(c, ...) do { checks++; if (!(c)) { if (failures++ < 16) { fprintf(stderr, "FAIL line %d case %u: ", __LINE__, cases); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } } while (0)
static uint32_t mip(uint32_t n, unsigned l) { n >>= l; return n ? n : 1; }
static uint64_t now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec; }
static void *alloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) { perror("malloc"); exit(2); } return p; }

typedef struct { nvn_texdesc d; nvn_texlayout L; uint8_t *storage; unsigned nsubs; upload_sub subs[256]; size_t offsets[256], total; } upload;
/* Staging laid out as vk_res.c upload_inner (16-byte aligned subresources), pitch padded by `pad` bytes. */
static void upload_init(upload *u, nvn_texdesc d, size_t pad) {
    memset(u, 0, sizeof *u); u->d = d; nvn_tex_layout(&d, &u->L);
    u->storage = alloc((size_t)u->L.size);
    for (size_t i = 0; i < u->L.size; i++) u->storage[i] = (uint8_t)random_u32();
    int bw, bh, bpb = nvn_fmt_bpb(d.format, &bw, &bh);
    for (unsigned l = 0; l < d.levels; l++) {
        uint32_t w = mip(d.width, l), h = mip(d.height, l), z = d.target == 2 ? mip(d.depth, l) : 1;
        size_t pitch = (size_t)((w + bw - 1) / bw) * bpb + pad, rows = (h + bh - 1) / bh;
        for (unsigned layer = 0; layer < (d.target == 2 ? 1u : u->L.layers) && u->nsubs < 256; layer++) {
            u->subs[u->nsubs] = (upload_sub){l, layer, NULL, pitch}; u->offsets[u->nsubs++] = u->total;
            u->total += (pitch * rows * z + 15) & ~(size_t)15;
        }
    }
}
static uint8_t *staging(const upload *u, upload_sub *subs, uint8_t poison) {
    uint8_t *s = alloc(u->total + 64); memset(s, poison, u->total + 64);
    for (unsigned i = 0; i < u->nsubs; i++) { subs[i] = u->subs[i]; subs[i].dst = s + u->offsets[i]; }
    return s;
}
static uint8_t *serial(const upload *u, uint8_t poison) {
    upload_sub subs[256]; uint8_t *s = staging(u, subs, poison);
    for (unsigned i = 0; i < u->nsubs; i++) CHECK(!nvn_deswizzle(&u->d, subs[i].level, subs[i].layer, u->storage, subs[i].dst, subs[i].pitch), "serial decode");
    return s;
}
static void compare(const upload *u, int helpers, size_t min_bytes, const char *what) {
    upload_pool_configure(helpers);
    for (unsigned pass = 0; pass < 2; pass++) {   /* two poisons: a byte no run wrote cannot match both */
        uint8_t poison = pass ? 0x38 : 0xc7, *expected = serial(u, poison);
        upload_sub subs[256]; uint8_t *out = staging(u, subs, poison);
        CHECK(!upload_pool_deswizzle(&u->d, u->storage, subs, u->nsubs, min_bytes), "%s: pool decode", what);
        CHECK(!memcmp(out, expected, u->total + 64), "%s: helpers=%d min=%zu fmt=%#x target=%d %ux%ux%u levels=%u: staging differs",
              what, helpers, min_bytes, u->d.format, u->d.target, u->d.width, u->d.height, u->d.depth, u->d.levels);
        free(out); free(expected);
    }
}
static void texture_case(nvn_texdesc d) {
    for (size_t pad = 0; pad <= 5; pad += 5) {
        upload u; upload_init(&u, d, pad); cases++;
        static const int helpers[] = {0, 1, 3};
        for (unsigned h = 0; h < 3; h++) {
            compare(&u, helpers[h], 0, "forced split");
            compare(&u, helpers[h], UPLOAD_POOL_MIN_BYTES, "default threshold");
        }
        free(u.storage);
    }
}
static void threshold_case(void) {
    /* Below the threshold nothing is split; at it the runs are counted and some run on helpers eventually. */
    upload small, large; upload_pool_stats s;
    upload_init(&small, (nvn_texdesc){.target=1, .format=0x25, .width=128, .height=128, .depth=1, .levels=1}, 0);   /* 64 KiB */
    upload_init(&large, (nvn_texdesc){.target=1, .format=0x25, .width=1024, .height=1024, .depth=1, .levels=1}, 0); /* 4 MiB */
    upload_pool_configure(3); upload_pool_stats_take(&s);
    upload_sub subs[256]; uint8_t *out = staging(&small, subs, 0);
    CHECK(!upload_pool_deswizzle(&small.d, small.storage, subs, small.nsubs, UPLOAD_POOL_MIN_BYTES), "small decode");
    upload_pool_stats_take(&s); CHECK(!s.jobs && !s.runs, "64 KiB upload stays serial (jobs %llu)", (unsigned long long)s.jobs);
    free(out);
    uint64_t helper_runs = 0;
    uint32_t row_bytes, rows, slices, bpb; uint64_t base; unsigned th, td;
    CHECK(!nvn_deswizzle_geom(&large.d, 0, 0, &row_bytes, &rows, &slices, &base, &th, &td, &bpb), "large geometry");
    uint64_t block_row = (row_bytes + 63) / 64 * (UINT64_C(512) << (th + td)), block_rows = (rows + (8u << th) - 1) / (8u << th);
    uint64_t per_run = (UPLOAD_POOL_RUN_BYTES + block_row - 1) / block_row, runs = (block_rows + per_run - 1) / per_run;
    for (unsigned i = 0; i < 64; i++) {
        out = staging(&large, subs, 0);
        CHECK(!upload_pool_deswizzle(&large.d, large.storage, subs, large.nsubs, UPLOAD_POOL_MIN_BYTES), "large decode");
        upload_pool_stats_take(&s); helper_runs += s.helper_runs;
        CHECK(s.jobs == 1 && s.runs == runs && runs >= 2, "4 MiB RGBA8 splits into %llu runs of whole block rows (jobs %llu runs %llu)",
              (unsigned long long)runs, (unsigned long long)s.jobs, (unsigned long long)s.runs);
        free(out);
    }
    CHECK(helper_runs > 0, "helpers claimed runs of the large uploads");
    upload_pool_configure(0);
    out = staging(&large, subs, 0);
    CHECK(!upload_pool_deswizzle(&large.d, large.storage, subs, large.nsubs, 0), "serial decode with helpers off");
    upload_pool_stats_take(&s); CHECK(!s.jobs, "no helpers configured: no parallel job");
    free(out); free(small.storage); free(large.storage);
    nvn_texdesc ms = {.target=1, .format=0x25, .width=1024, .height=1024, .depth=1, .levels=1, .samples=4};
    upload_sub one = {0, 0, NULL, 4096};
    for (int h = 0; h <= 3; h += 3) { upload_pool_configure(h); CHECK(upload_pool_deswizzle(&ms, NULL, &one, 1, 0) == -1, "multisampled storage is rejected (helpers %d)", h); }
}
static int bench(void) {
    static const struct { const char *name; nvn_texdesc d; } textures[] = {
        {"RGBA8 2048x2048 mips", {.target=1, .format=0x25, .width=2048, .height=2048, .depth=1, .levels=12}},
        {"RGBA8 1024x1024x6 cube mips", {.target=8, .format=0x25, .width=1024, .height=1024, .depth=6, .levels=11}},
        {"RGBA8 512x512 (1 MiB)", {.target=1, .format=0x25, .width=512, .height=512, .depth=1, .levels=1}},
        {"RGBA8 256x256 (256 KiB)", {.target=1, .format=0x25, .width=256, .height=256, .depth=1, .levels=1}},
    };
    for (unsigned t = 0; t < sizeof textures / sizeof *textures; t++) {
        upload u; upload_init(&u, textures[t].d, 0);
        upload_sub subs[256]; uint8_t *out = staging(&u, subs, 0);
        double ms[3]; static const int helpers[] = {0, 1, 3};
        for (unsigned h = 0; h < 3; h++) {
            upload_pool_configure(helpers[h]);
            upload_pool_deswizzle(&u.d, u.storage, subs, u.nsubs, UPLOAD_POOL_MIN_BYTES);   /* start helpers, warm caches */
            unsigned n = 20; uint64_t t0 = now();
            for (unsigned i = 0; i < n; i++) upload_pool_deswizzle(&u.d, u.storage, subs, u.nsubs, UPLOAD_POOL_MIN_BYTES);
            ms[h] = (now() - t0) / (1e6 * n);
        }
        printf("upload-pool-bench: %s staging=%.2f MiB serial=%.3f ms helpers1=%.3f ms helpers3=%.3f ms\n",
               textures[t].name, u.total / 1048576.0, ms[0], ms[1], ms[2]);
        free(out); free(u.storage);
    }
    return 0;
}
/* A live TOTK_VK_UPLOAD_THREADS (vk_res.c upload_threads_refresh) reconfigures the pool between uploads, from any
   uploading thread. Here another thread toggles 0..3 helpers while uploads run: every decode stays byte-identical. */
static _Atomic int live_stop;
static void *live_toggler(void *arg) {
    (void)arg; uint32_t s = 0x1234567u; unsigned long n = 0;
    while (!atomic_load(&live_stop)) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; upload_pool_configure((int)(s % 4)); n++; usleep(200); }
    return (void *)(uintptr_t)n;
}
static void live_case(void) {
    static upload large; upload_init(&large, (nvn_texdesc){.target=1, .format=0x25, .width=1024, .height=1024, .depth=1, .levels=2}, 0);
    uint8_t *expected = serial(&large, 0x5a);
    upload_sub subs[256]; upload_pool_stats s; upload_pool_stats_take(&s);
    pthread_t t; pthread_create(&t, NULL, live_toggler, NULL);
    for (unsigned i = 0; i < 200; i++) {
        uint8_t *out = staging(&large, subs, 0x5a);
        CHECK(!upload_pool_deswizzle(&large.d, large.storage, subs, large.nsubs, UPLOAD_POOL_MIN_BYTES), "live decode %u", i);
        CHECK(!memcmp(out, expected, large.total + 64), "live toggles: decode %u differs", i);
        free(out);
    }
    atomic_store(&live_stop, 1); void *toggles; pthread_join(t, &toggles);
    upload_pool_stats_take(&s);
    CHECK(s.jobs > 0, "some uploads ran on helpers while toggled (%llu jobs)", (unsigned long long)s.jobs);
    printf("live: 200 uploads byte-identical while the helpers were reconfigured %lu times (%llu parallel)\n",
           (unsigned long)(uintptr_t)toggles, (unsigned long long)s.jobs);
    upload_pool_configure(0);
    free(expected); free(large.storage);
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--bench")) return bench();
    /* One format per (bytes per block, compressed) class: 1/2/4/8/12/16-byte and block-compressed formats. */
    int formats[32], nformats = 0; uint64_t seen = 0;
    for (int fmt = 1; fmt <= 0x9c && nformats < 32; fmt++) {
        int bw, bh, bpb = nvn_fmt_bpb(fmt, &bw, &bh);
        uint64_t key = bpb ? UINT64_C(1) << (bpb + (bw > 1 ? 32 : 0)) : 0;
        if (!bpb || (seen & key)) continue;
        seen |= key; formats[nformats++] = fmt;
    }
    for (int f = 0; f < nformats; f++) {
        int bw, bh; nvn_fmt_bpb(formats[f], &bw, &bh);
        texture_case((nvn_texdesc){.target=1, .format=formats[f], .width=1, .height=1, .depth=1, .levels=1});
        texture_case((nvn_texdesc){.target=1, .format=formats[f], .width=13u*bw, .height=7u*bh, .depth=1, .levels=3});
        texture_case((nvn_texdesc){.target=1, .format=formats[f], .width=300u*bw-1, .height=200u*bh-1, .depth=1, .levels=9});
        texture_case((nvn_texdesc){.target=4, .format=formats[f], .width=96u*bw, .height=80u*bh, .depth=4, .levels=4});
        texture_case((nvn_texdesc){.target=8, .format=formats[f], .width=128, .height=128, .depth=6, .levels=8});
        texture_case((nvn_texdesc){.target=2, .format=formats[f], .width=64u*bw, .height=48u*bh, .depth=24, .levels=4});
        texture_case((nvn_texdesc){.target=1, .format=formats[f], .width=1024u*bw, .height=256u*bh, .depth=1, .levels=2, .pkg_block_h=0x03 + 1});
    }
    /* Deep packaged 3D blocks: one block row spans several slices. */
    for (unsigned td = 0; td <= 4; td += 2)
        texture_case((nvn_texdesc){.target=2, .format=0x25, .width=200, .height=130, .depth=37, .levels=3, .pkg_block_h=(int)((td << 4) | 2) + 1});
    threshold_case();
    live_case();
    printf("%s: upload pool %d formats, %u textures, %u checks, %u failed\n", failures ? "FAILED" : "ok", nformats, cases, checks, failures);
    return !!failures;
}
