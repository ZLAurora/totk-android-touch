/* XCI / NSP reading for the container route, a C port of scripts/extract_exefs.py: bounded HFS0 / PFS0 tables, common
   tickets (personalized ones are refused), NCA3 headers decrypted with header_key, and the base / update Program NCA
   choice. Every offset read from the file is checked against the enclosing range before use. */
#include "setup_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }
static int bad(SetupCtx *c, const char *code, const char *what, const char *msg) { setup_fail(c, code, "%s: %s", what, msg); return -1; }
static int valid_utf8(const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        unsigned c = *p; int n = c < 0x80 ? 1 : c >= 0xc2 && c < 0xe0 ? 2 : c >= 0xe0 && c < 0xf0 ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
        if (!n) return 0;
        uint32_t v = n == 1 ? c : c & (n == 2 ? 0x1f : n == 3 ? 0x0f : 0x07);
        for (int i = 1; i < n; i++) { if ((p[i] & 0xc0) != 0x80) return 0; v = v << 6 | (p[i] & 0x3f); }
        if ((n == 3 && (v < 0x800 || (v >= 0xd800 && v < 0xe000))) || (n == 4 && (v < 0x10000 || v > 0x10ffff))) return 0;
        p += n;
    }
    return 1;
}
static int by_start(const void *a, const void *b) { const PEntry *x = a, *y = b; return x->off < y->off ? -1 : x->off > y->off; }

int part_parse(SetupCtx *c, int fd, uint64_t off, uint64_t bound, const char *magic, int stride, PEntry **out, int *n) {
    const char *what = magic[0] == 'H' ? "HFS0 partition" : "PFS0 partition";
    uint8_t h[16];
    *out = NULL; *n = 0;
    if (bound < 16) return bad(c, "missing_input", what, "truncated header");
    if (st_pread(fd, h, 16, off)) return st_io_fail(c, "read", what);
    if (memcmp(h, magic, 4)) return bad(c, "missing_input", what, "unexpected magic (not an XCI / NSP, or a damaged file)");
    uint32_t count = le32(h + 4), strsize = le32(h + 8);
    if (count > 4096 || strsize > (1u << 20)) return bad(c, "missing_input", what, "invalid table size");
    uint64_t size = 16 + (uint64_t)count * (uint64_t)stride + strsize;
    if (size > bound) return bad(c, "missing_input", what, "table out of bounds");
    uint8_t *raw = malloc((size_t)size + 1);
    PEntry *e = calloc(count ? count : 1, sizeof *e);
    if (!raw || !e) { free(raw); free(e); setup_fail(c, "internal", "Out of memory"); return -1; }
    if (st_pread(fd, raw, (size_t)size, off)) { int rc = st_io_fail(c, "read", what); free(raw); free(e); return rc; }
    const char *strings = (const char *)raw + 16 + (size_t)count * (size_t)stride;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *r = raw + 16 + (size_t)i * (size_t)stride;
        uint64_t eo = le64(r), el = le64(r + 8); uint32_t no = le32(r + 16);
        if (no >= strsize) { free(raw); free(e); return bad(c, "missing_input", what, "invalid file name offset"); }
        const char *name = strings + no, *nul = memchr(name, 0, strsize - no);
        if (!nul) { free(raw); free(e); return bad(c, "missing_input", what, "unterminated file name"); }
        size_t nl = (size_t)(nul - name);
        if (!nl || nl >= sizeof e[i].name || !strcmp(name, ".") || !strcmp(name, "..") || memchr(name, '/', nl) || memchr(name, '\\', nl) || !valid_utf8(name)) {
            free(raw); free(e); return bad(c, "missing_input", what, "unsafe file name"); }
        for (uint32_t j = 0; j < i; j++) if (!strcmp(e[j].name, name)) { free(raw); free(e); return bad(c, "missing_input", what, "duplicate file name"); }
        if (eo > bound - size || el > bound - size - eo) { free(raw); free(e); return bad(c, "missing_input", what, "entry out of bounds"); }
        memcpy(e[i].name, name, nl + 1); e[i].off = off + size + eo; e[i].size = el;
    }
    free(raw);
    PEntry *sorted = malloc((count ? count : 1) * sizeof *sorted);
    if (!sorted) { free(e); setup_fail(c, "internal", "Out of memory"); return -1; }
    memcpy(sorted, e, count * sizeof *e);
    qsort(sorted, count, sizeof *sorted, by_start);
    uint64_t end = off + size;
    for (uint32_t i = 0; i < count; i++) {
        if (sorted[i].off < end) { free(sorted); free(e); return bad(c, "missing_input", what, "overlapping entries"); }
        end = sorted[i].off + sorted[i].size;
    }
    free(sorted);
    *out = e; *n = (int)count;
    return 0;
}

static int ends_with(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    if (a < b) return 0;
    for (size_t i = 0; i < b; i++) { char x = s[a - b + i]; if (x >= 'A' && x <= 'Z') x += 32; if (x != suffix[i]) return 0; }
    return 1;
}
/* Common tickets keep only (rights id, encrypted title key); offsets are relative to the signed body, as in Python. */
static int tickets(SetupCtx *c, Container *k) {
    for (int i = 0; i < k->nentries; i++) {
        const PEntry *e = &k->entries[i];
        if (!ends_with(e->name, ".tik")) continue;
        uint8_t sig[4], data[0x180];
        if (e->size < 4) return bad(c, "missing_input", "NSP ticket", "truncated");
        if (st_pread(k->fd, sig, 4, e->off)) return st_io_fail(c, "read", "ticket");
        uint32_t type = le32(sig), body;
        switch (type) {
        case 0x10000: case 0x10003: body = 0x240; break;
        case 0x10001: case 0x10004: body = 0x140; break;
        case 0x10002: case 0x10005: body = 0x80; break;
        default: return bad(c, "missing_input", "NSP ticket", "unsupported signature type");
        }
        if (e->size < body + 0x180) return bad(c, "missing_input", "NSP ticket", "truncated body");
        if (st_pread(k->fd, data, 0x180, e->off + body)) return st_io_fail(c, "read", "ticket");
        if (data[0x141]) return bad(c, "missing_input", "NSP ticket", "personalized tickets are unsupported; a common ticket is required (dump the game again with nxdumptool, ticket: common)");
        int found = 0;
        for (int j = 0; j < k->ntickets; j++)
            if (!memcmp(k->tickets[j].rights, data + 0x160, 16)) {
                if (memcmp(k->tickets[j].key, data + 0x40, 16)) return bad(c, "missing_input", "NSP ticket", "conflicting tickets");
                found = 1;
            }
        if (found) continue;
        Ticket *t = realloc(k->tickets, (size_t)(k->ntickets + 1) * sizeof *t);
        if (!t) { setup_fail(c, "internal", "Out of memory"); return -1; }
        k->tickets = t;
        memcpy(t[k->ntickets].rights, data + 0x160, 16); memcpy(t[k->ntickets].key, data + 0x40, 16); k->ntickets++;
        memset(data, 0, sizeof data);
    }
    return 0;
}

int container_open(SetupCtx *c, const SetupInput *in, Container *k, const char *role) {
    memset(k, 0, sizeof *k);
    k->fd = in->fd; k->size = in->size; k->program = -1; k->bktr_section = -1;
    if (!in->present) { setup_fail(c, "missing_input", "The %s game file was not selected", role); return -1; }
    uint8_t m[4];
    if (k->size < 16) return bad(c, "missing_input", role, "the file is too small to be an XCI or NSP");
    if (st_pread(k->fd, m, 4, 0)) return st_io_fail(c, "read", role);
    if (!memcmp(m, "PFS0", 4)) {
        if (part_parse(c, k->fd, 0, k->size, "PFS0", 24, &k->entries, &k->nentries)) return -1;
    } else {
        uint8_t h[0x38];
        if (k->size < 0x138) return bad(c, "missing_input", role, "expected an XCI or NSP (PFS0) game file");
        if (st_pread(k->fd, h, sizeof h, 0x100)) return st_io_fail(c, "read", role);
        if (memcmp(h, "HEAD", 4)) return bad(c, "missing_input", role, "expected an XCI or NSP (PFS0) game file");
        uint64_t root = le64(h + 0x30);
        if (root >= k->size) return bad(c, "missing_input", role, "XCI root partition out of bounds");
        PEntry *parts; int n;
        if (part_parse(c, k->fd, root, k->size - root, "HFS0", 64, &parts, &n)) return -1;
        int secure = -1, count = 0;
        for (int i = 0; i < n; i++) if (!strcmp(parts[i].name, "secure")) { secure = i; count++; }
        if (count != 1) { free(parts); return bad(c, "missing_input", role, "expected one XCI secure partition"); }
        PEntry s = parts[secure]; free(parts);
        if (part_parse(c, k->fd, s.off, s.size, "HFS0", 64, &k->entries, &k->nentries)) return -1;
        k->is_xci = 1;
    }
    return tickets(c, k);
}

int container_inventory(SetupCtx *c, Container *k, const SetupKeys *keys) {
    for (int i = 0; i < k->nentries; i++) {
        const PEntry *e = &k->entries[i];
        if (!ends_with(e->name, ".nca")) continue;
        if (e->size < 0xc00) return bad(c, "missing_input", e->name, "truncated NCA header");
        NcaRecord *r = realloc(k->ncas, (size_t)(k->nncas + 1) * sizeof *r);
        if (!r) { setup_fail(c, "internal", "Out of memory"); return -1; }
        k->ncas = r; r += k->nncas;
        memset(r, 0, sizeof *r);
        if (st_pread(k->fd, r->hdr, 0xc00, e->off)) return st_io_fail(c, "read", "NCA header");
        aes128_xts_decrypt(keys->header_key, 0, r->hdr, 0xc00);
        if (memcmp(r->hdr + 0x200, "NCA3", 4)) return bad(c, "bad_keys", "NCA header", "did not decrypt to NCA3 (wrong header_key in prod.keys, or a damaged game file)");
        if (le64(r->hdr + 0x208) != e->size) return bad(c, "missing_input", e->name, "NCA size does not match its container entry");
        for (int s = 0; s < 4; s++) {
            uint64_t start = le32(r->hdr + 0x240 + 16 * s), end = le32(r->hdr + 0x244 + 16 * s);
            if (!(start == 0 && end == 0) && !(0xc00 <= start * 512 && start < end && end * 512 <= e->size))
                return bad(c, "missing_input", e->name, "NCA section out of bounds");
        }
        memcpy(r->name, e->name, sizeof r->name); r->off = e->off; r->size = e->size; r->content_type = r->hdr[0x205];
        snprintf(r->title_id, sizeof r->title_id, "%016llX", (unsigned long long)le64(r->hdr + 0x210));
        k->nncas++;
    }
    return 0;
}

static int is_bktr(const NcaRecord *r, int s) { return r->hdr[0x400 + s * 512 + 4] == 4; }
int container_select(SetupCtx *c, Container *k, int update) {
    const char *title = c->pins.title_id;
    char update_title[17];
    snprintf(update_title, sizeof update_title, "%.13s800", title);   /* the update's title id: base | 0x800 */
    int programs = 0, program = -1, meta = 0, own = 0, update_meta = 0;
    for (int i = 0; i < k->nncas; i++) {
        const char *tid = k->ncas[i].title_id;
        if (!strcmp(tid, title) || !strcmp(tid, update_title)) own = 1;
        if (k->ncas[i].content_type == 0) { programs++; program = i; }
        if (k->ncas[i].content_type == 1) { meta++; if (!update && strcmp(tid, title)) meta = 99; if (!strcmp(tid, update_title)) update_meta = 1; }
    }
    if (update) {
        if (k->is_xci) return bad(c, "missing_input", "update", "the update must be an NSP");
        if (programs != 1) return bad(c, "unsupported_version", "update", "expected one update Program NCA; combined packages are unsupported");
        NcaRecord *r = &k->ncas[program];
        if (strcmp(r->title_id, title)) return bad(c, "unsupported_version", "update", "it belongs to another game");
        int sections = 0;
        for (int s = 0; s < 4; s++) if (is_bktr(r, s)) { sections++; k->bktr_section = s; }
        if (!sections) return bad(c, "unsupported_version", "update", "expected a BKTR update, not a base game");
        if (sections != 1) return bad(c, "unsupported_version", "update", "expected one BKTR section");
        for (int i = 0; i < k->nncas; i++)
            if (strcmp(k->ncas[i].title_id, title) && strcmp(k->ncas[i].title_id, update_title))
                return bad(c, "unsupported_version", "update", "mixed-title update package");
    } else {
        /* The two mistakes a player makes, named plainly: another game's file, or the update picked as the base game. */
        if (!own) return bad(c, "unsupported_version", "base game", "it belongs to another game, not Tears of the Kingdom");
        if (update_meta) return bad(c, "unsupported_version", "base game", "this is the update, not the base game 1.0.0");
        if (meta > 1) return bad(c, "unsupported_version", "base game", "expected Tears of the Kingdom base game metadata; updates, DLC and combined packages are unsupported");
        if (programs != 1) return bad(c, "unsupported_version", "base game", "expected one base Program NCA; updates, DLC and combined packages are unsupported");
        if (strcmp(k->ncas[program].title_id, title))
            return bad(c, "unsupported_version", "base game", "expected Tears of the Kingdom base game 1.0.0; updates, DLC and other games are unsupported");
    }
    k->program = program;
    return 0;
}

/* The program's AES-CTR key (key area or common ticket), then a 16-byte probe of the ExeFS PFS0 magic so wrong or too
   old keys are reported here instead of as a hactool failure. */
int container_check_key(SetupCtx *c, Container *k, const SetupKeys *keys, const Ticket *extra, int nextra) {
    const NcaRecord *r = &k->ncas[k->program];
    int gen = r->hdr[0x206] > r->hdr[0x220] ? r->hdr[0x206] : r->hdr[0x220];
    gen = gen ? gen - 1 : 0;
    uint8_t key[16], zero[16] = {0};
    if (memcmp(r->hdr + 0x230, zero, 16)) {
        const Ticket *t = NULL;
        for (int i = 0; i < k->ntickets && !t; i++) if (!memcmp(k->tickets[i].rights, r->hdr + 0x230, 16)) t = &k->tickets[i];
        for (int i = 0; i < nextra && !t; i++) if (!memcmp(extra[i].rights, r->hdr + 0x230, 16)) t = &extra[i];
        if (!t) return bad(c, "missing_input", r->name, "missing common ticket for this rights-managed NCA");
        if (gen >= KEY_GENERATIONS || !(keys->titlekek_mask >> gen & 1)) { setup_fail(c, "bad_keys", "prod.keys lacks titlekek_%02x (dump the keys again on a newer firmware)", gen); return -1; }
        memcpy(key, t->key, 16); aes128_ecb_decrypt(keys->titlekek[gen], key, 16);
    } else {
        if (r->hdr[0x207] != 0) return bad(c, "bad_keys", r->name, "unsupported NCA key area");
        if (gen >= KEY_GENERATIONS || !(keys->kak_mask >> gen & 1)) { setup_fail(c, "bad_keys", "prod.keys lacks key_area_key_application_%02x (dump the keys again on a newer firmware)", gen); return -1; }
        uint8_t area[64]; memcpy(area, r->hdr + 0x300, 64); aes128_ecb_decrypt(keys->kak_app[gen], area, 64);
        memcpy(key, area + 32, 16); memset(area, 0, sizeof area);
    }
    int rc = 0;
    for (int s = 0; s < 4; s++) {
        const uint8_t *fs = r->hdr + 0x400 + 512 * s;
        uint64_t start = le32(r->hdr + 0x240 + 16 * s), end = le32(r->hdr + 0x244 + 16 * s);
        if (!start || fs[2] != 1 || fs[3] != 2 || fs[4] != 3) continue;
        char want[65], got[65];
        sha256_hex(r->hdr + 0x280 + 32 * s, want); sha256_buf_hex(fs, 512, got);
        if (strcmp(want, got)) { rc = bad(c, "missing_input", r->name, "FS header hash mismatch (damaged file)"); break; }
        uint64_t pfsoff = le64(fs + 0x40), secsize = (end - start) * 512;
        if (pfsoff > secsize - 32) { rc = bad(c, "missing_input", r->name, "ExeFS out of bounds"); break; }
        uint64_t abs = start * 512 + pfsoff, aligned = abs & ~(uint64_t)15;
        uint8_t ctr[16], probe[48];
        for (int i = 0; i < 8; i++) ctr[i] = fs[0x147 - i];
        for (int i = 0; i < 8; i++) ctr[8 + i] = (uint8_t)((aligned / 16) >> (56 - 8 * i));
        if (st_pread(k->fd, probe, sizeof probe, r->off + aligned)) { rc = st_io_fail(c, "read", "NCA"); break; }
        aes128_ctr(key, ctr, probe, sizeof probe);
        if (memcmp(probe + (abs - aligned), "PFS0", 4)) {
            setup_fail(c, "bad_keys", "The keys do not decrypt %s (key generation %02x): prod.keys or the ticket does not match this game file", r->name, gen);
            rc = -1;
        }
        memset(probe, 0, sizeof probe);
        break;
    }
    memset(key, 0, sizeof key);
    return rc;
}

void container_free(Container *k) {
    free(k->entries);
    if (k->tickets) memset(k->tickets, 0, (size_t)k->ntickets * sizeof *k->tickets);
    free(k->tickets); free(k->ncas);
    memset(k, 0, sizeof *k);
    k->fd = -1;
}
