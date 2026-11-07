/* runtime-tables.bin (TOTKTAB1): the C port of scripts/prepare_runtime.py's writer, byte-identical to it for the same
   rewritten images. Also writes a rewritten module to disk and reads one back (fixups.tsv / gates.tsv) for the host tests. */
#include "rw_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define E(...) do { snprintf(err, errlen, __VA_ARGS__); goto fail; } while (0)

static int write_file(const char *path, const void *data, size_t n, RwSha256 *sha, char *err, size_t errlen) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) { snprintf(err, errlen, "cannot create %s: %s", path, strerror(errno)); return RW_FAIL; }
    if (sha && n) rw_sha256_update(sha, data, n);
    for (size_t o = 0; o < n;) {
        ssize_t w = write(fd, (const uint8_t *)data + o, n - o > (1u << 24) ? (1u << 24) : n - o);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) { snprintf(err, errlen, "cannot write %s: %s", path, w ? strerror(errno) : "no space"); close(fd); return RW_FAIL; }
        o += (size_t)w;
    }
    if (fsync(fd) < 0 || close(fd) < 0) { snprintf(err, errlen, "cannot write %s: %s", path, strerror(errno)); return RW_FAIL; }
    return RW_OK;
}

int rw_write_module(const RwModule *m, const char *dir, int image, int tsvs, char sha256_hex[65], char *err, size_t errlen) {
    char path[4096]; RwSha256 s; rw_sha256_init(&s);
    if (image) {
        snprintf(path, sizeof path, "%s/%s.patched.bin", dir, m->l.name);
        if (write_file(path, m->image, m->image_size, &s, err, errlen)) return RW_FAIL;
        if (sha256_hex) rw_sha256_hex(&s, sha256_hex);
    }
    for (int k = 0; tsvs && k < 6; k++) {
        snprintf(path, sizeof path, "%s/%s.%s.tsv", dir, m->l.name, rw_tsv_names[k]);
        if (write_file(path, m->tsv[k].p, m->tsv[k].n, NULL, err, errlen)) return RW_FAIL;
    }
    return RW_OK;
}

int rw_read_layout(const char *dir, const char *name, RwLayout *l, char *err, size_t errlen) {
    char path[4096]; uint8_t *d; size_t n; int got = 0;
    memset(l, 0, sizeof *l); snprintf(l->name, sizeof l->name, "%s", name);
    snprintf(path, sizeof path, "%s/%s.fixups.tsv", dir, name);
    if (rw_read_file(path, &d, &n, err, errlen)) return RW_FAIL;
    char *t = realloc(d, n + 1); if (!t) { free(d); snprintf(err, errlen, "out of memory"); return RW_FAIL; }
    t[n] = 0;
    for (char *line = strtok(t, "\n"); line; line = strtok(NULL, "\n")) {
        char key[64]; unsigned long long v;
        if (sscanf(line, "# %63s %lli", key, &v) != 2) continue;
        if (!strcmp(key, "PATCH_OFF")) { l->patch_off = v; got |= 1; }
        else if (!strcmp(key, "KEY_PAGE_OFF")) { l->key_page_off = v; got |= 2; }
        else if (!strcmp(key, "TEXT_END")) { l->text_end = v; got |= 4; }
        else if (!strcmp(key, "RO_OFF")) { l->ro_off = v; got |= 8; }
        else if (!strcmp(key, "DATA_OFF")) { l->data_off = v; got |= 16; }
    }
    free(t);
    if (got != 31) { snprintf(err, errlen, "%s: incomplete layout", path); return RW_FAIL; }
    return RW_OK;
}

int rw_read_gates(const char *dir, const char *name, RwGate **gates, size_t *n, char *err, size_t errlen) {
    char path[4096]; uint8_t *d; size_t dn, cap = 0;
    *gates = NULL; *n = 0;
    snprintf(path, sizeof path, "%s/%s.gates.tsv", dir, name);
    if (rw_read_file(path, &d, &dn, err, errlen)) return RW_FAIL;
    char *t = realloc(d, dn + 1); if (!t) { free(d); snprintf(err, errlen, "out of memory"); return RW_FAIL; }
    t[dn] = 0;
    for (char *line = strtok(t, "\n"); line; line = strtok(NULL, "\n")) {
        unsigned long long ret, orig; unsigned arg; char kind[16]; RwGate g;
        if (sscanf(line, "%lli %15s %u %lli", &ret, kind, &arg, &orig) != 4) { snprintf(err, errlen, "%s: bad line", path); goto fail; }
        if (!strcmp(kind, "svc")) g.kind = RW_GATE_SVC; else if (!strcmp(kind, "cntpct")) g.kind = RW_GATE_CNTPCT;
        else if (!strcmp(kind, "cntvct")) g.kind = RW_GATE_CNTVCT; else { snprintf(err, errlen, "%s: gate kind %s", path, kind); goto fail; }
        g.ret = ret; g.original = orig; g.arg = arg;
        if (*n == cap) { cap = cap ? 2 * cap : 64; RwGate *p = realloc(*gates, cap * sizeof *p); if (!p) { snprintf(err, errlen, "out of memory"); goto fail; } *gates = p; }
        (*gates)[(*n)++] = g;
    }
    free(t); return RW_OK;
fail:
    free(t); free(*gates); *gates = NULL; *n = 0; return RW_FAIL;
}

/* ---- prepare_runtime.py per-module tables ---- */
typedef struct { uint64_t value, size; uint32_t name; uint8_t defined, binding, type; } Sym;
typedef struct { uint64_t where; uint32_t kind, symbol; int64_t addend; } Rel;
struct TabModule {
    char name[16], sha[65];
    uint64_t size, text_end, ro_off, data_off, patch_off, key_off, init, ctor_flag, init_array, init_size, probe_plt, probe_slot;
    Sym *sym; size_t nsym; char *names; size_t names_len;   /* names: NUL-terminated, Sym.name = offset in names */
    Rel *rel; size_t nrel; RwGate *gate; size_t ngate;
};

static int utf8_ok(const uint8_t *s, size_t n) {         /* Python's strict bytes.decode() */
    for (size_t i = 0; i < n;) {
        uint8_t c = s[i]; size_t k; uint32_t cp, min;
        if (c < 0x80) { i++; continue; }
        if ((c & 0xE0) == 0xC0) { k = 1; cp = c & 0x1F; min = 0x80; }
        else if ((c & 0xF0) == 0xE0) { k = 2; cp = c & 0x0F; min = 0x800; }
        else if ((c & 0xF8) == 0xF0) { k = 3; cp = c & 0x07; min = 0x10000; }
        else return 0;
        for (size_t j = 1; j <= k; j++) { if (i + j >= n || (s[i + j] & 0xC0) != 0x80) return 0; cp = cp << 6 | (s[i + j] & 0x3F); }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
        i += k + 1;
    }
    return 1;
}
static int cmp_gate(const void *a, const void *b) { uint64_t x = ((const RwGate *)a)->ret, y = ((const RwGate *)b)->ret; return x < y ? -1 : x > y; }
static int cmp_u64(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }

void tab_module_free(TabModule *m) { if (!m) return; free(m->sym); free(m->names); free(m->rel); free(m->gate); free(m); }

TabModule *tab_module(const RwLayout *l, const uint8_t *img, size_t size, const RwGate *gates, size_t ngate,
                      const uint8_t nso_header[0x100], char *err, size_t errlen) {
    TabModule *m = calloc(1, sizeof *m); RwTags tags = {0}; RwRelr *relr = NULL; size_t nrelr = 0, capt = 0, capn = 0; uint64_t *wh = NULL;
    if (!m) { snprintf(err, errlen, "out of memory"); return NULL; }
    snprintf(m->name, sizeof m->name, "%s", l->name);
    { RwSha256 s; rw_sha256_init(&s); rw_sha256_update(&s, img, size); rw_sha256_hex(&s, m->sha); }
    m->size = size; m->text_end = l->text_end; m->ro_off = l->ro_off; m->data_off = l->data_off; m->patch_off = l->patch_off; m->key_off = l->key_page_off;
    if (size < 8) E("%s: image too small", l->name);
    uint64_t mod0 = rd32(img + 4);
    if (mod0 + 8 > size || memcmp(img + mod0, "MOD0", 4)) E("%s: no MOD0 header", l->name);
    uint64_t dyn = mod0 + (uint64_t)(int64_t)(int32_t)rd32(img + mod0 + 4); int term = 0;
    for (uint64_t o = dyn; o < dyn + 4096 * 16; o += 16) {
        if (o + 16 > size) E("%s: dynamic table outside image", l->name);
        RwTag t = {(int64_t)rd64(img + o), rd64(img + o + 8)};
        if (!t.tag) { term = 1; break; }
        if (tags.n == capt) { capt = capt ? 2 * capt : 64; RwTag *p = realloc(tags.v, capt * sizeof *p); if (!p) E("out of memory"); tags.v = p; }
        tags.v[tags.n++] = t;
    }
    if (!term) E("Unterminated dynamic table");
    uint64_t hash, symtab, strtab, strsz, v;
#define NEED(t, out) do { if (!rw_tag(&tags, t, &(out))) E("%s: dynamic tag %d missing", l->name, t); } while (0)
    NEED(6, symtab);
    if (rw_tag(&tags, 4, &hash)) { if (hash + 8 > size) E("%s: DT_HASH outside image", l->name); m->nsym = rd32(img + hash + 4); }
    else {   /* GNU hash only (newer SDK modules): the NSO header's verified dynsym extent gives the exact count */
        uint32_t so = rd32(nso_header + 0x98), ss = rd32(nso_header + 0x9c);
        if (l->ro_off + so != symtab || ss % 24) E("%s: dynsym extent does not match the NSO header", l->name);
        if (!(0 < ss && ss <= l->data_off - symtab)) E("%s: dynsym extent", l->name);
        m->nsym = ss / 24;
    }
    NEED(11, v); if (v != 24) E("%s: DT_SYMENT", l->name);
    NEED(9, v); if (v != 24) E("%s: DT_RELAENT", l->name);
    NEED(20, v); if (v != 7) E("%s: DT_PLTREL", l->name);
    NEED(5, strtab); NEED(10, strsz);
    m->sym = calloc(m->nsym ? m->nsym : 1, sizeof *m->sym);
    if (!m->sym) E("out of memory");
    for (size_t i = 0; i < m->nsym; i++) {
        uint64_t o = symtab + 24 * i;
        if (o + 24 > size) E("%s: symbol %zu outside image", l->name, i);
        uint32_t no = rd32(img + o); uint8_t info = img[o + 4]; uint16_t section = (uint16_t)(img[o + 6] | img[o + 7] << 8);
        uint64_t s0 = strtab + no, s1 = strtab + strsz < size ? strtab + strsz : size, e = s0;
        while (e < s1 && img[e]) e++;
        if (s0 > s1 || e >= s1) E("%s: symbol %zu name unterminated", l->name, i);
        if (!utf8_ok(img + s0, e - s0)) E("%s: symbol %zu name is not UTF-8", l->name, i);
        size_t len = e - s0;
        if (m->names_len + len + 1 > capn) { while (m->names_len + len + 1 > capn) capn = capn ? 2 * capn : 1 << 16; char *p = realloc(m->names, capn); if (!p) E("out of memory"); m->names = p; }
        m->sym[i].name = (uint32_t)m->names_len; memcpy(m->names + m->names_len, img + s0, len); m->names[m->names_len + len] = 0; m->names_len += len + 1;
        m->sym[i].value = rd64(img + o + 8); m->sym[i].size = rd64(img + o + 16);
        m->sym[i].defined = section != 0; m->sym[i].binding = info >> 4; m->sym[i].type = info & 15;
    }
    if (rw_decode_relr(img, size, &tags, l->data_off, l->key_page_off, &relr, &nrelr, err, errlen)) goto fail;
    uint64_t rela, relasz, jmp, jmpsz; NEED(7, rela); NEED(8, relasz); NEED(23, jmp); NEED(2, jmpsz);
    if (relasz % 24 || jmpsz % 24) E("%s: relocation table size", l->name);
    size_t total = nrelr + relasz / 24 + jmpsz / 24;
    m->rel = malloc((total ? total : 1) * sizeof *m->rel); wh = malloc((total ? total : 1) * sizeof *wh);
    if (!m->rel || !wh) E("out of memory");
    for (size_t i = 0; i < nrelr; i++) { m->rel[m->nrel] = (Rel){relr[i].where, 1027, 0, (int64_t)relr[i].addend}; wh[m->nrel++] = relr[i].where; }
    uint64_t tab[2][2] = {{rela, relasz}, {jmp, jmpsz}};
    for (int k = 0; k < 2; k++) for (uint64_t o = tab[k][0]; o < tab[k][0] + tab[k][1]; o += 24) {
        if (o + 24 > size) E("%s: relocation outside image", l->name);
        uint64_t where = rd64(img + o), info = rd64(img + o + 8); int64_t addend = (int64_t)rd64(img + o + 16);
        uint32_t kind = (uint32_t)info, si = (uint32_t)(info >> 32);
        if (!(kind == 257 || kind == 1025 || kind == 1026 || kind == 1027) || si >= m->nsym) E("%s: relocation kind/symbol at %#llx", l->name, (unsigned long long)where);
        if (where < l->data_off || l->key_page_off < 8 || where > l->key_page_off - 8) E("%s: relocation outside data %#llx", l->name, (unsigned long long)where);
        if (kind == 1027 && (addend < 0 || (uint64_t)addend >= l->key_page_off)) E("%s: relative addend %#llx", l->name, (unsigned long long)where);
        m->rel[m->nrel] = (Rel){where, kind, si, addend}; wh[m->nrel++] = where;
    }
    qsort(wh, m->nrel, sizeof *wh, cmp_u64);
    for (size_t i = 1; i < m->nrel; i++) if (wh[i] == wh[i - 1]) E("Duplicate RELR/RELA relocation");
    m->gate = malloc((ngate ? ngate : 1) * sizeof *m->gate);
    if (!m->gate) E("out of memory");
    if (ngate) memcpy(m->gate, gates, ngate * sizeof *gates);
    m->ngate = ngate;
    qsort(m->gate, ngate, sizeof *m->gate, cmp_gate);  /* runtime_dispatch binary-searches by return address */
    for (size_t i = 1; i < ngate; i++) if (m->gate[i].ret == m->gate[i - 1].ret) E("duplicate gate return address");
    /* DT_INIT's constructor-done flag, as 1.4.3's compiler loads it (adrp x19 at +16, ldr w8, [x19, #..] at +20) */
    NEED(12, m->init); rw_tag(&tags, 25, &m->init_array); rw_tag(&tags, 27, &m->init_size);
    if (m->init + 24 > size) E("%s: DT_INIT outside image", l->name);
    uint32_t a = rd32(img + m->init + 16), w = rd32(img + m->init + 20);
    if ((a & 0x9f00001f) != (0x90000000u | 19) || (w & 0xffc003ff) != (0xb9400008u | 19 << 5)) E("%s: unexpected DT_INIT state load", l->name);
    int64_t imm = (int64_t)((a >> 29 & 3) | (a >> 5 & 0x7ffff) << 2); imm = (imm ^ (1 << 20)) - (1 << 20);
    m->ctor_flag = (uint64_t)((int64_t)((m->init + 16) & ~4095ull) + imm * 4096 + (int64_t)((w >> 10 & 0xfff) * 4));
    if (m->ctor_flag < l->data_off || m->ctor_flag >= l->key_page_off) E("%s: constructor flag outside data", l->name);
    if (!strcmp(l->name, "main")) {   /* main's ORIGINAL PLT stub addressing the relocated GetSystemTick slot */
        size_t r; int found = 0;
        for (r = 0; r < m->nrel; r++) if (m->rel[r].kind == 1026 && !strcmp(m->names + m->sym[m->rel[r].symbol].name, "_ZN2nn2os13GetSystemTickEv")) { found = 1; break; }
        if (!found) E("main: no GetSystemTick slot");
        m->probe_slot = m->rel[r].where; int hits = 0;
        if (l->text_end < 0x10000 || l->text_end > size) E("main: text");
        for (uint64_t ad = l->text_end - 0x10000; ad < l->text_end - 12; ad += 4) {
            uint32_t p0 = rd32(img + ad), p1 = rd32(img + ad + 4), p3 = rd32(img + ad + 12);
            if ((p0 & 0x9f00001f) != 0x90000010 || (p1 & 0xffc003ff) != 0xf9400211 || p3 != 0xd61f0220) continue;
            int64_t im = (int64_t)((p0 >> 29 & 3) | (p0 >> 5 & 0x7ffff) << 2); im = (im ^ (1 << 20)) - (1 << 20);
            uint64_t ref = (uint64_t)((int64_t)(ad & ~4095ull) + im * 4096 + (int64_t)((p1 >> 10 & 0xfff) * 8));
            if (ref == m->probe_slot) { m->probe_plt = ad; hits++; }
        }
        if (hits != 1) E("main: %d PLT matches", hits);
    }
#undef NEED
    free(tags.v); free(relr); free(wh); return m;
fail:
    free(tags.v); free(relr); free(wh); tab_module_free(m); return NULL;
}

/* ---- the file: 64-byte header, 3 x 208-byte records, 8-aligned arrays, then the name pool ---- */
typedef struct { uint8_t *p; size_t n, cap; } Bin;
static int put(Bin *b, const void *d, size_t n) {
    if (b->n + n > b->cap) { size_t c = b->cap ? b->cap : 1 << 16; while (c < b->n + n) c *= 2; uint8_t *q = realloc(b->p, c); if (!q) return -1; b->p = q; b->cap = c; }
    if (n) memcpy(b->p + b->n, d, n);
    b->n += n; return 0;
}
static void le(uint8_t *p, uint64_t v, int n) { for (int i = 0; i < n; i++) p[i] = (uint8_t)(v >> 8 * i); }

int tab_write(TabModule *const mods[3], const char *path, char sha256_hex[65], char *err, size_t errlen) {
    enum { HEADER = 64, RECORD = 208, NMOD = 3 };
    Bin arrays = {0}, strings = {0}; uint8_t rec[NMOD][RECORD], z[8] = {0}, e[24]; uint64_t off[3]; int rc = RW_FAIL;
    uint32_t base = 0;
    for (int k = 0; k < NMOD; k++) {
        const TabModule *m = mods[k];
        if (strings.n + m->names_len > UINT32_MAX) E("name pool too large");
        base = (uint32_t)strings.n;
        if (put(&strings, m->names, m->names_len)) E("out of memory");
        /* place(): pad the arrays to 8, append, and return the blob's file offset */
        for (int t = 0; t < 3; t++) {
            if (put(&arrays, z, (8 - arrays.n % 8) % 8)) E("out of memory");
            off[t] = HEADER + RECORD * NMOD + arrays.n;
            size_t cnt = t == 0 ? m->nsym : t == 1 ? m->nrel : m->ngate;
            for (size_t i = 0; i < cnt; i++) {
                memset(e, 0, sizeof e);
                if (t == 0) { const Sym *s = &m->sym[i]; le(e, s->value, 8); le(e + 8, s->size, 8); le(e + 16, base + s->name, 4); e[20] = s->defined; e[21] = s->binding; e[22] = s->type; }
                else if (t == 1) { const Rel *r = &m->rel[i]; le(e, r->where, 8); le(e + 8, r->kind, 4); le(e + 12, r->symbol, 4); le(e + 16, (uint64_t)r->addend, 8); }
                else { const RwGate *g = &m->gate[i]; le(e, g->ret, 8); le(e + 8, g->original, 8); le(e + 16, g->kind != RW_GATE_SVC, 4); le(e + 20, g->arg, 4); }
                if (put(&arrays, e, 24)) E("out of memory");
            }
        }
        uint8_t *r = rec[k]; memset(r, 0, RECORD);
        memcpy(r, m->name, strlen(m->name) < 16 ? strlen(m->name) : 16); memcpy(r + 16, m->sha, 64);
        uint64_t q[13] = {m->size, m->text_end, m->ro_off, m->data_off, m->patch_off, m->key_off, m->init, m->ctor_flag, m->init_array, m->init_size, off[0], off[1], off[2]};
        for (int i = 0; i < 13; i++) le(r + 88 + 8 * i, q[i], 8);
        le(r + 192, m->nsym, 4); le(r + 196, m->nrel, 4); le(r + 200, m->ngate, 4); le(r + 204, 0, 4);
    }
    if (put(&arrays, z, (8 - arrays.n % 8) % 8)) E("out of memory");
    uint64_t strings_offset = HEADER + RECORD * NMOD + arrays.n, size = strings_offset + strings.n;
    uint8_t h[HEADER] = {0};
    memcpy(h, "TOTKTAB1", 8); memcpy(h + 8, "v143", 4); le(h + 16, NMOD, 4); le(h + 20, 1, 4);
    le(h + 24, mods[0]->probe_plt, 8); le(h + 32, mods[0]->probe_slot, 8); le(h + 40, strings_offset, 8); le(h + 48, strings.n, 8); le(h + 56, size, 8);
    char part[4096]; snprintf(part, sizeof part, "%s.partial", path);
    int fd = open(part, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) E("cannot create %s: %s", part, strerror(errno));
    RwSha256 s; rw_sha256_init(&s);
    const void *pp[4] = {h, rec, arrays.p, strings.p}; size_t pn[4] = {HEADER, sizeof rec, arrays.n, strings.n};
    for (int k = 0; k < 4; k++) {
        if (pn[k]) rw_sha256_update(&s, pp[k], pn[k]);
        for (size_t o = 0; o < pn[k];) {
            ssize_t w = write(fd, (const uint8_t *)pp[k] + o, pn[k] - o);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) { int e = errno; close(fd); unlink(part); E("cannot write %s: %s", part, w ? strerror(e) : "no space"); }
            o += (size_t)w;
        }
    }
    if (fsync(fd) < 0) { int e = errno; close(fd); unlink(part); E("cannot sync %s: %s", path, strerror(e)); }
    if (close(fd) < 0) { int e = errno; unlink(part); E("cannot close %s: %s", path, strerror(e)); }
    if (rename(part, path) < 0) { int e = errno; unlink(part); E("cannot rename %s: %s", path, strerror(e)); }
    if (sha256_hex) rw_sha256_hex(&s, sha256_hex);
    rc = RW_OK;
fail:
    free(arrays.p); free(strings.p); return rc;
}
