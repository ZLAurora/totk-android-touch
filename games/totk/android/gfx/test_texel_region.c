/* MIT: byte-exact oracle is the existing whole-level decoder/encoder, not a
   second block-linear address implementation. No Vulkan or game files needed. */
#include "../../third_party/d2r_gfx/gfx/tex/nvn_deswizzle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static unsigned checks, failures, cases, spans;
static uint32_t seed = 0x8719a325;
static uint32_t random_u32(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }
#define CHECK(c) do { checks++; if (!(c)) { if (failures++ < 12) fprintf(stderr, "FAIL line %d case %u\n", __LINE__, cases); } } while (0)
static uint32_t mip(uint32_t n, unsigned l) { n >>= l; return n ? n : 1; }
static uint64_t now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec; }
static void *alloc(size_t n) { void *p = malloc(n); if (!p) { perror("malloc"); exit(2); } return p; }

/* Each owner is a distinct allocation; dst_span is never rebased to pretend it
   contains the whole texture. Compare every span, including padding, against
   the original full encoder before assembling the complete storage image. */
static void swizzle_span_case(nvn_texdesc d) {
    enum { GUARD = 19 };
    static const size_t edges[] = {1,3,15,16,17,31,32,33,63,64,65,127,128,129,255,256,257,511,512,513,1023,1024,1025};
    nvn_texlayout layout; nvn_tex_layout(&d, &layout);
    size_t storage_size = (size_t)layout.size + 32;
    uint8_t *expected = alloc(storage_size), *split = alloc(storage_size), guard[GUARD];
    memset(guard, 0xa7, sizeof guard);
    for (size_t i = 0; i < storage_size; i++) expected[i] = (uint8_t)random_u32();
    memcpy(split, expected, storage_size);
    int bw, bh, bpb = nvn_fmt_bpb(d.format, &bw, &bh);
    for (unsigned l = 0; l < d.levels; l++) for (unsigned layer = 0; layer < layout.layers; layer++) {
        cases++;
        uint32_t row = (mip(d.width, l) + bw - 1) / bw * bpb;
        uint32_t rows = d.target == 0 || d.target == 3 ? 1 : (mip(d.height, l) + bh - 1) / bh;
        uint32_t slices = d.target == 2 ? mip(d.depth, l) : 1;
        size_t pitch = row + 7, linear_size = pitch * rows * slices + 32;
        uint8_t *src = alloc(linear_size), *before = alloc(linear_size);
        for (size_t i = 0; i < linear_size; i++) src[i] = (uint8_t)random_u32();
        memcpy(before, src, linear_size);
        size_t level_offset = (size_t)nvn_tex_level_offset(&d, l);
        size_t base = (size_t)layer * layout.layer_size + level_offset;
        size_t next = l + 1 < d.levels ? (size_t)nvn_tex_level_offset(&d, l + 1) : (size_t)layout.layer_size;
        /* Include real inter-level/layer padding and bytes beyond this level.
           Even the last layer has another 32 guarded storage bytes available. */
        size_t extent = next - level_offset + 23;
        uint8_t *whole = alloc(extent + 2 * GUARD);
        memset(whole, 0xa7, extent + 2 * GUARD);
        memcpy(whole + GUARD, split + base, extent);
        CHECK(!nvn_swizzle(&d, l, layer, src, pitch, expected));
        for (size_t offset = 0, edge = 0; offset < extent;) {
            size_t end = edge < sizeof edges / sizeof *edges ? edges[edge++] : offset + 1 + random_u32() % 1061;
            if (end > extent) end = extent;
            size_t n = end - offset;
            uint8_t *owner = alloc(n + 2 * GUARD);
            memset(owner, 0xa7, n + 2 * GUARD);
            memcpy(owner + GUARD, split + base + offset, n);
            CHECK(!nvn_swizzle_span_into(&d, l, layer, src, pitch, owner + GUARD, offset, n));
            CHECK(!memcmp(owner + GUARD, expected + base + offset, n));
            CHECK(!memcmp(owner, guard, GUARD) && !memcmp(owner + GUARD + n, guard, GUARD));
            memcpy(split + base + offset, owner + GUARD, n);
            free(owner);
            spans++;
            offset = end;
        }
        CHECK(!memcmp(split, expected, storage_size));
        /* A single span with a saturating count must stop at the actual texels,
           leaving trailing alignment, other levels/layers, and guards intact. */
        uint8_t first_byte = whole[GUARD];
        CHECK(!nvn_swizzle_span_into(&d, l, layer, src, pitch, whole + GUARD + 1, 1, UINT64_MAX));
        CHECK(whole[GUARD] == first_byte);
        CHECK(!memcmp(whole + GUARD + 1, expected + base + 1, extent - 1));
        CHECK(!nvn_swizzle_span_into(&d, l, layer, src, pitch, whole + GUARD, 0, UINT64_MAX));
        CHECK(!memcmp(whole + GUARD, expected + base, extent));
        CHECK(!memcmp(whole, guard, GUARD) && !memcmp(whole + GUARD + extent, guard, GUARD));
        CHECK(!nvn_swizzle_span_into(&d, l, layer, src, pitch, whole + GUARD, 1, 0));
        CHECK(!nvn_swizzle_span_into(&d, l, layer, src, pitch, whole + GUARD, UINT64_MAX, UINT64_MAX));
        CHECK(!memcmp(whole + GUARD, expected + base, extent));
        CHECK(nvn_swizzle_span_into(&d, l, layer, src, row - 1, whole + GUARD, 0, extent) == -1);
        CHECK(nvn_swizzle_span_into(&d, l, layout.layers, src, pitch, whole + GUARD, 0, extent) == -1);
        CHECK(nvn_swizzle_span_into(&d, d.levels, layer, src, pitch, whole + GUARD, 0, extent) == -1);
        CHECK(!memcmp(whole + GUARD, expected + base, extent));
        CHECK(!memcmp(whole, guard, GUARD) && !memcmp(whole + GUARD + extent, guard, GUARD));
        if (!nvn_tex_is_linear(&d)) {
            /* The old whole-storage span API must still reject partial GOBs. */
            CHECK(nvn_swizzle_span(&d, l, layer, src, pitch, split, 1, 512) == -1);
            CHECK(nvn_swizzle_span(&d, l, layer, src, pitch, split, 0, 511) == -1);
            CHECK(!memcmp(split, expected, storage_size));
        }
        CHECK(!memcmp(src, before, linear_size));
        free(whole); free(src); free(before);
    }
    free(expected); free(split);
}

static void texture_case(nvn_texdesc d) {
    nvn_texlayout layout; nvn_tex_layout(&d, &layout);
    size_t storage_size = (size_t)layout.size + 32;
    uint8_t *old = alloc(storage_size), *region = alloc(storage_size);
    for (size_t i = 0; i < storage_size; i++) old[i] = (uint8_t)random_u32();
    memcpy(region, old, storage_size);
    int bw, bh, bpb = nvn_fmt_bpb(d.format, &bw, &bh);
    for (unsigned l = 0; l < d.levels; l++) for (unsigned layer = 0; layer < layout.layers; layer++) {
        uint32_t row = (mip(d.width, l) + bw - 1) / bw * bpb;
        uint32_t rows = d.target == 0 || d.target == 3 ? 1 : (mip(d.height, l) + bh - 1) / bh;
        uint32_t slices = d.target == 2 ? mip(d.depth, l) : 1;
        size_t level_size = (size_t)row * rows * slices;
        uint8_t *linear = alloc(level_size);
        for (unsigned trial = 0; trial < 6; trial++) {
            cases++;
            /* Full, last byte, both GOB edges, and randomized byte-aligned
               regions (including boundaries inside 3/6/12-byte blocks). */
            uint32_t x = random_u32() % row, y = random_u32() % rows, z = random_u32() % slices;
            uint32_t w = 1 + random_u32() % (row - x), h = 1 + random_u32() % (rows - y), depth = 1 + random_u32() % (slices - z);
            if (!trial) { x = y = z = 0; w = row; h = rows; depth = slices; }
            if (trial == 1) { x = row - 1; y = rows - 1; z = slices - 1; w = h = depth = 1; }
            if (trial == 2 && row > 70 && rows > 9) { x = 61; y = 7; w = 9; h = 3; }
            size_t pitch = w + trial * 3, image = pitch * h + trial * 5, size = image * depth + 32;
            uint8_t *src = alloc(size), *out = alloc(size), *expected = alloc(size);
            for (size_t i = 0; i < size; i++) src[i] = (uint8_t)random_u32();
            memset(out, 0xc7, size); memset(expected, 0xc7, size);
            CHECK(!nvn_deswizzle(&d, l, layer, old, linear, row));
            for (uint32_t zz = 0; zz < depth; zz++) for (uint32_t yy = 0; yy < h; yy++)
                memcpy(linear + ((size_t)(z + zz) * rows + y + yy) * row + x, src + (size_t)zz * image + (size_t)yy * pitch, w);
            CHECK(!nvn_swizzle(&d, l, layer, linear, row, old));
            CHECK(!nvn_swizzle_region(&d, l, layer, x, y, z, w, h, depth, src, pitch, image, region));
            /* Entire allocation: untouched texels, other mips/layers, storage
               padding and guard bytes must all remain identical. */
            CHECK(!memcmp(old, region, storage_size));
            CHECK(!nvn_deswizzle(&d, l, layer, old, linear, row));
            for (uint32_t zz = 0; zz < depth; zz++) for (uint32_t yy = 0; yy < h; yy++)
                memcpy(expected + (size_t)zz * image + (size_t)yy * pitch, linear + ((size_t)(z + zz) * rows + y + yy) * row + x, w);
            CHECK(!nvn_deswizzle_region(&d, l, layer, x, y, z, w, h, depth, region, out, pitch, image));
            CHECK(!memcmp(out, expected, size));
            free(src); free(out); free(expected);
        }
        free(linear);
    }
    free(old); free(region);
}

static int bench(void) {
    const nvn_texdesc d = {.target=1, .format=0x25, .width=2048, .height=2048, .depth=1, .levels=1};
    nvn_texlayout layout; nvn_tex_layout(&d, &layout);
    const unsigned n = 40, w = 32 * 4, h = 32, x = 1007 * 4, y = 999;
    uint8_t *old = alloc(layout.size), *region = alloc(layout.size), src[w * h];
    memset(old, 0x63, layout.size); memcpy(region, old, layout.size); memset(src, 0xb9, sizeof src);
    uint64_t old_ns = now();
    for (unsigned i = 0; i < n; i++) {
        uint8_t *linear = alloc(2048 * 2048 * 4);
        if (nvn_deswizzle(&d, 0, 0, old, linear, 2048 * 4)) return 2;
        for (unsigned yy = 0; yy < h; yy++) memcpy(linear + (y + yy) * 2048 * 4 + x, src + yy * w, w);
        if (nvn_swizzle(&d, 0, 0, linear, 2048 * 4, old)) return 2;
        free(linear);
    }
    old_ns = now() - old_ns;
    uint64_t new_ns = now();
    for (unsigned i = 0; i < n; i++) if (nvn_swizzle_region(&d, 0, 0, x, y, 0, w, h, 1, src, w, sizeof src, region)) return 2;
    new_ns = now() - new_ns;
    CHECK(!memcmp(old, region, layout.size));
    printf("texel-region-bench: RGBA8 2048x2048 region=(1007,999):32x32 iterations=%u old=%.3f us/call region=%.3f us/call speedup=%.1fx old_convert=33554432 bytes/call region_convert=4096 bytes/call exact=%s\n",
        n, old_ns / (1000.0 * n), new_ns / (1000.0 * n), (double)old_ns / new_ns, failures ? "no" : "yes");
    free(old); free(region); return !!failures;
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--bench")) return bench();
    unsigned formats = 0, bpb_mask = 0;
    const int targets[] = {0, 1, 2, 3, 4, 7, 8, 9};
    for (int fmt = 1; fmt <= 0x9c; fmt++) {
        int bw, bh, bpb = nvn_fmt_bpb(fmt, &bw, &bh);
        if (!bpb) continue;
        formats++; bpb_mask |= 1u << bpb;
        for (unsigned target = 0; target < sizeof targets / sizeof *targets; target++) for (unsigned variant = 0; variant < 3; variant++) {
            nvn_texdesc d = {.target=targets[target], .format=fmt, .width=69u*bw-1, .height=33u*bh-1, .depth=5, .levels=3};
            if (d.target == 3) d.height = 3;
            if (d.target == 8 || d.target == 9) { d.width = d.height = 33; d.depth = d.target == 9 ? 12 : 6; }
            if (variant == 1) d.pkg_block_h = 0x23 + 1; /* packaged H3/D2 before per-mip shrink */
            if (variant == 2) d.flags = 0x200; /* minimal layout */
            texture_case(d);
        }
        for (unsigned v = 0; v < 2; v++) texture_case((nvn_texdesc){.target=1, .format=fmt, .width=69u*bw-1, .height=33u*bh-1,
            .depth=1, .levels=1, .flags=v ? 0x40 : 0x10});
        swizzle_span_case((nvn_texdesc){.target=4, .format=fmt, .width=69u*bw-1, .height=19u*bh-1,
            .depth=3, .levels=3, .pkg_block_h=3});
        swizzle_span_case((nvn_texdesc){.target=2, .format=fmt, .width=23u*bw-1, .height=17u*bh-1,
            .depth=5, .levels=3, .pkg_block_h=0x23});
        swizzle_span_case((nvn_texdesc){.target=3, .format=fmt, .width=69u*bw-1, .height=3, .depth=1, .levels=3});
        for (unsigned v = 0; v < 2; v++) swizzle_span_case((nvn_texdesc){.target=1, .format=fmt,
            .width=23u*bw-1, .height=11u*bh-1, .depth=1, .levels=1, .flags=v ? 0x40 : 0x10,
            .stride=v ? 0 : (uint32_t)(23 * bpb + 13)});
    }
    /* Keep tall/deep packaged GOB variants from collapsing to small mip tiles. */
    for (unsigned th = 0; th <= 5; th++) for (unsigned td = 0; td <= 4; td++)
        texture_case((nvn_texdesc){.target=2, .format=0x19, .width=23, .height=259, .depth=17, .levels=3, .pkg_block_h=(int)((td << 4) | th) + 1});
    uint8_t storage[4096], before[4096], src[64]; memset(storage, 0xa7, sizeof storage); memcpy(before, storage, sizeof storage);
    nvn_texdesc invalid = {.target=1, .format=0x25, .width=16, .height=16, .depth=1, .levels=1};
    CHECK(nvn_swizzle_region(&invalid,0,0,63,0,0,2,1,1,src,64,64,storage) == -1);
    CHECK(!memcmp(storage,before,sizeof storage));
    invalid.samples=4; CHECK(nvn_swizzle_region(&invalid,0,0,0,0,0,4,1,1,src,64,64,storage) == -1);
    CHECK(!memcmp(storage,before,sizeof storage));
    printf("%s: texel region %u formats, supported bpb mask=%#x (1,2,3,4,6,8,12,16), %u cases, %u independent swizzle spans, %u checks, %u failed\n",
        failures ? "FAILED" : "ok", formats, bpb_mask, cases, spans, checks, failures);
    return !!failures;
}
