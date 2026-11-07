// Tegra X1 块线性排布（公开描述见 deko3d / Ryujinx BlockLinearLayout）：
//   GOB = 64 字节 × 8 行 = 512 字节；GOB 内 (x 字节, y 行) 的偏移 =
//     (x%64/32)·256 + (y%8/2)·64 + (x%32/16)·32 + (y%2)·16 + x%16
//   块 = 1 个 GOB 宽 × 2^tileH 个 GOB 高 × 2^tileD 个 GOB 深，块内先排 y 再排 z；块按 x → y → z 行优先铺满本级。
// 级 / 层起点与本级实际块高块深取自 nvn_tex.c，这里只做级内寻址与搬运。
// Tegra X1 block-linear layout (public descriptions: deko3d / Ryujinx BlockLinearLayout):
//   GOB = 64 bytes × 8 rows = 512 bytes; offset of (x byte, y row) within a GOB =
//     (x%64/32)·256 + (y%8/2)·64 + (x%32/16)·32 + (y%2)·16 + x%16
//   block = 1 GOB wide × 2^tileH GOBs high × 2^tileD GOBs deep, ordered y first then z within a block; blocks fill the level in x → y → z row-major order.
// Level / layer start offsets and the actual block height and block depth of this level come from nvn_tex.c; here we only do intra-level addressing and copying.
#include "nvn_deswizzle.h"
#include <string.h>

static uint32_t gob_off(uint32_t x, uint32_t y) {
    return (x % 64 / 32) * 256 + (y % 8 / 2) * 64 + (x % 32 / 16) * 32 + (y % 2) * 16 + x % 16;
}

uint64_t nvn_bl_offset(uint32_t x, uint32_t y, uint32_t z, uint32_t row_bytes, uint32_t rows, unsigned th, unsigned td) {
    uint32_t bh = 1u << th, bd = 1u << td;
    uint64_t gobs_x = (row_bytes + 63) / 64, blocks_y = (rows + 8 * bh - 1) / (8 * bh);
    uint64_t block = 512ull * bh * bd;                                  // 一个块的字节 | bytes in one block
    return (z / bd) * gobs_x * blocks_y * block                        // 第几层块（深度方向） | which block layer (along depth)
         + (y / (8 * bh)) * gobs_x * block                             // 第几行块 | which block row
         + (x / 64) * block                                            // 第几列块 | which block column
         + ((z % bd) * bh + (y % (8 * bh)) / 8) * 512ull               // 块内第几个 GOB | which GOB within the block
         + gob_off(x, y);
}

static uint32_t mip(uint32_t s, unsigned l) { s >>= l; return s ? s : 1; }

// 本级几何：块宽字节数、块行数、切片数，以及本级本层在存储里的起点；不支持的贴图返回 -1
// Geometry of this level: width-in-blocks byte count, block row count, slice count, and where this level and layer starts in storage; returns -1 for unsupported textures
int nvn_deswizzle_geom(const nvn_texdesc *d, unsigned level, unsigned layer,
                       uint32_t *row_bytes, uint32_t *rows, uint32_t *slices, uint64_t *base, unsigned *th, unsigned *td, uint32_t *bpb) {
    if (d->target == 5 || d->target == 6 || d->target == 10 || nvn_tex_is_linear(d) || d->samples > 1) return -1;   // MS / BUFFER / LINEAR
    int bw, bh; *bpb = (uint32_t)nvn_fmt_bpb(d->format, &bw, &bh);
    if (!*bpb || level >= (d->levels ? d->levels : 1)) return -1;
    nvn_texlayout L; nvn_tex_layout(d, &L);
    if (layer >= L.layers) return -1;
    int one_d = d->target == 0 || d->target == 3;
    uint32_t w = mip(d->width, level), h = one_d ? 1 : mip(d->height, level);
    *slices = d->target == 2 ? mip(d->depth ? d->depth : 1, level) : 1;
    *row_bytes = (w + bw - 1) / bw * *bpb; *rows = (h + bh - 1) / bh;
    *base = (uint64_t)layer * L.layer_size + nvn_tex_level_offset(d, level);
    nvn_tex_level_tiles(d, level, th, td);
    return 0;
}

/* Storage-order traversal lets the same engine move a bounded contiguous guest span. Padding is never copied.
   Full decode/encode use the entire span; settled write-back resumes at GOB boundaries without a second swizzler. */
static int move(const nvn_texdesc *d, unsigned level, unsigned layer, uint8_t *bl, uint8_t *lin, size_t pitch,
                int to_linear, uint64_t first, uint64_t count) {
    if (nvn_tex_is_linear(d)) {
        if (level || layer || d->samples > 1 || (d->target != 0 && d->target != 1 && d->target != 7)) return -1;
        int bw, bh, bpb = nvn_fmt_bpb(d->format, &bw, &bh);
        if (!bpb) return -1;
        size_t row = (size_t)((d->width + bw - 1) / bw) * bpb;
        uint32_t rows = d->target == 0 ? 1 : (d->height + bh - 1) / bh, stride = nvn_tex_linear_stride(d);
        if (pitch < row || stride < row) return -1;
        uint64_t total = (uint64_t)stride * rows;
        if (first >= total) return 0;
        uint64_t end = count < total - first ? first + count : total;
        for (uint64_t y = first / stride; y < rows && y * stride < end; y++) {
            uint64_t lo = first > y * stride ? first - y * stride : 0;
            uint64_t hi = end - y * stride < row ? end - y * stride : row;
            if (lo >= hi) continue;
            if (to_linear) memcpy(lin + y * pitch + lo, bl + y * stride + lo, (size_t)(hi - lo));
            else memcpy(bl + y * stride + lo, lin + y * pitch + lo, (size_t)(hi - lo));
        }
        return 0;
    }
    uint32_t row_bytes, rows, slices, bpb; uint64_t base; unsigned th, td;
    if (nvn_deswizzle_geom(d, level, layer, &row_bytes, &rows, &slices, &base, &th, &td, &bpb)) return -1;
    if (pitch < row_bytes) return -1;
    if (first % 512 || (count != UINT64_MAX && count % 512)) return -1;
    uint64_t bh = UINT64_C(1) << th, bd = UINT64_C(1) << td, gx = (row_bytes + 63) / 64;
    uint64_t by = (rows + 8 * bh - 1) / (8 * bh), bz = (slices + bd - 1) / bd;
    uint64_t total = gx * by * bz * bh * bd * 512;
    if (first >= total) return 0;
    uint64_t end = count < total - first ? first + count : total;
    bl += base;
    for (uint64_t gob = first / 512; gob < end / 512; gob++) {
        uint64_t block = gob / (bh * bd), within = gob % (bh * bd);
        uint32_t x = (uint32_t)(block % gx * 64), y = (uint32_t)(block / gx % by * 8 * bh + within % bh * 8);
        uint32_t z = (uint32_t)(block / (gx * by) * bd + within / bh);
        if (z >= slices) continue;
        for (uint32_t yy = 0; yy < 8 && y + yy < rows; yy++) {
            uint8_t *row = lin + ((size_t)z * rows + y + yy) * pitch;
            for (uint32_t xx = 0; xx < 64 && x + xx < row_bytes; xx += 16) {
                uint32_t n = row_bytes - x - xx < 16 ? row_bytes - x - xx : 16;
                uint8_t *p = bl + gob * 512 + gob_off(xx, yy);
                if (to_linear) memcpy(row + x + xx, p, n); else memcpy(p, row + x + xx, n);
            }
        }
    }
    return 0;
}

int nvn_deswizzle(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src, void *dst, size_t dst_pitch) {
    return move(d, level, layer, (uint8_t *)src, (uint8_t *)dst, dst_pitch, 1, 0, UINT64_MAX);
}
int nvn_deswizzle_span(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src, void *dst, size_t dst_pitch,
                       uint64_t offset, uint64_t size) {
    return move(d, level, layer, (uint8_t *)src, (uint8_t *)dst, dst_pitch, 1, offset, size);
}
int nvn_swizzle(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src, size_t src_pitch, void *dst) {
    return move(d, level, layer, (uint8_t *)dst, (uint8_t *)src, src_pitch, 0, 0, UINT64_MAX);
}
int nvn_swizzle_span(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src, size_t pitch,
                     void *dst, uint64_t offset, uint64_t size) {
    return move(d, level, layer, (uint8_t *)dst, (uint8_t *)src, pitch, 0, offset, size);
}

/* Traverse only the requested rows, splitting at the GOB's contiguous 16-byte
   runs. Byte coordinates also cover 3/6/12-byte formats straddling those runs. */
static int move_region(const nvn_texdesc *d, unsigned level, unsigned layer,
                       uint32_t x, uint32_t y, uint32_t z, uint32_t width, uint32_t height, uint32_t depth,
                       uint8_t *bl, uint8_t *lin, size_t pitch, size_t image_pitch, int to_linear) {
    uint32_t row_bytes, rows, slices, bpb, stride = 0; uint64_t base; unsigned th = 0, td = 0;
    if (nvn_tex_is_linear(d)) {
        if (level || layer || d->samples > 1 || (d->target != 0 && d->target != 1 && d->target != 7)) return -1;
        int bw, bh; bpb = (uint32_t)nvn_fmt_bpb(d->format, &bw, &bh);
        if (!bpb) return -1;
        row_bytes = (d->width + bw - 1) / bw * bpb;
        rows = d->target == 0 ? 1 : (d->height + bh - 1) / bh;
        slices = 1; base = 0; stride = nvn_tex_linear_stride(d);
        if (stride < row_bytes) return -1;
    } else if (nvn_deswizzle_geom(d, level, layer, &row_bytes, &rows, &slices, &base, &th, &td, &bpb)) return -1;
    if (x > row_bytes || width > row_bytes - x || y > rows || height > rows - y ||
        z > slices || depth > slices - z || pitch < width ||
        (height && pitch > SIZE_MAX / height) || image_pitch < pitch * height) return -1;
    bl += base;
    uint64_t block_bytes = UINT64_C(512) << (th + td);
    for (uint32_t zz = 0; zz < depth; zz++) for (uint32_t yy = 0; yy < height; yy++) {
        uint8_t *row = lin + (size_t)zz * image_pitch + (size_t)yy * pitch;
        if (stride) {
            uint8_t *p = bl + (size_t)(y + yy) * stride + x;
            if (to_linear) memcpy(row, p, width); else memcpy(p, row, width);
        } else {
            uint8_t *gob_row = bl + nvn_bl_offset(0, y + yy, z + zz, row_bytes, rows, th, td);
            for (uint32_t xx = 0; xx < width;) {
                uint32_t n = 16 - ((x + xx) & 15);
                if (n > width - xx) n = width - xx;
                uint32_t bx = x + xx;
                uint8_t *p = gob_row + (bx / 64) * block_bytes + (bx & 32) * 8 + (bx & 16) * 2 + (bx & 15);
                if (to_linear) memcpy(row + xx, p, n); else memcpy(p, row + xx, n);
                xx += n;
            }
        }
    }
    return 0;
}

int nvn_swizzle_region(const nvn_texdesc *d, unsigned level, unsigned layer,
                       uint32_t x, uint32_t y, uint32_t z, uint32_t width, uint32_t height, uint32_t depth,
                       const void *src, size_t pitch, size_t image_pitch, void *dst) {
    return move_region(d, level, layer, x, y, z, width, height, depth, dst, (uint8_t *)src, pitch, image_pitch, 0);
}
int nvn_deswizzle_region(const nvn_texdesc *d, unsigned level, unsigned layer,
                         uint32_t x, uint32_t y, uint32_t z, uint32_t width, uint32_t height, uint32_t depth,
                         const void *src, void *dst, size_t pitch, size_t image_pitch) {
    return move_region(d, level, layer, x, y, z, width, height, depth, (uint8_t *)src, dst, pitch, image_pitch, 1);
}
