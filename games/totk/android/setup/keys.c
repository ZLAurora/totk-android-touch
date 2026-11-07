/* prod.keys, read in memory only. Lines are parsed the way scripts/extract_exefs.py load_keys reads them ('#' starts a
   comment, "name = hex", names case-insensitive); only header_key, titlekek_XX and key_area_key_application_XX are kept,
   the keys an application NCA header, its key area and a common ticket need. Messages name keys, never values. */
#include "setup_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int hexval(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
/* Hex digits with optional whitespace between them (bytes.fromhex accepts that); exactly n bytes. */
static int parse_hex(const char *s, size_t len, uint8_t *out, size_t n) {
    size_t got = 0; int hi = -1;
    for (size_t i = 0; i < len; i++) {
        int c = (unsigned char)s[i];
        if (c == ' ' || c == '\t') { if (hi >= 0) return -1; continue; }
        int v = hexval(c); if (v < 0) return -1;
        if (hi < 0) hi = v; else { if (got == n) return -1; out[got++] = (uint8_t)(hi << 4 | v); hi = -1; }
    }
    return hi < 0 && got == n ? 0 : -1;
}
static int generation(const char *name, const char *prefix) {
    size_t n = strlen(prefix);
    if (strncmp(name, prefix, n) || strlen(name) != n + 2) return -1;
    int a = hexval(name[n]), b = hexval(name[n + 1]);
    return a < 0 || b < 0 ? -1 : a * 16 + b;
}
/* One "name = value" line; -1 when a kept key has a malformed value. */
static int key_line(SetupKeys *k, const char *a, const char *b, const char *v, const char *ve) {
    char name[64];
    if ((size_t)(b - a) >= sizeof name) return 0;
    for (size_t i = 0; i < (size_t)(b - a); i++) name[i] = (char)((a[i] >= 'A' && a[i] <= 'Z') ? a[i] + 32 : a[i]);
    name[b - a] = 0;
    int g, r = 0; uint8_t tmp[16];
    if (!strcmp(name, "header_key")) {
        if (!(r = parse_hex(v, (size_t)(ve - v), k->header_key, 32))) k->has_header = 1;
    } else if ((g = generation(name, "titlekek_")) >= 0 && g < KEY_GENERATIONS) {
        if (!(r = parse_hex(v, (size_t)(ve - v), tmp, 16))) { memcpy(k->titlekek[g], tmp, 16); k->titlekek_mask |= 1ull << g; }
    } else if ((g = generation(name, "key_area_key_application_")) >= 0 && g < KEY_GENERATIONS) {
        if (!(r = parse_hex(v, (size_t)(ve - v), tmp, 16))) { memcpy(k->kak_app[g], tmp, 16); k->kak_mask |= 1ull << g; }
    }
    memset(tmp, 0, sizeof tmp);
    return r;
}
static int blank(char ch) { return ch == ' ' || ch == '\t' || ch == '\r'; }
int keys_parse(SetupCtx *c, const char *text, size_t len, SetupKeys *k) {
    memset(k, 0, sizeof *k);
    size_t pos = 0; int line_no = 0;
    if (len >= 3 && !memcmp(text, "\xef\xbb\xbf", 3)) pos = 3;
    while (pos < len) {
        size_t end = pos; while (end < len && text[end] != '\n') end++;
        size_t stop = pos; while (stop < end && text[stop] != '#') stop++;
        const char *eq = memchr(text + pos, '=', stop - pos);
        line_no++;
        if (eq) {
            const char *a = text + pos, *b = eq, *v = eq + 1, *ve = text + stop;
            while (a < b && blank(*a)) a++;
            while (b > a && blank(b[-1])) b--;
            while (v < ve && blank(*v)) v++;
            while (ve > v && blank(ve[-1])) ve--;
            if (key_line(k, a, b, v, ve)) {
                setup_fail(c, "bad_keys", "prod.keys line %d: the value has the wrong length or is not hex", line_no);
                keys_wipe(k);
                return -1;
            }
        }
        pos = end + 1;
    }
    if (!k->has_header) { setup_fail(c, "bad_keys", "prod.keys has no header_key (dump the keys again with Lockpick_RCM)"); keys_wipe(k); return -1; }
    return 0;
}
int keys_load_fd(SetupCtx *c, const SetupInput *in, SetupKeys *k) {
    if (!in->present) { setup_fail(c, "missing_input", "prod.keys was not selected"); return -1; }
    if (in->size == 0 || in->size > (1u << 20)) { setup_fail(c, "bad_keys", "prod.keys must be a small text file (got %llu bytes)", (unsigned long long)in->size); return -1; }
    char *text = malloc((size_t)in->size);
    if (!text) { setup_fail(c, "internal", "Out of memory"); return -1; }
    if (st_pread(in->fd, text, (size_t)in->size, in->offset)) { int r = st_io_fail(c, "read", "prod.keys"); free(text); return r; }
    int r = keys_parse(c, text, (size_t)in->size, k);
    memset(text, 0, (size_t)in->size);
    free(text);
    return r;
}
void keys_wipe(SetupKeys *k) { volatile uint8_t *p = (volatile uint8_t *)k; for (size_t i = 0; i < sizeof *k; i++) p[i] = 0; }
static void put_key(Buf *b, const char *name, const uint8_t *key, size_t n) {
    static const char x[] = "0123456789abcdef";
    char hex[65];
    for (size_t i = 0; i < n; i++) { hex[2 * i] = x[key[i] >> 4]; hex[2 * i + 1] = x[key[i] & 15]; }
    hex[2 * n] = 0;
    buf_printf(b, "%s = %s\n", name, hex);
    memset(hex, 0, sizeof hex);
}
int keys_write_hactool(SetupCtx *c, const SetupKeys *k, const char *path) {
    /* Sized once for every generation so realloc never leaves stray copies of key bytes in freed memory. */
    Buf b = {0}; char name[48];
    b.cap = 64 + KEY_GENERATIONS * 2 * 64; b.p = malloc(b.cap); if (!b.p) b.oom = 1; else b.p[0] = 0;
    put_key(&b, "header_key", k->header_key, 32);
    for (int g = 0; g < KEY_GENERATIONS; g++) {
        if (k->kak_mask >> g & 1) { snprintf(name, sizeof name, "key_area_key_application_%02x", g); put_key(&b, name, k->kak_app[g], 16); }
        if (k->titlekek_mask >> g & 1) { snprintf(name, sizeof name, "titlekek_%02x", g); put_key(&b, name, k->titlekek[g], 16); }
    }
    int r = 0;
    if (b.oom) { setup_fail(c, "internal", "Out of memory"); r = -1; }
    else {
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0) r = st_io_fail(c, "open the temporary key file", path);
        else {
            if (st_write(fd, b.p, b.n)) r = st_io_fail(c, "write the temporary key file", path);
            if (close(fd) && !r) r = st_io_fail(c, "close the temporary key file", path);
        }
        if (r) unlink(path);
    }
    if (b.p) memset(b.p, 0, b.n);
    buf_free(&b);
    return r;
}
