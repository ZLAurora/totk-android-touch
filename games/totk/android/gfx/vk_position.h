/* Startup-only vertex precision policy. Keep the original seed immutable; only
   the module passed to Vulkan and its cache namespace change. */
#ifndef TOTK_VK_POSITION_H
#define TOTK_VK_POSITION_H
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static inline unsigned vk_position_policy(const char *invariant, const char *nocontract) {
    return (invariant && !strcmp(invariant, "1") ? 1u : 0u) |
           (nocontract && !strcmp(nocontract, "1") ? 2u : 0u);
}
/* Off/off preserves legacy paths; the version also isolates future pass changes. */
static inline const char *vk_position_cache_suffix(unsigned policy) {
    static const char *const suffix[] = {"", ".pos-v1-10", ".pos-v1-01", ".pos-v1-11"};
    return suffix[policy & 3];
}

typedef struct {
    uint32_t offset, type, head, member;
    uint8_t position, needed, invariant, nocontract;
} vk_position_id;
typedef struct { uint32_t id, next; } vk_position_edge;
static inline void vk_position_edge_add(vk_position_id *ids, vk_position_edge *edges,
        size_t *used, uint32_t from, uint32_t to, uint32_t bound) {
    if (!from || from >= bound || !to || to >= bound) return;
    edges[*used] = (vk_position_edge){to, ids[from].head};
    ids[from].head = (uint32_t)(*used)++;
}
/* These core floating arithmetic instructions admit NoContraction. Explicit
   GLSL.std.450 Fma stays fused: the guest already requested that operation. */
static inline int vk_position_float_op(uint32_t op) {
    return op == 127 || op == 129 || op == 131 || op == 133 || op == 136 ||
           op == 140 || op == 141 || (op >= 142 && op <= 148);
}
static inline int vk_position_pointer(const uint32_t *w, const vk_position_id *ids,
        uint32_t id, uint32_t bound) {
    uint32_t t = id < bound ? ids[id].type : 0;
    return t && ids[t].offset && (w[ids[t].offset] & 0xffff) == 32;
}
/* 0 unchanged, 1 decorated, -1 malformed/unsupported position dependency or OOM.
   No mutation on failure. Both switches off is an allocation-free exact no-op.
   The backward slice closes SSA, function arguments/returns and memory aliases
   (all stores to an aliased local count, irrespective of branch/loop order).
   This is deliberately conservative for memory/composite values, not a blanket
   NoContraction on unrelated vertex outputs or on fragment/compute arithmetic. */
static inline int vk_spirv_position(uint32_t **words, size_t *count, unsigned policy) {
    if (!(policy & 3)) return 0;
    const uint32_t *w = *words; size_t nw = *count, annotation = 0;
    if (nw < 5 || w[0] != 0x07230203 || !w[3] || w[3] > (1u << 22) || nw > UINT32_MAX / 2) return -1;
    uint32_t bound = w[3], entry = 0; unsigned entries = 0;
    for (size_t i = 5; i < nw; i += w[i] >> 16) {
        uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
        if (!n || n > nw - i) return -1;
        if (op == 15) {
            if (n < 4 || ++entries != 1) return -1;
            entry = w[i + 1];
        }
        if (!annotation && ((op >= 19 && op <= 39) || op == 59 || op == 54)) annotation = i;
    }
    if (entries != 1) return -1;
    if (entry != 0) return 0; /* Vertex only, even when called without reflection. */
    if (!annotation) return -1;
    vk_position_id *ids = calloc(bound, sizeof *ids);
    uint32_t *queue = (policy & 2) ? malloc((size_t)bound * sizeof *queue) : NULL;
    vk_position_edge *edges = (policy & 2) ? malloc(2 * nw * sizeof *edges) : NULL;
    if (!ids || ((policy & 2) && (!queue || !edges))) { free(ids); free(queue); free(edges); return -1; }
    for (uint32_t id = 0; id < bound; id++) ids[id].head = ids[id].member = UINT32_MAX;
    int result = -1; size_t used = 0, pending = 0, extra = 0;
#define POS_ID(id) ((id) && (id) < bound)
#define POS_EDGE(from, to) vk_position_edge_add(ids, edges, &used, (from), (to), bound)
#define POS_NEED(id) do { uint32_t p_ = (id); if (POS_ID(p_) && !(ids[p_].needed & 1)) { ids[p_].needed |= 1; queue[pending++] = p_; } } while (0)
    /* Types are declared before their uses. Ignore debug/annotation literals. */
    for (size_t i = 5; i < nw; i += w[i] >> 16) {
        uint32_t op = w[i] & 0xffff, n = w[i] >> 16, id = 0;
        if ((op == 32 && n != 4) || ((op == 28 || op == 23 || op == 24) && n != 4) ||
            (op == 29 && n != 3) || (op == 54 && n != 5) || (op == 55 && n != 3) ||
            (op == 59 && n < 4) || (op == 57 && n < 4) || (op == 12 && n < 5)) goto done;
        if (op >= 19 && op <= 39 && n >= 2) id = w[i+1];
        else if (n >= 3 && POS_ID(w[i+1]) && ids[w[i+1]].offset &&
                 (w[ids[w[i+1]].offset] & 0xffff) >= 19 &&
                 (w[ids[w[i+1]].offset] & 0xffff) <= 39 &&
                 (op == 12 || (op >= 41 && op <= 52) || (op >= 54 && op <= 70) || (i > annotation && op >= 77))) {
            id = w[i+2];
            if (!POS_ID(id)) goto done;
            ids[id].type = w[i+1];
        }
        if (id) { if (!POS_ID(id)) goto done; ids[id].offset = (uint32_t)i; }
    }
    /* Decoration targets, including the translator's gl_PerVertex member. */
    for (size_t i = 5; i < nw; i += w[i] >> 16) {
        uint32_t op = w[i] & 0xffff, n = w[i] >> 16;
        if (op == 71 && n >= 3 && POS_ID(w[i+1])) {
            if (n == 4 && w[i+2] == 11 && w[i+3] == 0) ids[w[i+1]].position = 1;
            if (w[i+2] == 18) ids[w[i+1]].invariant = 1;
            if (w[i+2] == 42) ids[w[i+1]].nocontract = 1;
        }
        if (op == 72 && n == 5 && POS_ID(w[i+1]) && w[i+3] == 11 && w[i+4] == 0)
            ids[w[i+1]].member = w[i+2];
    }
    for (uint32_t id = 1; id < bound; id++) {
        if (!vk_position_pointer(w, ids, id, bound)) continue;
        uint32_t t = ids[id].type, at = ids[t].offset;
        if (w[at+3] >= bound) goto done;
        if (w[at+2] == 3 && ids[w[at+3]].member != UINT32_MAX) ids[id].position = 2;
    }
    /* Position pointer provenance, including copies/phi and call parameters. */
    int changed;
    if (policy & 2) do {
        changed = 0; uint32_t function = 0, parameter = 0;
        for (size_t i = 5; i < nw; i += w[i] >> 16) {
            uint32_t op = w[i] & 0xffff, n = w[i] >> 16, id = n >= 3 ? w[i+2] : 0, pos = 0;
            if (op == 54 && n == 5) { function = id; parameter = 0; }
            if (op == 55 && n == 3 && POS_ID(id)) {
                for (size_t j = 5; j < nw; j += w[j] >> 16)
                    if ((w[j] & 0xffff) == 57 && (w[j] >> 16) > 4 + parameter && w[j+3] == function && POS_ID(w[j+4+parameter]))
                        pos |= ids[w[j+4+parameter]].position;
                parameter++;
            } else if ((op == 65 || op == 66) && n >= 5 && POS_ID(id) && POS_ID(w[i+3])) {
                uint32_t base = w[i+3]; pos = ids[base].position;
                if (pos == 2) {
                    uint32_t t = ids[base].type;
                    if (!vk_position_pointer(w, ids, base, bound)) goto done;
                    t = w[ids[t].offset+3];
                    for (uint32_t j = 4; j < n && pos == 2; j++) {
                        if (!POS_ID(t) || !ids[t].offset) goto done;
                        uint32_t at = ids[t].offset, kind = w[at] & 0xffff;
                        if (kind == 30) {
                            uint32_t index = w[i+j];
                            if (!POS_ID(index) || !ids[index].offset) goto done;
                            uint32_t c = ids[index].offset;
                            if ((w[c] & 0xffff) != 43 || (w[c] >> 16) != 4) goto done;
                            pos = w[c+3] == ids[t].member ? 1 : 0;
                        } else if (kind == 28 || kind == 29) t = w[at+2];
                        else goto done;
                    }
                }
            } else if (op == 83 && n == 4 && POS_ID(w[i+3])) pos = ids[w[i+3]].position;
            else if (op == 169 && n == 6 && POS_ID(w[i+4]) && POS_ID(w[i+5])) pos = ids[w[i+4]].position | ids[w[i+5]].position;
            else if (op == 245 && n >= 5) for (uint32_t j = 3; j < n; j += 2) if (POS_ID(w[i+j])) pos |= ids[w[i+j]].position;
            if (pos && POS_ID(id) && vk_position_pointer(w, ids, id, bound) && (ids[id].position | pos) != ids[id].position) {
                ids[id].position |= (uint8_t)pos; changed = 1;
            }
        }
    } while (changed);
    /* Count only missing Invariant decorations, never duplicate existing ones. */
    if (policy & 1) for (size_t i = 5; i < nw; i += w[i] >> 16) {
        uint32_t op = w[i] & 0xffff, n = w[i] >> 16;
        if (op == 71 && n == 4 && w[i+2] == 11 && w[i+3] == 0 && POS_ID(w[i+1])) {
            uint32_t id = w[i+1], at = ids[id].offset;
            if (!at || (w[at] & 0xffff) != 59 || !vk_position_pointer(w, ids, id, bound)) goto done;
            if (w[ids[ids[id].type].offset+2] != 3) continue;
            if (!ids[id].invariant) extra += 3;
        }
        if (op == 72 && n == 5 && w[i+3] == 11 && w[i+4] == 0) {
            int has = 0;
            for (size_t j = 5; j < nw; j += w[j] >> 16)
                if ((w[j] & 0xffff) == 72 && (w[j] >> 16) == 4 && w[j+1] == w[i+1] && w[j+2] == w[i+2] && w[j+3] == 18) has = 1;
            if (!has) extra += 4;
        }
    }
    if (policy & 2) {
        uint32_t function = 0, parameter = 0;
        for (size_t i = 5; i < nw; i += w[i] >> 16) {
            uint32_t op = w[i] & 0xffff, n = w[i] >> 16, id = n >= 3 ? w[i+2] : 0;
            if (op == 54) { function = id; parameter = 0; continue; }
            if (op == 55) {
                for (size_t j = 5; j < nw; j += w[j] >> 16) if ((w[j] & 0xffff) == 57 && (w[j] >> 16) > 4 + parameter && w[j+3] == function) {
                    POS_EDGE(id, w[j+4+parameter]);
                    if (vk_position_pointer(w, ids, id, bound)) POS_EDGE(w[j+4+parameter], id);
                }
                parameter++; continue;
            }
            if (op == 254 && n == 2) { POS_EDGE(function, w[i+1]); continue; }
            if (op == 62 && n >= 3) {
                POS_EDGE(w[i+1], w[i+2]);
                if (POS_ID(w[i+1]) && ids[w[i+1]].position) POS_NEED(w[i+2]);
                continue;
            }
            if ((op == 63 || op == 64) && n >= 3) {
                POS_EDGE(w[i+1], w[i+2]);
                if (POS_ID(w[i+1]) && ids[w[i+1]].position) POS_NEED(w[i+2]);
                continue;
            }
            if (!POS_ID(id) || ids[id].offset != i || !ids[id].type) continue;
            uint32_t first = 3, last = n, skip = UINT32_MAX;
            if (op == 59) { first = 4; }
            else if (op == 61) last = 4;
            else if (op == 57) last = 4; /* function -> returns -> parameters -> call arguments */
            else if (op == 12) first = 5; /* ExtInst set and instruction number are not value operands */
            else if (op == 79 || op == 82) last = 5; /* VectorShuffle / CompositeInsert literal indices */
            else if (op == 81) last = 4; /* CompositeExtract literal indices */
            else if (op == 245) skip = 4; /* alternating predecessor labels */
            else if (op >= 87 && op <= 98) skip = (op == 89 || op == 90 || op == 93 || op == 94 || op == 96 || op == 97) ? 6 : 5;
            else if ((op >= 41 && op <= 52) || op == 45) { /* constants: only composite constituents are IDs */
                if (op != 44 && op != 51) continue;
            } else if (!((op >= 65 && op <= 70) || (op >= 77 && op <= 86) || (op >= 100 && op <= 107) || (op >= 109 && op <= 215))) {
                /* Unknown result operations are refused only if the slice reaches them. */
                ids[id].needed |= 2; continue;
            }
            for (uint32_t j = first; j < last; j++) {
                if (j == skip || (op == 245 && !(j & 1))) continue;
                POS_EDGE(id, w[i+j]);
                if (vk_position_pointer(w, ids, id, bound) && (op == 65 || op == 66 || op == 67 || op == 70 || op == 83 || op == 169 || op == 245) &&
                    vk_position_pointer(w, ids, w[i+j], bound)) POS_EDGE(w[i+j], id);
            }
        }
        for (size_t q = 0; q < pending; q++) {
            if (ids[queue[q]].needed & 2) goto done;
            for (uint32_t e = ids[queue[q]].head; e != UINT32_MAX; e = edges[e].next)
                POS_NEED(edges[e].id);
        }
        for (size_t q = 0; q < pending; q++) {
            uint32_t id = queue[q];
            if (ids[id].offset && vk_position_float_op(w[ids[id].offset] & 0xffff) && !ids[id].nocontract) extra += 3;
        }
    }
    if (!extra) { result = 0; goto done; }
    if (extra > SIZE_MAX / sizeof *w - nw) goto done;
    uint32_t *out = malloc((nw + extra) * sizeof *out);
    if (!out) goto done;
    memcpy(out, w, annotation * sizeof *out); size_t k = annotation;
    if (policy & 1) for (size_t i = 5; i < nw; i += w[i] >> 16) {
        uint32_t op = w[i] & 0xffff, n = w[i] >> 16;
        if (op == 71 && n == 4 && w[i+2] == 11 && w[i+3] == 0 && POS_ID(w[i+1]) && !ids[w[i+1]].invariant) {
            uint32_t id = w[i+1];
            if (w[ids[ids[id].type].offset+2] == 3) { out[k++] = 3u << 16 | 71; out[k++] = id; out[k++] = 18; }
        }
        if (op == 72 && n == 5 && w[i+3] == 11 && w[i+4] == 0) {
            int has = 0;
            for (size_t j = 5; j < nw; j += w[j] >> 16)
                if ((w[j] & 0xffff) == 72 && (w[j] >> 16) == 4 && w[j+1] == w[i+1] && w[j+2] == w[i+2] && w[j+3] == 18) has = 1;
            if (!has) { out[k++] = 4u << 16 | 72; out[k++] = w[i+1]; out[k++] = w[i+2]; out[k++] = 18; }
        }
    }
    for (size_t q = 0; q < pending; q++) {
        uint32_t id = queue[q];
        if (ids[id].offset && vk_position_float_op(w[ids[id].offset] & 0xffff) && !ids[id].nocontract) {
            out[k++] = 3u << 16 | 71; out[k++] = id; out[k++] = 42;
        }
    }
    memcpy(out + k, w + annotation, (nw - annotation) * sizeof *out);
    free(*words); *words = out; *count = nw + extra; result = 1;
done:
    free(ids); free(queue); free(edges);
#undef POS_ID
#undef POS_EDGE
#undef POS_NEED
    return result;
}
#endif
