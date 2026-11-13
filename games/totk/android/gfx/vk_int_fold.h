/* Exact low-bit propagation for scalar SPIR-V integer address arithmetic.
 * The translator splits a byte address into a vec4 index and component. Mesa
 * can miss the alignment when it crosses Function-local loads/stores, turning
 * a known component into a four-way dynamic extract. No bounds, FP or NaN
 * semantics change here. Unknown operations remain unknown.
 */
#ifndef TOTK_VK_INT_FOLD_H
#define TOTK_VK_INT_FOLD_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint32_t known, value; } vk_int_bits;
typedef struct {
    vk_int_bits bits, stored;
    uint32_t type, pointee, stored_block;
    unsigned scalar : 1, integer : 1, local : 1, escaped : 1;
    unsigned decorated : 1, relaxed : 1, group : 1;
} vk_int_fact;

static unsigned vk_int_prefix(uint32_t bits) {
    unsigned n = 0;
    while (n < 32 && (bits & 1)) { n++; bits >>= 1; }
    return n;
}
static uint32_t vk_int_mask(unsigned n) { return n == 32 ? UINT32_MAX : (1u << n) - 1; }
static vk_int_bits vk_int_binary(unsigned op, vk_int_bits a, vk_int_bits b) {
    vk_int_bits r = {0};
    uint32_t k = a.known, x = a.value, l = b.known, y = b.value;
    if (op == 199) { /* OpBitwiseAnd */
        uint32_t z = (k & ~x) | (l & ~y), o = k & x & l & y;
        r = (vk_int_bits){z | o, o};
    } else if (op == 197) { /* OpBitwiseOr */
        uint32_t z = k & ~x & l & ~y, o = (k & x) | (l & y);
        r = (vk_int_bits){z | o, o};
    } else if (op == 128 || op == 132) { /* OpIAdd / OpIMul: modulo 2^32 */
        unsigned n = vk_int_prefix(k), m = vk_int_prefix(l);
        if (m < n) n = m;
        if (op == 132) {
            unsigned z = vk_int_prefix(k & ~x) + vk_int_prefix(l & ~y);
            if (z > 32) z = 32;
            if (z > n) n = z;
        }
        r.known = vk_int_mask(n);
        r.value = (op == 128 ? x + y : x * y) & r.known;
    } else if (l == UINT32_MAX && y < 32) {
        if (op == 194) { /* OpShiftRightLogical; do not infer shift >= width */
            r.known = y ? (k >> y) | (UINT32_MAX << (32 - y)) : k;
            r.value = x >> y;
        } else if (op == 196) { /* OpShiftLeftLogical */
            r.known = (k << y) | vk_int_mask(y); r.value = x << y;
        }
    }
    return r;
}

typedef struct { uint32_t type, value, id; } vk_int_constant;
static size_t vk_int_constant_slot(uint32_t type, uint32_t value, size_t mask) {
    uint32_t hash = value ^ (type * 0x9e3779b9u);
    hash ^= hash >> 16; hash *= 0x85ebca6bu; hash ^= hash >> 13;
    return hash & mask;
}

/* In-place, unchanged IDs/bound/word count. Returns folded definitions, or -1
 * on unsupported framing/bound or allocation failure before mutation. This is
 * not a SPIR-V semantic validator. Only existing same-type OpConstants are
 * reused. No proof state is allocated when the launch switch is off.
 */
static int vk_spirv_int_fold(uint32_t *w, size_t nw) {
    if (nw < 5 || w[0] != 0x07230203 || !w[3] || w[3] > (1u << 20)) return -1;
    uint32_t bound = w[3];
    vk_int_fact *f = calloc(bound, sizeof *f);
    if (!f) return -1;
    /* Validate framing before mutation; collect scalar types, locals and every
     * direct decoration before propagating decoration groups. */
    size_t constants = 0;
    for (size_t i = 5; i < nw; i += w[i] >> 16) {
        unsigned n = w[i] >> 16, op = w[i] & 0xffff;
        if (!n || n > nw - i) { free(f); return -1; }
        if ((op == 21 && n == 4) || (op == 22 && n == 3)) {
            if (w[i+1] < bound && w[i+2] == 32) {
                f[w[i+1]].scalar = 1; f[w[i+1]].integer = op == 21;
            }
        } else if (op == 32 && n == 4 && w[i+1] < bound && w[i+3] < bound) {
            if (w[i+2] == 7) f[w[i+1]].pointee = w[i+3]; /* Function pointer */
        } else if (op == 59 && n >= 4 && w[i+1] < bound && w[i+2] < bound) {
            uint32_t t = f[w[i+1]].pointee;
            if (t && f[t].scalar && w[i+3] == 7) {
                f[w[i+2]].local = 1; f[w[i+2]].pointee = t;
            }
        }
        if (op == 43 && n == 4) constants++;
        if ((op == 71 || op == 332 || op == 5632) && n >= 3 && w[i+1] < bound) {
            f[w[i+1]].decorated = 1;
            if (w[i+2] == 0) f[w[i+1]].relaxed = 1; /* RelaxedPrecision */
        }
        if (op == 73 && n == 2 && w[i+1] < bound) f[w[i+1]].group = 1;
    }
    for (size_t i = 5; i < nw; i += w[i] >> 16) {
        unsigned n = w[i] >> 16, op = w[i] & 0xffff;
        if ((op != 74 && op != 75) || n < 3 || w[i+1] >= bound) continue;
        const vk_int_fact *group = &f[w[i+1]];
        for (unsigned j = 2; j < n; j += op == 75 ? 2 : 1) {
            uint32_t id = w[i+j]; if (id >= bound) continue;
            /* Valid groups do not target other groups. Decline rather than
             * depend on annotation order for unsupported nested groups. */
            if (f[id].group) { free(f); return -1; }
            f[id].decorated |= group->decorated;
            f[id].relaxed |= group->relaxed;
        }
    }
    if (!constants) { free(f); return 0; }
    if (constants > bound) { free(f); return -1; }
    size_t capacity = 2;
    while (capacity < constants * 2) capacity *= 2;
    vk_int_constant *index = calloc(capacity, sizeof *index);
    if (!index) { free(f); return -1; }
    /* Never model an aliased, volatile, or otherwise escaping local. Scanning
     * all function operands is deliberately conservative (literal collisions
     * can disable a local, never enable a fold). */
    int function = 0;
    for (size_t i = 5; i < nw; i += w[i] >> 16) {
        unsigned n = w[i] >> 16, op = w[i] & 0xffff;
        if (op == 54) function = 1;
        if (function) for (unsigned j = 1; j < n; j++) {
            uint32_t id = w[i+j];
            if (id >= bound || !f[id].local) continue;
            if ((op == 59 && j == 2) || (op == 61 && n == 4 && j == 3) ||
                (op == 62 && n == 3 && j == 1)) continue;
            f[id].escaped = 1;
        }
        if (op == 56) function = 0;
    }
    int folded = 0; uint32_t block = 0;
    for (size_t i = 5; i < nw; i += w[i] >> 16) {
        unsigned n = w[i] >> 16, op = w[i] & 0xffff;
        if (op == 248 && !++block) { /* Generation stamps avoid a bound-sized clear per label. */
            for (uint32_t id = 1; id < bound; id++) f[id].stored_block = 0;
            block = 1; /* Only relevant to invalid streams with >UINT32_MAX labels. */
        }
        if (op == 62 && n == 3 && w[i+1] < bound && w[i+2] < bound) {
            vk_int_fact *p = &f[w[i+1]];
            if (p->local && !p->escaped && !p->relaxed && !f[p->pointee].relaxed) {
                p->stored = f[w[i+2]].bits; p->stored_block = block;
            }
        }
        if (op == 21 || op == 22 || op == 32) continue; /* Type declarations have no result type. */
        if (n < 4 || w[i+1] >= bound || w[i+2] >= bound || !f[w[i+1]].scalar) continue;
        uint32_t type = w[i+1], id = w[i+2]; vk_int_bits bits = {0};
        if (op == 43 && n == 4 && f[type].integer) { /* OpConstant */
            bits = (vk_int_bits){UINT32_MAX, w[i+3]};
        } else if (op == 61 && n == 4 && w[i+3] < bound) {
            vk_int_fact *p = &f[w[i+3]];
            if (p->local && !p->escaped && !p->relaxed && !f[p->pointee].relaxed &&
                p->stored_block == block) bits = p->stored;
        } else if ((op == 124 || op == 83) && n == 4 && w[i+3] < bound) {
            bits = f[w[i+3]].bits; /* OpBitcast / OpCopyObject */
        } else if ((op == 128 || op == 132 || op == 194 || op == 196 || op == 197 || op == 199) &&
                   n == 5 && w[i+3] < bound && w[i+4] < bound && f[type].integer) {
            bits = vk_int_binary(op, f[w[i+3]].bits, f[w[i+4]].bits);
        }
        /* Reduced-precision integer/float values and decorated locals may
         * truncate operands/results. They cannot supply exact 32-bit facts. */
        if (f[id].relaxed || f[type].relaxed) bits = (vk_int_bits){0};
        f[id].bits = bits; f[id].type = type;
        if (op == 43 && bits.known == UINT32_MAX) {
            size_t slot = vk_int_constant_slot(type, bits.value, capacity - 1);
            while (index[slot].id && (index[slot].type != type || index[slot].value != bits.value))
                slot = (slot + 1) & (capacity - 1);
            /* Match the old scan's lowest already-seen constant ID exactly. */
            if (!index[slot].id || id < index[slot].id)
                index[slot] = (vk_int_constant){type, bits.value, id};
        }
        /* No decoration is assumed legal on a replacement opcode. Preserve
         * decorated operations (including grouped wrap guarantees) verbatim. */
        if (op == 43 || f[id].decorated || !f[type].integer || bits.known != UINT32_MAX) continue;
        size_t slot = vk_int_constant_slot(type, bits.value, capacity - 1);
        while (index[slot].id && (index[slot].type != type || index[slot].value != bits.value))
            slot = (slot + 1) & (capacity - 1);
        uint32_t c = index[slot].id;
        if (!c) continue;
        /* OpCopyObject plus padding OpNops preserves all original word offsets. */
        w[i] = (4u << 16) | 83; w[i+3] = c;
        for (unsigned j = 4; j < n; j++) w[i+j] = 1u << 16;
        folded++;
    }
    free(index); free(f); return folded;
}
#endif
