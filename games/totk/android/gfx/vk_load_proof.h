/* Straight-line SPIR-V coverage certificates. Unknown values never certify a
   position. No shader names, guessed vertex layouts or bounds are trusted. */
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint8_t position_attr; uint32_t full_outputs; } vk_load_certificate;
typedef struct {
    uint32_t type, storage, base, member, location, builtin, width, literal;
    uint32_t component[4]; /* 1/2: input xy, 3: zero, 4: one; 0 unknown */
    int32_t integer;
} vk_load_value;
static inline vk_load_certificate vk_load_certify(const uint32_t *w, size_t n, int fragment) {
    vk_load_certificate result = {0};
    if (n < 5 || w[0] != 0x07230203 || !w[3] || w[3] > 65536) return result;
    uint32_t bound = w[3], functions = 0, labels = 0, position[4] = {0}, attr = UINT32_MAX;
    vk_load_value *v = calloc(bound, sizeof *v);
    if (!v) return result;
    for (uint32_t i = 0; i < bound; i++) v[i].location = v[i].builtin = v[i].member = UINT32_MAX;
    /* Decorations precede types/variables; member BuiltIn Position is represented
       on its struct type (only member zero is admitted). */
    int bad = 0;
    for (size_t p = 5; p < n && !bad;) {
        unsigned op = w[p] & 65535, z = w[p] >> 16;
        if (!z || z > n - p) { bad = 1; break; }
        const uint32_t *a = w + p;
#define ID(x) ((x) < bound)
        if (op == 71 && z >= 4 && ID(a[1])) {
            if (a[2] == 30) v[a[1]].location = a[3];
            if (a[2] == 11) v[a[1]].builtin = a[3];
            if (a[2] == 31 && a[3] != 0) bad = 1; /* a component-decorated input/output is not an entire location */
        } else if (op == 72 && z >= 5 && ID(a[1]) && a[3] == 11 && a[4] == 0 && a[2] == 0) {
            v[a[1]].builtin = 0;
        } else if (op == 22 && z == 3 && ID(a[1]) && a[2] == 32) v[a[1]].width = 1;
        else if (op == 23 && z == 4 && ID(a[1]) && ID(a[2]) && v[a[2]].width == 1 && a[3] <= 4) v[a[1]].width = a[3];
        else if (op == 32 && z == 4 && ID(a[1]) && ID(a[3])) { v[a[1]].type = a[3]; v[a[1]].storage = a[2]; }
        else if (op == 43 && z == 4 && ID(a[1]) && ID(a[2])) {
            v[a[2]].integer = (int32_t)a[3];
            v[a[2]].literal = 1;
            v[a[2]].width = v[a[1]].width;
            v[a[2]].component[0] = a[3] == 0 ? 3 : a[3] == 0x3f800000 ? 4 : 0;
        } else if (op == 59 && z >= 4 && ID(a[1]) && ID(a[2])) {
            v[a[2]].type = v[a[1]].type; v[a[2]].storage = a[3]; v[a[2]].base = a[2];
        } else if (op == 65 && z == 5 && ID(a[1]) && ID(a[2]) && ID(a[3]) && ID(a[4]) && v[a[4]].literal) {
            v[a[2]].base = v[a[3]].base; v[a[2]].storage = v[a[3]].storage;
            v[a[2]].type = v[a[1]].type; v[a[2]].member = (uint32_t)v[a[4]].integer;
        } else if (op == 61 && z >= 4 && ID(a[1]) && ID(a[2]) && ID(a[3])) {
            uint32_t b = v[a[3]].base;
            v[a[2]].width = v[a[1]].width;
            if (!fragment && v[a[3]].storage == 1 && (!ID(b) || v[b].builtin != UINT32_MAX)) bad = 1;
            if (ID(b) && v[a[3]].storage == 1 && v[b].location < 16 && v[a[3]].member == UINT32_MAX) {
                if (attr != UINT32_MAX && attr != v[b].location) bad = !fragment;
                attr = v[b].location;
                for (unsigned k = 0; k < v[a[2]].width && k < 2; k++) v[a[2]].component[k] = k + 1;
            }
        } else if (op == 81 && z == 5 && ID(a[1]) && ID(a[2]) && ID(a[3]) && a[4] < 4) {
            v[a[2]].width = 1; v[a[2]].component[0] = v[a[3]].component[a[4]];
        } else if (op == 80 && z >= 4 && ID(a[1]) && ID(a[2])) {
            unsigned k = 0; v[a[2]].width = v[a[1]].width;
            for (unsigned j = 3; j < z && k < 4; j++) {
                if (!ID(a[j]) || !v[a[j]].width) break;
                for (unsigned q = 0; q < v[a[j]].width && k < 4; q++) v[a[2]].component[k++] = v[a[j]].component[q];
            }
        } else if (op == 62 && z >= 3 && ID(a[1]) && ID(a[2])) {
            uint32_t b = v[a[1]].base;
            if (ID(b) && v[a[1]].storage == 3) {
                if (fragment) {
                    if (v[b].location < 8 && v[a[1]].member == UINT32_MAX && v[v[a[1]].type].width == 4)
                        result.full_outputs |= 1u << v[b].location;
                    else bad = 1; /* depth, sample mask, partial or indirect output */
                } else if ((v[b].builtin == 0 && v[a[1]].member == UINT32_MAX) ||
                    (v[v[b].type].builtin == 0 && v[a[1]].member == 0)) {
                    memcpy(position, v[a[2]].component, sizeof position);
                } else if (v[b].builtin != UINT32_MAX || v[a[1]].member != UINT32_MAX) bad = 1;
            } else if (!fragment) bad = 1; /* no local memory or indirect stores in a vertex certificate */
        } else if (op == 54) functions++;
        else if (op == 248) labels++;
        else if (!fragment && (op == 12 || op == 63 || op == 64 || (op >= 227 && op <= 242))) bad = 1;
        else if (op == 57 || op == 249 || op == 250 || op == 251 || op == 252 || op == 255 || op == 4416 || op == 5380) bad = 1;
        p += z;
#undef ID
    }
    if (bad || functions != 1 || labels != 1) result = (vk_load_certificate){0};
    else if (!fragment && attr < 16 && position[0] == 1 && position[1] == 2 && position[2] == 3 && position[3] == 4)
        result.position_attr = (uint8_t)(attr + 1);
    free(v); return result;
}
