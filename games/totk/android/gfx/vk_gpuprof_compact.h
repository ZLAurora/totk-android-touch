/* Worker-owned, allocation-free pipeline aggregation. No Vulkan dependencies:
 * the host fixture feeds the same fenced-lease consumer with synthetic queries.
 * IDs are launch-local and never reused. Inactive dictionary entries can be
 * evicted between windows; current-window observations are never evicted. */
#ifndef VK_GPUPROF_COMPACT_H
#define VK_GPUPROF_COMPACT_H
#include <stdint.h>
#include <string.h>
#define GP_COMPACT_KEYS 8192u
#define GP_COMPACT_EXPOSURES 64u
#define GP_COMPACT_COLUMNS 20u
enum {
    GC_OBSERVED, GC_SELECTED, GC_VALID, GC_MISSING, GC_DISCARDED,
    GC_DRAWS, GC_DISPATCHES, GC_PRIMITIVES, GC_UNKNOWN_DRAWS,
    GC_MS, GC_SQUARE, GC_ELIGIBLE, GC_ELIGIBLE_MS, GC_ELIGIBLE_WEIGHTED,
    GC_ELIGIBLE_SQUARE, GC_ELIGIBLE_MISSING, GC_OUT_OF_RANGE,
    GC_SAMPLED_DRAWS, GC_SAMPLED_DISPATCHES, GC_WEIGHTED
};
static const char *const gp_compact_columns[GP_COMPACT_COLUMNS] = {
    "observed_groups", "selected_groups", "valid_samples", "missing_selected", "discarded_groups",
    "draws", "dispatches", "known_primitives", "unknown_primitive_draws", "gpu_ms_sum",
    "gpu_ms_square_sum", "eligible_samples", "eligible_gpu_ms_sum", "eligible_weighted_gpu_ms_sum",
    "eligible_weighted_gpu_ms_square_sum", "eligible_missing_selected", "out_of_range_groups",
    "sampled_draws", "sampled_dispatches", "weighted_gpu_ms_sum"
};
/* Names are immutable and remain alive until the consumer stops. The renderer's
   published vk_shader objects never move or die (vk_shader.c); fixtures obey the
   same lifetime. Dictionary text is still copied/compared exactly on interning. */
typedef struct { uint64_t pipeline; uint32_t compute; const char *shader[3]; } gp_compact_key;
typedef union { double real; uint64_t integer; } gp_compact_value;
typedef struct {
    gp_compact_key key;
    uint64_t id, hash, frame, window, announced_rotation;
    uint32_t map_slot, selected_discarded;
    /* Integer columns never need per-observation integer-to-double conversion.
       Each column has one representation, selected explicitly by the encoder. */
    gp_compact_value v[GP_COMPACT_COLUMNS];
    double pending[5]; /* valid, ms, weighted ms, weighted square, missing */
    char shader[3][80]; /* owned canonical text; hot source-pointer tuple is in key */
} gp_compact_entry;
typedef struct {
    uint32_t population, numerator, denominator;
    uint64_t frames, eligible_frames, observed, selected;
} gp_compact_exposure;
typedef struct {
    gp_compact_entry key[GP_COMPACT_KEYS];
    uint32_t map[GP_COMPACT_KEYS * 2], nkeys, evict;
    uint32_t touched[GP_COMPACT_KEYS], ntouched, map_dirty;
    gp_compact_exposure exposure[GP_COMPACT_EXPOSURES];
    uint32_t nexposure;
    uint64_t next_id, start, end, frames, eligible_frames, domain_excluded_frames;
    uint64_t observed, selected, valid, missing, timestamps, dropped, unretained, out_of_range, discarded;
    uint64_t unavailable, invalid, query_failures, key_overflow, aggregate_overflow, frame_overflow;
    uint64_t frame, frame_observed, frame_selected, frame_out_of_range, frame_dropped;
    uint32_t frame_population, frame_numerator;
    int frame_bad, counts_complete;
    double cpu_ms;
} gp_compact;
static uint64_t gp_compact_hash(const gp_compact_key *key) {
    /* The handle distributes buckets; full shader strings below still decide
       identity (including a reused handle). Do not rehash 140+ shader bytes for
       every optional observation. Mix aligned Vulkan handles before masking. */
    uint64_t h = key->pipeline ^ ((uint64_t)key->compute << 63);
    h ^= h >> 30; h *= UINT64_C(0xbf58476d1ce4e5b9);
    h ^= h >> 27; h *= UINT64_C(0x94d049bb133111eb);
    return h ^ (h >> 31);
}
static gp_compact_entry *gp_compact_get(gp_compact *c, const gp_compact_key *key) {
    uint64_t hash = gp_compact_hash(key);
    unsigned at = hash & (GP_COMPACT_KEYS * 2 - 1), insert = UINT32_MAX;
    for (unsigned n = 0; n < GP_COMPACT_KEYS * 2; n++, at = (at + 1) & (GP_COMPACT_KEYS * 2 - 1)) {
        uint32_t index = c->map[at];
        if (!index) { if (insert == UINT32_MAX) insert = at; break; }
        if (index == UINT32_MAX) { if (insert == UINT32_MAX) insert = at; continue; }
        gp_compact_entry *e = &c->key[index - 1];
        if (e->hash != hash || e->key.pipeline != key->pipeline || e->key.compute != key->compute) continue;
        int same_source = e->key.shader[0] == key->shader[0] && e->key.shader[1] == key->shader[1] &&
            e->key.shader[2] == key->shader[2];
        if (!same_source) {
            if (strcmp(e->shader[0], key->shader[0] ? key->shader[0] : "") ||
                strcmp(e->shader[1], key->shader[1] ? key->shader[1] : "") ||
                strcmp(e->shader[2], key->shader[2] ? key->shader[2] : "")) continue;
            e->key = *key;
        }
        e->window = c->start; return e;
    }
    if (insert == UINT32_MAX || c->next_id == UINT64_MAX) { c->aggregate_overflow++; return NULL; }
    size_t length[3];
    for (unsigned i = 0; i < 3; i++) {
        length[i] = key->shader[i] ? strnlen(key->shader[i], 80) : 0;
        if (length[i] == 80) { c->aggregate_overflow++; c->counts_complete = 0; return NULL; }
    }
    unsigned index = c->nkeys;
    if (index == GP_COMPACT_KEYS) {
        unsigned n;
        for (n = 0; n < GP_COMPACT_KEYS; n++) {
            index = c->evict++ & (GP_COMPACT_KEYS - 1);
            if (c->key[index].window != c->start) break;
        }
        if (n == GP_COMPACT_KEYS) { c->key_overflow++; c->counts_complete = 0; return NULL; }
        c->map[c->key[index].map_slot] = UINT32_MAX;
        c->map_dirty = 1;
    } else c->nkeys++;
    gp_compact_entry *e = &c->key[index];
    memset(e, 0, sizeof *e);
    e->key = *key; e->hash = hash; e->id = ++c->next_id; e->window = c->start; e->map_slot = insert;
    for (unsigned i = 0; i < 3; i++)
        if (key->shader[i]) memcpy(e->shader[i], key->shader[i], length[i] + 1);
    c->map[insert] = index + 1;
    return e;
}
static void gp_compact_begin(gp_compact *c, uint64_t frame, uint32_t population, uint32_t numerator) {
    if (!c->start) { c->start = (frame - 1) / 300 * 300 + 1; c->counts_complete = 1; }
    if (!c->frame) { c->frame = frame; c->frame_population = population; c->frame_numerator = numerator; }
    if (c->frame != frame || c->frame_population != population || c->frame_numerator != numerator) {
        c->frame_bad = 1; c->frame_overflow++;
    }
}
static void gp_compact_observe(gp_compact *c, const gp_compact_key *key, int selected,
        int valid, int discarded, int out_of_range, double ms, uint32_t population, uint32_t numerator,
        uint64_t draws, uint64_t dispatches, uint64_t primitives, uint64_t unknown_draws) {
    gp_compact_entry *e = gp_compact_get(c, key);
    if (!e) return;
    gp_compact_value *v = e->v;
    if (discarded) {
        v[GC_DISCARDED].integer++; e->selected_discarded += selected; c->frame_bad = 1; return;
    }
    /* Preserve the exact binary64 output contract, without hot FP conversions.
       Overflow makes the whole window non-authoritative, never wraps counts. */
    const uint64_t exact = UINT64_C(9007199254740991);
    if (draws > exact - v[GC_DRAWS].integer || dispatches > exact - v[GC_DISPATCHES].integer ||
        primitives > exact - v[GC_PRIMITIVES].integer || unknown_draws > exact - v[GC_UNKNOWN_DRAWS].integer) {
        c->aggregate_overflow++; c->counts_complete = 0; return;
    }
    v[GC_OBSERVED].integer++; v[GC_SELECTED].integer += selected; v[GC_OUT_OF_RANGE].integer += out_of_range;
    v[GC_DRAWS].integer += draws; v[GC_DISPATCHES].integer += dispatches;
    v[GC_PRIMITIVES].integer += primitives; v[GC_UNKNOWN_DRAWS].integer += unknown_draws;
    if (!selected) return;
    if (e->frame != c->frame) {
        e->frame = c->frame; memset(e->pending, 0, sizeof e->pending);
        c->touched[c->ntouched++] = (uint32_t)(e - c->key);
    }
    if (!valid || !population || !numerator || out_of_range) {
        v[GC_MISSING].integer++; e->pending[4]++; return;
    }
    double weighted = ms * population / numerator;
    v[GC_VALID].integer++; v[GC_MS].real += ms; v[GC_SQUARE].real += ms * ms; v[GC_WEIGHTED].real += weighted;
    v[GC_SAMPLED_DRAWS].integer += draws; v[GC_SAMPLED_DISPATCHES].integer += dispatches;
    e->pending[0]++; e->pending[1] += ms; e->pending[2] += weighted; e->pending[3] += weighted * weighted;
}
static void gp_compact_finish(gp_compact *c, int complete) {
    if (!c->frame) return;
    int eligible = complete && !c->frame_bad && !c->frame_out_of_range && !c->frame_dropped;
    c->frames++; c->eligible_frames += eligible; c->domain_excluded_frames += !eligible; c->end = c->frame;
    for (unsigned i = 0; i < c->ntouched; i++) {
        gp_compact_entry *e = &c->key[c->touched[i]];
        if (eligible) {
            e->v[GC_ELIGIBLE].real += e->pending[0]; e->v[GC_ELIGIBLE_MS].real += e->pending[1];
            e->v[GC_ELIGIBLE_WEIGHTED].real += e->pending[2]; e->v[GC_ELIGIBLE_SQUARE].real += e->pending[3];
            e->v[GC_ELIGIBLE_MISSING].real += e->pending[4];
        }
    }
    unsigned at;
    for (at = 0; at < c->nexposure; at++) if (c->exposure[at].population == c->frame_population &&
        c->exposure[at].numerator == c->frame_numerator) break;
    if (at == GP_COMPACT_EXPOSURES) c->aggregate_overflow++;
    else {
        if (at == c->nexposure) c->exposure[c->nexposure++] = (gp_compact_exposure){
            .population = c->frame_population, .numerator = c->frame_numerator, .denominator = c->frame_population};
        gp_compact_exposure *e = &c->exposure[at];
        e->frames++; e->eligible_frames += eligible; e->observed += c->frame_observed; e->selected += c->frame_selected;
    }
    c->frame = c->frame_observed = c->frame_selected = c->frame_out_of_range = c->frame_dropped = 0;
    c->frame_population = c->frame_numerator = c->ntouched = 0; c->frame_bad = 0;
}
static void gp_compact_reset(gp_compact *c) {
    /* Retain identities and the already-correct index. Only an eviction creates
       tombstones; rebuilding a clean index is a needless cold report walk. */
    int rebuild = c->map_dirty;
    if (rebuild) memset(c->map, 0, sizeof c->map);
    for (unsigned i = 0; i < c->nkeys; i++) {
        gp_compact_entry *e = &c->key[i];
        if (e->window == c->start) {
            memset(e->v, 0, sizeof e->v); memset(e->pending, 0, sizeof e->pending); e->frame = 0;
        }
        e->selected_discarded = 0;
        if (rebuild) {
            unsigned at = e->hash & (GP_COMPACT_KEYS * 2 - 1);
            while (c->map[at]) at = (at + 1) & (GP_COMPACT_KEYS * 2 - 1);
            c->map[at] = i + 1; e->map_slot = at;
        }
    }
    uint64_t next = c->next_id;
    memset(c->touched, 0, sizeof *c - (size_t)((char *)c->touched - (char *)c));
    c->next_id = next;
}
#endif
