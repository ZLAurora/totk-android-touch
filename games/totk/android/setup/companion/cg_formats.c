/* Data formats of make_companion_assets.py: byml() (BYML v7 subset), msbt(), extract_font()'s de-obfuscation, and the
   json.dumps(ensure_ascii=False, separators=(',', ':')) pieces write_json() produces. */
#include "cg.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t rd16(const uint8_t *p, int big) { return big ? (uint16_t)(p[0] << 8 | p[1]) : (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p, int big) {
    return big ? (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]
               : (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t rd64(const uint8_t *p) { return rd32(p, 0) | (uint64_t)rd32(p + 4, 0) << 32; }

/* ---- BYML ---- */
typedef struct { const uint8_t *d; size_t n; const char **keys, **values; size_t nkeys, nvalues; } Byml;
static int byml_strings(const uint8_t *d, size_t n, uint32_t at, const char ***out, size_t *count) {
    *out = NULL; *count = 0;
    if (!at) return 0;
    if (at > n - 4) return -1;
    uint32_t cnt = rd32(d + at, 0) >> 8;
    if (cnt > (n - at - 4) / 4) return -1;
    const char **s = calloc(cnt ? cnt : 1, sizeof *s);
    if (!s) return -1;
    for (uint32_t i = 0; i < cnt; i++) {
        uint64_t o = (uint64_t)at + rd32(d + at + 4 + i * 4, 0);
        if (o >= n || !memchr(d + o, 0, n - o)) { free(s); return -1; }
        s[i] = (const char *)d + o;
    }
    *out = s; *count = cnt;
    return 0;
}
static int byml_value(const Byml *b, unsigned t, uint32_t v, CgVal *out, int depth) {
    memset(out, 0, sizeof *out);
    const uint8_t *d = b->d; size_t n = b->n;
    if (depth > 64) return -1;
    switch (t) {
    case 0xa0: if (v >= b->nvalues) return -1; out->type = CGV_STR; out->s = b->values[v]; return 0;
    case 0xc0: case 0xc1: {
        if (v > n - 4) return -1;
        uint32_t cnt = rd32(d + v, 0) >> 8;
        out->type = t == 0xc0 ? CGV_ARR : CGV_DICT;
        if (!(out->items = calloc(cnt ? cnt : 1, sizeof *out->items))) return -1;
        out->n = cnt;
        if (t == 0xc0) {
            uint64_t start = ((uint64_t)v + 4 + cnt + 3) & ~3ull;
            if (start > n || cnt > (n - start) / 4) return -1;
            for (uint32_t i = 0; i < cnt; i++)
                if (byml_value(b, d[v + 4 + i], rd32(d + start + i * 4, 0), &out->items[i], depth + 1)) return -1;
            return 0;
        }
        if (cnt > (n - v - 4) / 8 || !(out->keys = calloc(cnt ? cnt : 1, sizeof *out->keys))) return -1;
        for (uint32_t i = 0; i < cnt; i++) {
            const uint8_t *a = d + v + 4 + i * 8;
            uint32_t k = rd32(a, 0) & 0xffffff;
            if (k >= b->nkeys) return -1;
            out->keys[i] = b->keys[k];
            if (byml_value(b, a[3], rd32(a + 4, 0), &out->items[i], depth + 1)) return -1;
        }
        return 0;
    }
    case 0xd0: out->type = CGV_BOOL; out->i = v != 0; return 0;
    case 0xd1: out->type = CGV_INT; out->i = (int32_t)v; return 0;
    case 0xd2: { float f; memcpy(&f, &v, 4); out->type = CGV_FLOAT; out->f = f; return 0; }
    case 0xd3: out->type = CGV_UINT; out->u = v; return 0;
    case 0xd4: case 0xd5: case 0xd6:
        if (v > n - 8) return -1;
        if (t == 0xd4) { out->type = CGV_INT; out->i = (int64_t)rd64(d + v); }
        else if (t == 0xd5) { out->type = CGV_UINT; out->u = rd64(d + v); }
        else { uint64_t u = rd64(d + v); out->type = CGV_FLOAT; memcpy(&out->f, &u, 8); }
        return 0;
    case 0xff: out->type = CGV_NULL; return 0;
    default: return -1;
    }
}
static void val_clear(CgVal *v) {
    if (v->type == CGV_ARR || v->type == CGV_DICT)
        for (size_t i = 0; i < v->n && v->items; i++) val_clear(&v->items[i]);
    free(v->items); free(v->keys);
}
void cg_val_free(CgVal *v) { if (v) { val_clear(v); free(v); } }
CgVal *cg_byml(const uint8_t *d, size_t n, SeedErr *err) {
    Byml b = { d, n, NULL, NULL, 0, 0 };
    CgVal *root = calloc(1, sizeof *root);
    if (!root) { seed_err(err, "internal", "Out of memory"); return NULL; }
    if (n < 16 || d[0] != 'Y' || d[1] != 'B') { seed_err(err, "internal", "Expected little-endian BYML"); goto fail; }
    uint32_t kt = rd32(d + 4, 0), st = rd32(d + 8, 0), at = rd32(d + 12, 0);
    if (byml_strings(d, n, kt, &b.keys, &b.nkeys) || byml_strings(d, n, st, &b.values, &b.nvalues) || at >= n ||
        byml_value(&b, d[at], at, root, 0)) { seed_err(err, "internal", "Unsupported or damaged BYML"); goto fail; }
    free(b.keys); free(b.values);
    return root;
fail:
    free(b.keys); free(b.values); cg_val_free(root);
    return NULL;
}
const CgVal *cg_get(const CgVal *dict, const char *key) {
    if (!dict || dict->type != CGV_DICT) return NULL;
    for (size_t i = dict->n; i-- > 0;) if (!strcmp(dict->keys[i], key)) return &dict->items[i];   /* the last wins, as dict() */
    return NULL;
}
int cg_val_str(const CgVal *v, char *out, size_t cap) {
    int r;
    if (v->type == CGV_INT) r = snprintf(out, cap, "%lld", (long long)v->i);
    else if (v->type == CGV_UINT) r = snprintf(out, cap, "%llu", (unsigned long long)v->u);
    else if (v->type == CGV_STR) r = snprintf(out, cap, "%s", v->s);
    else return -1;
    return r < 0 || (size_t)r >= cap ? -1 : 0;
}

/* ---- UTF-16 -> UTF-8 and str.strip() ---- */
static int py_space(uint32_t c) {   /* str.isspace() in Python 3.14 */
    return (c >= 0x09 && c <= 0x0d) || (c >= 0x1c && c <= 0x20) || c == 0x85 || c == 0xa0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000;
}
static void put_utf8(Buf *b, uint32_t c) {
    char s[4]; size_t n;
    if (c < 0x80) { s[0] = (char)c; n = 1; }
    else if (c < 0x800) { s[0] = (char)(0xc0 | c >> 6); s[1] = (char)(0x80 | (c & 0x3f)); n = 2; }
    else if (c < 0x10000) { s[0] = (char)(0xe0 | c >> 12); s[1] = (char)(0x80 | (c >> 6 & 0x3f)); s[2] = (char)(0x80 | (c & 0x3f)); n = 3; }
    else { s[0] = (char)(0xf0 | c >> 18); s[1] = (char)(0x80 | (c >> 12 & 0x3f)); s[2] = (char)(0x80 | (c >> 6 & 0x3f)); s[3] = (char)(0x80 | (c & 0x3f)); n = 4; }
    buf_add(b, s, n);
}
/* pieces.decode('utf-16').strip(): strict decoding (a lone surrogate fails, as in Python). */
static char *utf16_strip(const uint16_t *u, size_t n) {
    uint32_t *cp = malloc((n ? n : 1) * sizeof *cp); size_t m = 0;
    if (!cp) return NULL;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = u[i];
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < n && u[i + 1] >= 0xdc00 && u[i + 1] < 0xe000) c = 0x10000 + ((c - 0xd800) << 10) + (u[++i] - 0xdc00);
        else if (c >= 0xd800 && c < 0xe000) { free(cp); return NULL; }
        cp[m++] = c;
    }
    size_t lo = 0, hi = m;
    while (lo < hi && py_space(cp[lo])) lo++;
    while (hi > lo && py_space(cp[hi - 1])) hi--;
    Buf b = { 0 };
    for (size_t i = lo; i < hi; i++) put_utf8(&b, cp[i]);
    buf_add(&b, "", 1);
    free(cp);
    if (b.oom) { buf_free(&b); return NULL; }
    return b.p;
}
static int utf8_valid(const uint8_t *s, size_t n) {
    for (size_t i = 0; i < n;) {
        unsigned c = s[i]; size_t k = c < 0x80 ? 1 : c >= 0xc2 && c < 0xe0 ? 2 : c >= 0xe0 && c < 0xf0 ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
        if (!k || i + k > n) return 0;
        uint32_t v = k == 1 ? c : c & (k == 2 ? 0x1f : k == 3 ? 0x0f : 0x07);
        for (size_t j = 1; j < k; j++) { if ((s[i + j] & 0xc0) != 0x80) return 0; v = v << 6 | (s[i + j] & 0x3f); }
        if ((k == 3 && (v < 0x800 || (v >= 0xd800 && v < 0xe000))) || (k == 4 && (v < 0x10000 || v > 0x10ffff))) return 0;
        i += k;
    }
    return 1;
}

/* ---- MSBT ---- */
void cg_msbt_free(CgMsg *m, size_t n) {
    for (size_t i = 0; m && i < n; i++) { free(m[i].label); free(m[i].text); }
    free(m);
}
const char *cg_msg_get(const CgMsg *m, size_t n, const char *label) {
    for (size_t i = 0; i < n; i++) if (!strcmp(m[i].label, label)) return m[i].text;
    return NULL;
}
int cg_msbt(const uint8_t *d, size_t n, CgMsg **out, size_t *count, SeedErr *err) {
    *out = NULL; *count = 0;
    const uint8_t *labels = NULL, *texts = NULL; size_t nl = 0, nt = 0;
    char **decoded = NULL; size_t ndecoded = 0; CgMsg *m = NULL; size_t nm = 0, cap = 0; uint16_t *u = NULL;
    if (n < 0x20 || memcmp(d, "MsgStdBn", 8)) { seed_err(err, "internal", "Expected MSBT"); return -1; }
    int big = !(d[8] == 0xff && d[9] == 0xfe);
    if (d[12] != 1) { seed_err(err, "internal", "Expected UTF-16 MSBT"); return -1; }
    size_t at = 0x20;
    for (unsigned i = 0, ns = rd16(d + 14, big); i < ns; i++) {
        if (at > n || n - at < 16) goto bad;
        size_t size = rd32(d + at + 4, big), start = at + 16, len = start + size <= n ? size : n - start;
        if (!memcmp(d + at, "LBL1", 4)) { labels = d + start; nl = len; }   /* the last section of a name wins */
        else if (!memcmp(d + at, "TXT2", 4)) { texts = d + start; nt = len; }
        at = (at + 16 + size + 15) & ~(size_t)15;
    }
    if (!labels || !texts || nt < 4 || nl < 4) goto bad;
    size_t ntext = rd32(texts, big);
    if (ntext > (nt - 4) / 4 || !(decoded = calloc(ntext ? ntext : 1, sizeof *decoded)) || !(u = malloc(nt))) goto oom;
    for (size_t i = 0; i < ntext; i++) {
        size_t pos = rd32(texts + 4 + i * 4, big), nu = 0;
        while (pos + 2 <= nt) {
            unsigned ch = rd16(texts + pos, big); pos += 2;
            if (!ch) break;
            if (ch == 0xe) {   /* tag: group, type, size, payload */
                if (pos + 6 > nt) goto bad;
                size_t size = rd16(texts + pos + 4, big);
                if (size > nt - pos - 6) goto bad;
                if (rd16(texts + pos, big) == 201 && rd16(texts + pos + 2, big) == 10) {
                    /* Declension alternatives: the first length-prefixed form is the standalone/nominative name. */
                    if (size < 2) goto bad;
                    size_t first = rd16(texts + pos + 6, big);
                    if ((first & 1) || first > size - 2) goto bad;
                    for (size_t k = 0; k < first; k += 2) u[nu++] = rd16(texts + pos + 8 + k, big);
                }
                pos += 6 + size;
            }
            else if (ch == 0xf) pos += 4;                                                           /* closing tag: group, type */
            else u[nu++] = (uint16_t)ch;
        }
        if (!(decoded[i] = utf16_strip(u, nu))) { seed_err(err, "internal", "Invalid UTF-16 text in MSBT"); goto fail; }
        ndecoded = i + 1;
    }
    for (size_t bucket = 0, nb = rd32(labels, big); bucket < nb; bucket++) {
        if (4 + bucket * 8 + 8 > nl) goto bad;
        size_t cnt = rd32(labels + 4 + bucket * 8, big), pos = rd32(labels + 8 + bucket * 8, big);
        for (size_t k = 0; k < cnt; k++) {
            if (pos >= nl) goto bad;
            size_t size = labels[pos];
            if (pos + 1 + size + 4 > nl || !utf8_valid(labels + pos + 1, size)) goto bad;
            size_t number = rd32(labels + pos + 1 + size, big);
            if (number >= ntext) goto bad;
            char *label = malloc(size + 1), *text = strdup(decoded[number]);
            if (!label || !text) { free(label); free(text); goto oom; }
            memcpy(label, labels + pos + 1, size); label[size] = 0;
            size_t j = 0;
            while (j < nm && strcmp(m[j].label, label)) j++;
            if (j < nm) { free(m[j].text); m[j].text = text; free(label); }   /* dict: an existing key keeps its place */
            else {
                if (nm == cap) {
                    CgMsg *g = realloc(m, (cap = cap ? cap * 2 : 256) * sizeof *m);
                    if (!g) { free(label); free(text); goto oom; }
                    m = g;
                }
                m[nm++] = (CgMsg){ label, text };
            }
            pos += 5 + size;
        }
    }
    for (size_t i = 0; i < ndecoded; i++) free(decoded[i]);
    free(decoded); free(u);
    *out = m; *count = nm;
    return 0;
oom:
    seed_err(err, "internal", "Out of memory");
    goto fail;
bad:
    seed_err(err, "internal", "Damaged MSBT");
fail:
    for (size_t i = 0; i < ndecoded; i++) free(decoded[i]);
    free(decoded); free(u); cg_msbt_free(m, nm);
    return -1;
}

/* ---- JSON ---- */
void cg_json_str(Buf *b, const char *s) {
    buf_add(b, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        const unsigned char *q = p;
        while (*q && *q >= 0x20 && *q != '"' && *q != '\\') q++;
        if (q > p) { buf_add(b, (const char *)p, (size_t)(q - p)); p = q; if (!*p) break; }
        switch (*p) {
        case '"': buf_puts(b, "\\\""); break;
        case '\\': buf_puts(b, "\\\\"); break;
        case '\n': buf_puts(b, "\\n"); break;
        case '\r': buf_puts(b, "\\r"); break;
        case '\t': buf_puts(b, "\\t"); break;
        case '\b': buf_puts(b, "\\b"); break;
        case '\f': buf_puts(b, "\\f"); break;
        default: buf_printf(b, "\\u%04x", *p);
        }
    }
    buf_add(b, "\"", 1);
}
/* float.__repr__: the shortest digits that round-trip; fixed notation for -4 < exponent <= 16, else d.ddde+XX. */
void cg_json_float(Buf *b, double x) {
    if (isnan(x)) { buf_puts(b, "NaN"); return; }
    if (isinf(x)) { buf_puts(b, x < 0 ? "-Infinity" : "Infinity"); return; }
    char e[40], digits[24]; int p = 1;
    for (; p < 17; p++) { snprintf(e, sizeof e, "%.*e", p - 1, x); if (strtod(e, NULL) == x) break; }
    snprintf(e, sizeof e, "%.*e", p - 1, x);
    const char *s = e; int neg = *s == '-', nd = 0;
    if (neg) s++;
    for (; *s && *s != 'e'; s++) if (*s != '.') digits[nd++] = *s;
    digits[nd] = 0;
    int decpt = atoi(s + 1) + 1;
    if (nd == 1 && digits[0] == '0') decpt = 1;   /* 0.0 */
    if (neg) buf_add(b, "-", 1);
    if (decpt > -4 && decpt <= 16) {
        if (decpt <= 0) { buf_puts(b, "0."); for (int i = 0; i < -decpt; i++) buf_add(b, "0", 1); buf_add(b, digits, (size_t)nd); }
        else if (decpt >= nd) { buf_add(b, digits, (size_t)nd); for (int i = nd; i < decpt; i++) buf_add(b, "0", 1); buf_puts(b, ".0"); }
        else { buf_add(b, digits, (size_t)decpt); buf_add(b, ".", 1); buf_add(b, digits + decpt, (size_t)(nd - decpt)); }
    } else {
        buf_add(b, digits, 1);
        if (nd > 1) { buf_add(b, ".", 1); buf_add(b, digits + 1, (size_t)(nd - 1)); }
        buf_printf(b, "e%c%02d", decpt - 1 < 0 ? '-' : '+', abs(decpt - 1));
    }
}
void cg_json_val(Buf *b, const CgVal *v) {
    switch (v->type) {
    case CGV_NULL: buf_puts(b, "null"); break;
    case CGV_BOOL: buf_puts(b, v->i ? "true" : "false"); break;
    case CGV_INT: buf_printf(b, "%lld", (long long)v->i); break;
    case CGV_UINT: buf_printf(b, "%llu", (unsigned long long)v->u); break;
    case CGV_FLOAT: cg_json_float(b, v->f); break;
    case CGV_STR: cg_json_str(b, v->s); break;
    default: {
        int arr = v->type == CGV_ARR;
        buf_add(b, arr ? "[" : "{", 1);
        for (size_t i = 0; i < v->n; i++) {
            if (i) buf_add(b, ",", 1);
            if (!arr) { cg_json_str(b, v->keys[i]); buf_add(b, ":", 1); }
            cg_json_val(b, &v->items[i]);
        }
        buf_add(b, arr ? "]" : "}", 1);
    }
    }
}

/* ---- the Rodin font (scft/<name>.bfotf): the size word XOR'd with a key derived from 0x18029a7f, body words with the key ---- */
int cg_font(const uint8_t *blob, size_t n, uint8_t **out, size_t *out_n, int *otf, SeedErr *err) {
    if (n < 8) { seed_err(err, "internal", "Invalid game font"); return -1; }
    uint32_t key = rd32(blob, 0) ^ 0x18029a7fu, v = rd32(blob + 4, 0) ^ key;
    size_t size = (size_t)(v >> 24 | (v >> 8 & 0xff00) | (v << 8 & 0xff0000) | v << 24);
    if (size > n - 8) { seed_err(err, "internal", "Invalid game font size"); return -1; }
    uint8_t *f = malloc(size ? size : 1);
    if (!f) { seed_err(err, "internal", "Out of memory"); return -1; }
    const uint8_t *body = blob + 8;
    size_t whole = size - size % 4;
    for (size_t i = 0; i < whole; i += 4) {
        uint32_t w = rd32(body + i, 0) ^ key;
        f[i] = (uint8_t)w; f[i + 1] = (uint8_t)(w >> 8); f[i + 2] = (uint8_t)(w >> 16); f[i + 3] = (uint8_t)(w >> 24);
    }
    memcpy(f + whole, body + whole, size - whole);
    if (size < 4 || (memcmp(f, "OTTO", 4) && memcmp(f, "\0\1\0\0", 4))) { free(f); seed_err(err, "internal", "Invalid decoded game font"); return -1; }
    *otf = !memcmp(f, "OTTO", 4);
    *out = f; *out_n = size;
    return 0;
}
