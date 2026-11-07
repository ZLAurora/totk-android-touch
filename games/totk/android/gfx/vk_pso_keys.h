/* Portable graphics key list v1. Explicit little-endian words, never C object representations.
 * Shipped asset admission: each stage's root64 must occur in shader_list.tsv V rows.
 * Keep the seed pack's exact optional "-<12 hex>" binding-signature suffix: it is an existing
 * variant identifier, not shader bytes. Missing exact variants are skipped, never remapped.
 * All fixed pipeline inputs are named by the encode/decode pair in vk_draw.c. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../../runtime/host_os.h"
#include "vk_pcache_store.h"
#define PSO_KEY_LIMIT 16384u
#define PSO_STATE_WORDS 155u
/* The stage word at byte 160: early-tests bit 0, alpha comparison (0=off, 1..7) bits 1..3. */
#define PSO_KEY_BYTES (160u + 4u + 4u * PSO_STATE_WORDS)
#define PSO_REC_BYTES (PSO_KEY_BYTES + 8u)
#define PSO_HEADER_BYTES 64u
#define PSO_BITMAP_BYTES (PSO_KEY_LIMIT / 8u)
typedef struct { uint8_t key[PSO_KEY_BYTES]; uint32_t frequency, capture; } pso_record;
_Static_assert(sizeof(pso_record) == PSO_REC_BYTES && offsetof(pso_record, frequency) == PSO_KEY_BYTES &&
               offsetof(pso_record, capture) == PSO_KEY_BYTES+4, "payload storage without padding");
static inline uint32_t pso_u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline void pso_put(uint8_t *p, uint32_t x) {
    p[0] = x; p[1] = x >> 8; p[2] = x >> 16; p[3] = x >> 24;
}
static inline void pso_hash(const void *p, size_t n, uint8_t sha[32]) {
    host_sha256_ctx c; host_sha256_init(&c); host_sha256_update(&c, p, n); host_sha256_final(&c, sha);
}
static inline int pso_name(const uint8_t *p, int optional) {
    size_t n = strnlen((const char *)p, 80);
    if (!n) { if (!optional) return 0; }
    else if (n != 64 && n != 77) return 0;
    for (size_t i = 0; i < n; i++)
        if (i == 64 ? p[i] != '-' : !((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f'))) return 0;
    for (size_t i = n; i < 80; i++) if (p[i]) return 0;
    return 1;
}
static inline int pso_valid(const pso_record *r) {
    if (!pso_name(r->key, 0) || !pso_name(r->key + 80, 1) || pso_u32(r->key + 160) > 15 || !r->frequency || r->capture > 1) return 0;
    const uint8_t *s = r->key + 164;
    uint32_t n = pso_u32(s + 10*4), samples = pso_u32(s + 11*4);
    if (n > 8 || samples > 64 || (samples && (samples & (samples - 1))) || pso_u32(s+12*4) > 255 ||
        pso_u32(s+14*4) > 1 || pso_u32(s+15*4) > 2 || pso_u32(s+16*4) > 2 || pso_u32(s+17*4) > 1 || pso_u32(s+18*4) > 16) return 0;
    for (unsigned i = 0; i < 8; i++) {
        const uint8_t *b = s + (19 + 7*i)*4;
        if (pso_u32(b) > 7) return 0;
        for (unsigned j = 1; j < 5; j++) if (pso_u32(b+4*j) > 127) return 0;
        if (pso_u32(b+20) > 7 || pso_u32(b+24) > 7) return 0;
    }
    for (unsigned i = 0; i < 16; i++)
        if (pso_u32(s+(75+3*i+2)*4) > 15 || pso_u32(s+(123+2*i)*4) > 1) return 0;
    return 1;
}
static inline int pso_write(const char *path, const pso_record *r, uint32_t count) {
    if (count > PSO_KEY_LIMIT) return 0;
    size_t size = (size_t)count * PSO_REC_BYTES;
    uint8_t *data = malloc(size ? size : 1), h[PSO_HEADER_BYTES] = {0};
    if (!data) return 0;
    for (uint32_t i = 0; i < count; i++) {
        if (!pso_valid(r+i)) { free(data); return 0; }
        uint8_t *p = data + (size_t)i * PSO_REC_BYTES;
        memcpy(p, r[i].key, PSO_KEY_BYTES); pso_put(p+PSO_KEY_BYTES, r[i].frequency); pso_put(p+PSO_KEY_BYTES+4, r[i].capture);
    }
    memcpy(h, "TOTKPS01", 8); pso_put(h+8, 1); pso_put(h+12, PSO_REC_BYTES); pso_put(h+16, count); pso_hash(data, size, h+24);
    pcache_store store = {.data=data, .capacity=size, .prefix=h, .prefix_size=sizeof h};
    int ok = pcache_replace(&store, path, size);
    free(data); return ok;
}
/* Strict whole-file validation before admitting a single key. Digest covers ordering/metadata too. */
static inline pso_record *pso_read(const char *path, uint32_t *count, uint8_t digest[32]) {
    *count = 0; FILE *f = fopen(path, "rb"); if (!f) return NULL;
    uint8_t h[PSO_HEADER_BYTES], sha[32]; pso_record *r = NULL;
    if (fread(h, 1, sizeof h, f) != sizeof h || memcmp(h, "TOTKPS01", 8) || pso_u32(h+8) != 1 ||
        pso_u32(h+12) != PSO_REC_BYTES || pso_u32(h+16) > PSO_KEY_LIMIT || pso_u32(h+20) || pso_u32(h+56) || pso_u32(h+60)) goto done;
    uint32_t n = pso_u32(h+16); size_t size = (size_t)n * PSO_REC_BYTES;
    r = malloc(size ? size : 1);
    if (!r || fread(r, 1, size, f) != size || fgetc(f) != EOF || ferror(f)) goto bad;
    pso_hash(r, size, sha); if (memcmp(sha, h+24, 32)) goto bad;
    for (uint32_t i = 0; i < n; i++) {
        /* Key bytes are already canonical. Decode metadata explicitly, including on big-endian hosts,
         * after authenticating the original bytes; no second full-size allocation/key copy is needed. */
        const uint8_t *p = (const uint8_t *)(r+i);
        r[i].frequency = pso_u32(p+PSO_KEY_BYTES); r[i].capture = pso_u32(p+PSO_KEY_BYTES+4);
        if (!pso_valid(r+i)) goto bad;
    }
    *count = n; if (digest) memcpy(digest, sha, 32); goto done;
bad: free(r); r = NULL;
done: fclose(f); return r;
}
/* Demand selection is strict priority; driver calls already in flight cannot be cancelled. */
static inline int pso_allow_background(int demand, int sync, int busy, int active, uint64_t now, uint64_t pause_until) {
    return !demand && !sync && !busy && !active && now >= pause_until;
}
