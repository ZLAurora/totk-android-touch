/* Maxwell's logical warp is 32 lanes. Keep raw Vulkan lane ids private to
   this rewrite: ballots select this warp's word, votes compare against its
   active mask, and absolute shuffle targets are rebased into the same warp.
   No singleton fallback: it changes vote-dependent output and allocation. */
#ifndef TOTK_VK_SUBGROUP_H
#define TOTK_VK_SUBGROUP_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Vulkan 1.3 promotes subgroup size control. Geometry is deliberately excluded. */
static inline uint32_t vk_subgroup_required(uint32_t stage, int control,
        uint32_t stages, uint32_t min, uint32_t max) {
    if (!control || stage == 8 || !(stages & stage)) return 0;
    if (min <= 32 && max >= 32) return 32;
    if (min <= 64 && max >= 64) return 64;
    return 0;
}

/* 0: no subgroup use, 1: rewritten, -1: unsupported/malformed/allocation failure.
   On failure the caller must not create an uncorrected module. No mutation on
   failure. Only the translator's observed operations are admitted. */
static inline int vk_spirv_subgroup32(uint32_t **words, size_t *count) {
    const uint32_t *w = *words; size_t nw = *count, first_type = 0, first_fn = 0, extra = 96;
    uint32_t u = 0, b = 0, v4 = 0, lane = 0, size = 0; int uses = 0, ballot_cap = 0, fragment = 0;
    if (nw < 5 || w[0] != 0x07230203 || !w[3] || w[3] > (1u << 22)) return -1;
    for (size_t i = 5; i < nw; ) {
        uint32_t n = w[i] >> 16, op = w[i] & 65535;
        if (!n || n > nw - i) return -1;
        if (extra > SIZE_MAX - 40) return -1;
        if (op == 15) { extra++; if (n >= 4 && w[i+1] == 4) fragment = 1; } /* possible new lane interface */
        if (op == 61 && n >= 4 && (w[i+3] == lane || w[i+3] == size)) { extra += 5; uses = 1; }
        if (op == 17 && n == 2 && w[i+1] == 64) ballot_cap = 1;
        if (op >= 19 && op <= 39 && !first_type) first_type = i;
        if (op == 54 && !first_fn) first_fn = i;
        if (op == 20 && n == 2) b = w[i+1];
        if (op == 21 && n == 4 && w[i+2] == 32 && !w[i+3]) u = w[i+1];
        if (op == 71 && n == 4 && w[i+2] == 11) {
            if (w[i+3] == 41) lane = w[i+1];
            if (w[i+3] == 36) size = w[i+1];
            if ((w[i+3] >= 37 && w[i+3] <= 40) || (w[i+3] >= 4416 && w[i+3] <= 4420)) return -1;
        }
        if (op >= 4421 && op <= 4432) return -1; /* legacy subgroup extension operations */
        if (op == 72 && n == 5 && w[i+3] == 11 &&
            ((w[i+4] >= 36 && w[i+4] <= 41) || (w[i+4] >= 4416 && w[i+4] <= 4420))) return -1;
        if (op >= 333 && op <= 366) {
            if (!((op == 334 || op == 335 || op == 339) && n == 5) && !(op == 345 && n == 6)) return -1;
            extra += 40;
            uses = 1;
        }
        i += n;
    }
    if (!uses) return 0;
    if (!first_type || !first_fn) return -1;
    for (size_t i = 5; i < nw; i += w[i] >> 16)
        if ((w[i] & 65535) == 23 && w[i] >> 16 == 4 && w[i+2] == u && w[i+3] == 4) v4 = w[i+1];
    if (extra > SIZE_MAX / sizeof(uint32_t) || nw > SIZE_MAX / sizeof(uint32_t) - extra) return -1;
    uint32_t *out = (uint32_t *)malloc((nw + extra) * sizeof *out);
    if (!out) return -1;
    size_t k = 5; memcpy(out, w, 20); uint32_t id = w[3];
    int new_u = !u, new_b = !b, new_v4 = !v4, new_lane = !lane;
    if (!u) u = id++;
    if (!b) b = id++;
    if (!v4) v4 = id++;
    uint32_t ptr = 0;
    if (!lane) { ptr = id++; lane = id++; }
    uint32_t zero = id++, c31 = id++, c32 = id++, c5 = id++, base_mask = id++, truth = id++, scope = id++;
#define SG_EMIT(op, ...) do { const uint32_t a_[] = {__VA_ARGS__}; \
    out[k++] = ((uint32_t)(sizeof a_ / 4 + 1) << 16) | (op); \
    memcpy(out + k, a_, sizeof a_); k += sizeof a_ / 4; } while (0)
#define SG_COPY() do { memcpy(out + k, w + i, n * 4); k += n; } while (0)
#define SG_WORD(result, predicate) do { \
    uint32_t vec_ = id++; SG_EMIT(339, v4, vec_, scope, predicate); \
    SG_EMIT(77, u, result, vec_, word); } while (0)
    int cap_done = 0;
    for (size_t i = 5; i < nw; ) {
        uint32_t n = w[i] >> 16, op = w[i] & 65535;
        if (!cap_done && op != 17) { if (!ballot_cap) SG_EMIT(17, 64); cap_done = 1; }
        if (i == first_type && new_lane) {
            SG_EMIT(71, lane, 11, 41);
            if (fragment) SG_EMIT(71, lane, 14); /* integer fragment input: Flat */
        }
        if (i == first_fn) {
            if (new_u) SG_EMIT(21, u, 32, 0);
            if (new_b) SG_EMIT(20, b);
            if (new_v4) SG_EMIT(23, v4, u, 4);
            SG_EMIT(43, u, zero, 0); SG_EMIT(43, u, c31, 31); SG_EMIT(43, u, c32, 32);
            SG_EMIT(43, u, c5, 5); SG_EMIT(43, u, base_mask, 0xffffffe0u);
            SG_EMIT(41, b, truth); SG_EMIT(43, u, scope, 3);
            if (new_lane) { SG_EMIT(32, ptr, 1, u); SG_EMIT(59, ptr, lane, 1); }
        }
        if (op == 15 && new_lane) {
            out[k++] = ((n + 1) << 16) | op; memcpy(out+k, w+i+1, (n-1)*4); k += n-1; out[k++] = lane;
        } else if (op == 61 && n >= 4 && (w[i+3] == lane || w[i+3] == size)) {
            if (w[i+3] == size) SG_EMIT(83, w[i+1], w[i+2], c32);
            else { uint32_t raw = id++; SG_EMIT(61, u, raw, lane); SG_EMIT(199, u, w[i+2], raw, c31); }
        } else if (op == 334 || op == 335 || op == 339 || op == 345) {
            uint32_t raw = id++, word = id++; SG_EMIT(61, u, raw, lane); SG_EMIT(194, u, word, raw, c5);
            if (op == 345) {
                uint32_t base = id++, target = id++, absolute = id++;
                SG_EMIT(199, u, base, raw, base_mask); SG_EMIT(199, u, target, w[i+5], c31);
                SG_EMIT(197, u, absolute, base, target);
                SG_EMIT(345, w[i+1], w[i+2], w[i+3], w[i+4], absolute);
            } else {
                uint32_t bits = id++; SG_WORD(bits, w[i+4]);
                if (op == 339) SG_EMIT(80, w[i+1], w[i+2], bits, zero, zero, zero);
                else if (op == 335) SG_EMIT(171, w[i+1], w[i+2], bits, zero);
                else { uint32_t active = id++; SG_WORD(active, truth); SG_EMIT(170, w[i+1], w[i+2], bits, active); }
            }
        } else SG_COPY();
        i += n;
    }
#undef SG_WORD
#undef SG_COPY
#undef SG_EMIT
    out[3] = id; free(*words); *words = out; *count = k; return 1;
}
#endif
