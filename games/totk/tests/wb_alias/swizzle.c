/* MIT. Arbitrarily split real encoder destinations, each independently mapped
 * between PROT_NONE guards. Reference is the unchanged whole-storage API. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "backing.h"
#include "../../third_party/d2r_gfx/gfx/tex/nvn_deswizzle.h"
static unsigned smaller(unsigned value, unsigned level) { value >>= level; return value ? value : 1; }
static void exercise(nvn_texdesc d) {
    nvn_texlayout layout; nvn_tex_layout(&d, &layout); required(layout.size != 0, "valid synthetic texture layout");
    uint8_t *reference = malloc((size_t)layout.size), *assembled = malloc((size_t)layout.size);
    required(reference && assembled, "swizzle storage allocations"); memset(reference, 0xa5, (size_t)layout.size); memset(assembled, 0xa5, (size_t)layout.size);
    size_t page = (size_t)getpagesize(); uint8_t *guard = mmap(NULL, 3 * page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    required(guard != MAP_FAILED && !mprotect(guard + page, page, PROT_READ | PROT_WRITE), "guarded independent destination mapping");
    static const size_t splits[] = {1, 15, 17, 31, 63, 127, 511, 513, 1031, 4093};
    for (unsigned layer = 0; layer < layout.layers; ++layer) for (unsigned level = 0; level < d.levels; ++level) {
        int bw, bh; int bpb = nvn_fmt_bpb(d.format, &bw, &bh);
        uint32_t width = smaller(d.width, level), height = d.target == 0 || d.target == 3 ? 1 : smaller(d.height, level);
        size_t row = (size_t)((width + bw - 1) / bw) * (size_t)bpb, rows = (height + bh - 1) / bh;
        size_t depth = d.target == 2 ? smaller(d.depth, level) : 1, pitch = row + 19;
        uint8_t *source = malloc(pitch * rows * depth); required(source != NULL, "linear source");
        for (size_t i = 0; i < pitch * rows * depth; ++i) source[i] = (uint8_t)(i * 37 + level * 13 + layer * 41);
        uint64_t offset = nvn_tex_level_offset(&d, level), base = layer * layout.layer_size + offset;
        uint64_t size = level + 1 < d.levels ? nvn_tex_level_offset(&d, level + 1) - offset : layout.layer_size - offset;
        CHECK(!nvn_swizzle(&d, level, layer, source, pitch, reference), "existing whole-storage encoder accepts descriptor");
        size_t part = 0;
        for (uint64_t first = 0; first < size; ++part) {
            size_t length = splits[part % (sizeof splits / sizeof splits[0])];
            if (length > page) length = page; if (length > size - first) length = (size_t)(size - first);
            memset(guard + page, 0xa5, page);
            /* Alternate exact mapping start and end, never fabricate a storage
             * base pointer before this destination object. */
            uint8_t *destination = guard + page + (part % 2 ? page - length : 0);
            CHECK(!nvn_swizzle_span_into(&d, level, layer, source, pitch, destination, first, length), "arbitrary byte span encodes into independently mapped destination");
            CHECK(!memcmp(destination, reference + base + first, length), "span texels and untouched padding exactly equal whole-storage encoding");
            int intact = 1; for (uint8_t *p = guard + page; p < destination; ++p) intact &= *p == 0xa5;
            for (uint8_t *p = destination + length; p < guard + 2 * page; ++p) intact &= *p == 0xa5;
            CHECK(intact, "only caller destination span is written");
            memcpy(assembled + base + first, destination, length); first += length;
        }
        CHECK(!nvn_swizzle_span_into(&d, level, layer, source, pitch, guard, size + 17, 0), "zero trailing span touches no inaccessible destination");
        free(source);
    }
    CHECK(!memcmp(reference, assembled, (size_t)layout.size), "all byte splits preserve exact layers, levels, texels and padding");
    required(!munmap(guard, 3 * page), "release guarded destination"); free(reference); free(assembled);
}
int main(void) {
    const nvn_texdesc descriptors[] = {
        {.target = 1, .format = 0x25, .width = 37, .height = 19, .depth = 1, .levels = 3},
        {.target = 4, .format = 0x25, .width = 73, .height = 35, .depth = 3, .levels = 4},
        {.target = 2, .format = 0x25, .width = 33, .height = 17, .depth = 5, .levels = 3, .pkg_block_h = 0x12},
        {.target = 4, .format = 0x41, .width = 133, .height = 71, .depth = 2, .levels = 3},
        {.target = 1, .format = 0x25, .flags = 0x10, .width = 37, .height = 19, .depth = 1, .levels = 1, .stride = 192},
        {.target = 1, .format = 0x01, .flags = 0x40, .width = 73, .height = 13, .depth = 1, .levels = 1}
    };
    for (unsigned i = 0; i < sizeof descriptors / sizeof descriptors[0]; ++i) exercise(descriptors[i]);
    printf("WB_ALIAS_SWIZZLE %s arbitrary-byte-spans independent-mappings padding layers mips 3D BC linear checks=%u failures=%u\n", failures ? "FAIL" : "PASS", checks, failures); return failures != 0;
}
