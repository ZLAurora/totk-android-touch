/* The six shared-font slots, filled from the open font bundled in the APK (the system shared fonts are not asked for:
   the game loads them lazily and normal play never does). Each slot has the form of the firmware .bfttf files the runtime
   served before (scripts/extract_shared_fonts.py bfttf_size decodes it): big-endian words 0x7f9a0218^K and size^K
   (K = 0x49621806), then the font with every big-endian word XOR'd with K, zero-padded to whole words (size counts the
   padding); plus shared-fonts.txt in the scripts/prepare_shared_fonts.py format. */
#include "setup_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *const setup_font_names[6] = { "0-standard", "1-chinese-simplified", "2-chinese-simplified-extension",
                                          "3-chinese-traditional", "4-korean", "5-nintendo-extension" };
#define BFTTF_KEY 0x49621806u
#define FONT_MAX (64u << 20)

static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

int setup_fonts_write(SetupCtx *c, const SetupInput *fonts, int nfonts, const char *out_dir, const char *out_txt, char src_sha[65]) {
    if (!nfonts) { setup_fail(c, "missing_input", "No bundled font was passed to setup (app packaging problem)"); return -1; }
    if (st_rmtree(out_dir) || st_mkdirs(out_dir)) return st_io_fail(c, "create", out_dir);
    Buf txt = {0};
    int rc = 0;
    for (int i = 0; i < 6 && !rc; i++) {
        /* One font per slot when several are bundled; the last one fills the remaining slots. */
        const SetupInput *in = &fonts[i < nfonts ? i : nfonts - 1];
        if (in->size < 12 || in->size > FONT_MAX - 12) { setup_fail(c, "missing_input", "%s: unexpected font size", in->name); rc = -1; break; }
        size_t padded = ((size_t)in->size + 3) & ~(size_t)3;
        uint8_t *data = calloc(1, padded + 8);
        if (!data) { setup_fail(c, "internal", "Out of memory"); rc = -1; break; }
        if (st_pread(in->fd, data + 8, (size_t)in->size, in->offset)) { rc = st_io_fail(c, "read", in->name); free(data); break; }
        const uint8_t *m = data + 8;
        if (memcmp(m, "\0\1\0\0", 4) && memcmp(m, "OTTO", 4) && memcmp(m, "true", 4)) { free(data); setup_fail(c, "missing_input", "%s is not a TrueType/OpenType font", in->name); rc = -1; break; }
        if (i == 0) sha256_buf_hex(data + 8, (size_t)in->size, src_sha);
        be32(data, 0x7f9a0218u ^ BFTTF_KEY); be32(data + 4, (uint32_t)padded ^ BFTTF_KEY);
        for (size_t k = 8; k < padded + 8; k += 4) { uint32_t w = (uint32_t)data[k] << 24 | (uint32_t)data[k + 1] << 16 | (uint32_t)data[k + 2] << 8 | data[k + 3]; be32(data + k, w ^ BFTTF_KEY); }
        char path[SETUP_PATH], sha[65];
        sha256_buf_hex(data, padded + 8, sha);
        if (st_path(path, sizeof path, "%s/%s.bfttf", out_dir, setup_font_names[i]) || st_write_file(path, data, padded + 8, 0644)) rc = st_io_fail(c, "write", path);
        free(data);
        buf_printf(&txt, "FONT%d_FILE %s.bfttf\nFONT%d_FILE_BYTES %llu\nFONT%d_FONT_BYTES %llu\nFONT%d_SHA256 %s\n", i, setup_font_names[i],
                   i, (unsigned long long)padded + 8, i, (unsigned long long)padded, i, sha);
        setup_progress(c, "fonts", (uint64_t)i + 1, 6, setup_font_names[i]);
    }
    if (!rc && txt.oom) { setup_fail(c, "internal", "Out of memory"); rc = -1; }
    if (!rc && st_write_file(out_txt, txt.p, txt.n, 0644)) rc = st_io_fail(c, "write", out_txt);
    buf_free(&txt);
    return rc;
}
