/* Included after pipe_key/queue declarations. Shader/layout maps remain startup/recorder-owned;
 * workers touch only immutable entries and queue-mutex-owned collection/progress. The existing
 * pcache saver owns all disk writes, including collection flushes and cache-bound resume state. */
#include "vk_pso_keys.h"
#include "vk_pcache_identity.h"
static pipe_ent *g_pw_head, *g_pw_tail;
static int g_pw_active, g_pso_sync, g_pq_active;
static uint64_t g_pw_pause_until;
static _Atomic uint64_t g_pso_pressure_until;
static _Atomic int g_pso_ready, g_pso_warming;
static int g_pso_collect;
static pso_record *g_pso_records;
static uint32_t g_pso_count, g_pso_capacity;
static uint64_t g_pso_generation, g_pso_saved;
static nvn_hmap g_pso_keys = {.klen = PSO_KEY_BYTES};
static uint8_t g_pw_digest[32], g_pw_done[PSO_BITMAP_BYTES];
static uint32_t g_pw_total, g_pw_completed, g_pw_missing, g_pw_failed;
static uint8_t g_pw_saved_done[PSO_BITMAP_BYTES]; /* saver-owned, initialized before ready */
static uint64_t g_pw_ns, g_pw_started;
/* The backend's cache path is bounded to 1100 bytes; appending metadata must never truncate onto that cache. */
static char g_pso_path[1200], g_pw_path[1200];
static int pso_collection_path(const char *cache) {
    if (!cache) {
        mkdir("state", 0700);
        return vk_shader_cache_path("state/vk-pso-collected.bin", g_pso_path, sizeof g_pso_path) != NULL;
    }
    int n = snprintf(g_pso_path, sizeof g_pso_path, "%s.pso-keys", cache);
    if (n >= 0 && (size_t)n < sizeof g_pso_path) return 1;
    g_pso_path[0] = 0; return 0;
}
static void pso_encode(const pipe_key *k, pso_record *r) {
    memset(r, 0, sizeof *r);
    memcpy(r->key, ((vk_shader *)k->vs)->key, strnlen(((vk_shader *)k->vs)->key, 79));
    if (k->fs) memcpy(r->key+80, ((vk_shader *)k->fs)->key, strnlen(((vk_shader *)k->fs)->key, 79));
    pso_put(r->key+160, k->fs ? (uint32_t)((vk_shader *)k->fs)->early_tests | ((vk_shader *)k->fs)->alpha_func<<1 : 0);
    uint8_t *p = r->key+164;
#define PW(x) do { pso_put(p, (uint32_t)(x)); p += 4; } while (0)
    for (unsigned i = 0; i < 8; i++) PW(k->cfmt[i]);
    PW(k->dfmt); PW(k->sfmt); PW(k->ncolor); PW(k->samples); PW(k->blend_en); PW(k->chmask);
    PW(k->a2c); PW(k->poly_mode); PW(k->topo_class); PW(k->depth_clamp); PW(k->logic_op);
    for (unsigned i = 0; i < 8; i++) { const blend_st *b = k->blend+i;
        PW(b->target); PW(b->src_rgb); PW(b->dst_rgb); PW(b->src_a); PW(b->dst_a); PW(b->eq_rgb); PW(b->eq_a);
    }
    for (unsigned i = 0; i < 16; i++) { PW(k->attr[i].fmt); PW(k->attr[i].offset); PW(k->attr[i].stream); }
    for (unsigned i = 0; i < 16; i++) { PW(k->stream[i].instanced); PW(k->stream[i].divisor); }
#undef PW
    r->frequency = 1;
}
static void pso_decode(const pso_record *r, pipe_key *k) {
    memset(k, 0, sizeof *k); const uint8_t *p = r->key+164;
#define PR(x) do { (x) = pso_u32(p); p += 4; } while (0)
    for (unsigned i = 0; i < 8; i++) PR(k->cfmt[i]);
    PR(k->dfmt); PR(k->sfmt); PR(k->ncolor); PR(k->samples); PR(k->blend_en); PR(k->chmask);
    PR(k->a2c); PR(k->poly_mode); PR(k->topo_class); PR(k->depth_clamp); PR(k->logic_op);
    for (unsigned i = 0; i < 8; i++) { blend_st *b = k->blend+i;
        PR(b->target); PR(b->src_rgb); PR(b->dst_rgb); PR(b->src_a); PR(b->dst_a); PR(b->eq_rgb); PR(b->eq_a);
    }
    for (unsigned i = 0; i < 16; i++) { PR(k->attr[i].fmt); PR(k->attr[i].offset); PR(k->attr[i].stream); }
    for (unsigned i = 0; i < 16; i++) { PR(k->stream[i].instanced); PR(k->stream[i].divisor); }
#undef PR
}
/* g_pq_mu held. Frequency counts successful graphics builds, never draw invocations. */
static void pso_collect_success(pipe_ent *e) {
    if (!g_pso_collect) return;
    pso_record r; pso_encode(&e->k, &r); r.capture = e->capture;
    uintptr_t found = (uintptr_t)nvn_hm_get(&g_pso_keys, r.key);
    if (found) {
        pso_record *old = g_pso_records + found-1;
        if (old->frequency < UINT32_MAX) old->frequency++;
        old->capture |= r.capture; g_pso_generation++; e->collected = (uint32_t)found; return;
    }
    if (g_pso_count == PSO_KEY_LIMIT) {
        static int warned; if (!warned++) LOGV("[pso-collect] key limit %u reached; collection coverage incomplete", PSO_KEY_LIMIT);
        return;
    }
    if (g_pso_count == g_pso_capacity) {
        uint32_t cap = g_pso_capacity ? MINV(g_pso_capacity * 2, PSO_KEY_LIMIT) : 64;
        pso_record *next = realloc(g_pso_records, (size_t)cap * sizeof *next);
        if (!next) { RED(4, "[pso-collect] allocation failed; key not recorded"); return; }
        g_pso_records = next; g_pso_capacity = cap;
    }
    g_pso_records[g_pso_count++] = r;
    nvn_hm_put(&g_pso_keys, r.key, (void *)(uintptr_t)g_pso_count);
    e->collected = g_pso_count; g_pso_generation++;
}
static void pso_demand(pipe_ent *e, int capture) {
    if (!atomic_load_explicit(&g_pso_ready, memory_order_relaxed)) return;
    if ((!capture || e->capture) && (!e->list_index || atomic_load_explicit(&e->state, memory_order_acquire) != PE_QUEUED)) return;
    pthread_mutex_lock(&g_pq_mu);
    e->capture |= capture;
    if (capture && e->collected && !g_pso_records[e->collected-1].capture) {
        g_pso_records[e->collected-1].capture = 1; g_pso_generation++;
    }
    /* Promote screen demand too, but never mutate the entry currently being compiled. */
    if (e->list_index && atomic_load_explicit(&e->state, memory_order_acquire) == PE_QUEUED) {
        pipe_ent *prev = NULL, *q = g_pw_head;
        while (q && q != e) { prev = q; q = q->next; }
        if (q) {
            if (prev) prev->next = q->next; else g_pw_head = q->next;
            if (g_pw_tail == q) g_pw_tail = prev;
            e->next = NULL;
            if (g_pq_tail) g_pq_tail->next = e; else g_pq_head = e;
            g_pq_tail = e; pthread_cond_broadcast(&g_pq_cv);
        }
    }
    pthread_mutex_unlock(&g_pq_mu);
}
void vk_pipeline_keys_collect_save(void) {
    if (!atomic_load_explicit(&g_pso_ready, memory_order_acquire) || !g_pso_collect) return;
    pthread_mutex_lock(&g_pq_mu);
    uint64_t generation = g_pso_generation; uint32_t count = g_pso_count;
    pso_record *copy = generation != g_pso_saved ? malloc((size_t)count * sizeof *copy) : NULL;
    if (copy) memcpy(copy, g_pso_records, (size_t)count * sizeof *copy);
    pthread_mutex_unlock(&g_pq_mu);
    if (!copy) return;
    int ok = pso_write(g_pso_path, copy, count); free(copy);
    if (ok) { g_pso_saved = generation; LOGV("[pso-collect] saved %u keys (%zu bytes)", count, PSO_HEADER_BYTES+(size_t)count*PSO_REC_BYTES); }
    else RED(4, "[pso-collect] could not save %s", g_pso_path);
}
int vk_pipeline_keys_dirty(void) {
    if (!atomic_load_explicit(&g_pso_ready, memory_order_acquire) || !g_pw_total) return 0;
    pthread_mutex_lock(&g_pq_mu);
    int dirty = memcmp(g_pw_done, g_pw_saved_done, PSO_BITMAP_BYTES) != 0;
    pthread_mutex_unlock(&g_pq_mu); return dirty;
}
/* Sample BEFORE Vulkan extraction; only a successful atomic cache save can acknowledge these bits. */
void *vk_pipeline_keys_snapshot(void) {
    if (!atomic_load_explicit(&g_pso_ready, memory_order_acquire) || !g_pw_total) return NULL;
    uint8_t *copy = malloc(PSO_BITMAP_BYTES); if (!copy) return NULL;
    pthread_mutex_lock(&g_pq_mu); memcpy(copy, g_pw_done, PSO_BITMAP_BYTES); pthread_mutex_unlock(&g_pq_mu);
    return copy;
}
void vk_pipeline_keys_saved(const char *cache, void *snapshot) {
    if (!snapshot) return;
    uint8_t bytes[8+32+32+PSO_BITMAP_BYTES+32] = {0};
    memcpy(bytes, "TOTKPM01", 8); memcpy(bytes+8, g_pw_digest, 32); memcpy(bytes+72, snapshot, PSO_BITMAP_BYTES); free(snapshot);
    if (!pcache_driver_hash(cache, bytes+40)) return;
    pso_hash(bytes, sizeof bytes-32, bytes+sizeof bytes-32);
    pcache_store store = {.data=bytes, .capacity=sizeof bytes};
    if (pcache_replace(&store, g_pw_path, sizeof bytes)) { memcpy(g_pw_saved_done, bytes+72, PSO_BITMAP_BYTES); return; }
    RED(4, "[pso-prewarm] resume state not saved");
}
static void pso_restore_from(const char *cache) {
    if (!cache || !g_pw_total) return;
    uint8_t bytes[8+32+32+PSO_BITMAP_BYTES+32], sha[32], cache_sha[32];
    FILE *f = fopen(g_pw_path, "rb"); if (!f) return;
    int ok = fread(bytes, 1, sizeof bytes, f) == sizeof bytes && fgetc(f) == EOF && !ferror(f); fclose(f);
    if (!ok || memcmp(bytes, "TOTKPM01", 8) || memcmp(bytes+8, g_pw_digest, 32)) return;
    pso_hash(bytes, sizeof bytes-32, sha);
    if (memcmp(sha, bytes+sizeof bytes-32, 32) || !pcache_driver_hash(cache, cache_sha) || memcmp(cache_sha, bytes+40, 32)) return;
    memcpy(g_pw_done, bytes+72, PSO_BITMAP_BYTES);
    for (uint32_t i = 0; i < g_pw_total; i++) g_pw_completed += (g_pw_done[i/8] >> (i%8)) & 1;
    memcpy(g_pw_saved_done, g_pw_done, PSO_BITMAP_BYTES);
}
static void pso_restore(void) { pso_restore_from(vk_pcache_loaded_path()); }
void vk_pipeline_keys_stats(uint64_t out[6]) {
    if (!atomic_load_explicit(&g_pso_ready, memory_order_acquire)) { memset(out, 0, 6*sizeof *out); return; }
    pthread_mutex_lock(&g_pq_mu);
    out[0] = g_pw_completed; out[1] = g_pw_total; out[2] = g_pw_ns; out[3] = g_pw_missing; out[4] = g_pw_failed; out[5] = g_pso_count;
    pthread_mutex_unlock(&g_pq_mu);
}
void vk_pipeline_keys_pressure(uint64_t now, int busy) {
    if (!atomic_load_explicit(&g_pso_ready, memory_order_relaxed) || !busy) return;
    uint64_t until = now+500000000, old = atomic_load_explicit(&g_pso_pressure_until, memory_order_relaxed);
    while (old < until && !atomic_compare_exchange_weak_explicit(&g_pso_pressure_until, &old, until, memory_order_relaxed, memory_order_relaxed)) {}
}
static int pso_sync_begin(void) {
    if (!atomic_load_explicit(&g_pso_warming, memory_order_relaxed)) return 0;
    pthread_mutex_lock(&g_pq_mu); g_pso_sync++; pthread_mutex_unlock(&g_pq_mu); return 1;
}
static void pso_sync_end(int active) {
    if (!active) return;
    pthread_mutex_lock(&g_pq_mu);
    g_pso_sync--; g_pw_pause_until = host_now_ns()+500000000;
    pthread_cond_broadcast(&g_pq_cv); pthread_mutex_unlock(&g_pq_mu);
}
static void pso_complete(pipe_ent *e, VkResult result, uint64_t dt) {
    if (!e->list_index) return;
    unsigned i = e->list_index-1;
    g_pw_ns += dt;
    if (!result && !(g_pw_done[i/8] >> (i%8) & 1)) { g_pw_done[i/8] |= 1u << (i%8); g_pw_completed++; }
    if (result) g_pw_failed++;
    if (!(g_pw_completed % 64) || !g_pw_head)
        LOGV("[pso-prewarm] progress %u/%u missing=%u failed=%u build_ms=%.1f wall_ms=%.1f", g_pw_completed, g_pw_total, g_pw_missing, g_pw_failed, g_pw_ns/1e6, (host_now_ns()-g_pw_started)/1e6);
}
#ifdef TOTK_VK_TESTS
int vk_pipeline_test_keys(void) {
    unsigned checks = 0, failed = 0;
#define KTEST(c) do { checks++; if (!(c)) { failed++; printf("FAIL keylist line %d: %s\n", __LINE__, #c); } } while (0)
    char cache_name[1100], overlong[1200];
    memset(cache_name, 'x', sizeof cache_name-1); cache_name[sizeof cache_name-1]=0;
    KTEST(pso_collection_path(cache_name) && strlen(g_pso_path)==strlen(cache_name)+9 &&
        !strcmp(g_pso_path+strlen(cache_name), ".pso-keys"));
    memset(overlong, 'x', sizeof overlong-1); overlong[sizeof overlong-1]=0;
    KTEST(!pso_collection_path(overlong) && !g_pso_path[0]);
    vk_shader vs = {0}, fs = {0};
    memset(vs.key, 'a', 64); memset(fs.key, 'b', 64); fs.early_tests = 1;
    pipe_key k = {.vs=&vs, .fs=&fs, .ncolor=2, .samples=4, .blend_en=1, .chmask=255,
        .a2c=1, .poly_mode=2, .topo_class=2, .depth_clamp=1, .logic_op=3, .dfmt=124, .sfmt=127};
    for (unsigned i=0; i<8; i++) {
        k.cfmt[i] = i < 2 ? 37 : 0;
        k.blend[i] = (blend_st){i, 1, 2, 3, 4, 5, 6};
    }
    for (unsigned i=0; i<16; i++) {
        k.attr[i].fmt=0x23+i; k.attr[i].offset=i*16; k.attr[i].stream=i;
        k.stream[i].instanced=i&1; k.stream[i].divisor=i;
    }
    pso_record record; pso_encode(&k, &record); record.capture=1;
    KTEST(pso_valid(&record));
    pipe_key decoded; pso_decode(&record, &decoded); decoded.vs=&vs; decoded.fs=&fs;
    KTEST(!memcmp(&decoded, &k, sizeof k));
    KTEST(pso_write("pso-roundtrip.bin", &record, 1));
    uint32_t count; uint8_t sha[32]; pso_record *loaded=pso_read("pso-roundtrip.bin", &count, sha);
    KTEST(loaded && count==1 && !memcmp(loaded, &record, sizeof record)); free(loaded);
    FILE *f=fopen("pso-roundtrip.bin", "r+b");
    KTEST(f != NULL); if (f) { fseek(f, -1, SEEK_END); fputc(0xff, f); fclose(f); }
    loaded=pso_read("pso-roundtrip.bin", &count, sha); KTEST(!loaded); free(loaded);
    KTEST(pso_allow_background(0,0,0,0,100,100));
    KTEST(!pso_allow_background(1,0,0,0,100,0)); /* demand before background */
    KTEST(!pso_allow_background(0,1,0,0,100,0));
    KTEST(!pso_allow_background(0,0,1,0,100,0));
    KTEST(!pso_allow_background(0,0,0,1,100,0));
    KTEST(!pso_allow_background(0,0,0,0,99,100));
    KTEST(!pso_sync_begin() && !g_pso_sync); /* feature inactive has no extra queue acquisition */
    atomic_store(&g_pso_warming,1);
    int sync_ticket = pso_sync_begin();
    KTEST(sync_ticket && g_pso_sync==1);
    atomic_store(&g_pso_warming,0); /* last background build can finish during this synchronous call */
    pso_sync_end(sync_ticket); KTEST(!g_pso_sync && g_pw_pause_until);
    pipe_ent a={.list_index=1}, b={.list_index=2}, demand={0};
    a.next=&b; g_pw_head=&a; g_pw_tail=&b;
    g_pq_head=g_pq_tail=&demand; atomic_store(&g_pso_ready,1);
    pso_demand(&b,1);
    KTEST(g_pw_head==&a && g_pw_tail==&a && !a.next && demand.next==&b && g_pq_tail==&b && !b.next);
    pso_demand(&b,1); KTEST(demand.next==&b && !b.next); /* idempotent promotion */
    g_pq_head=g_pq_tail=g_pw_head=g_pw_tail=NULL;
    g_pso_collect=1; a.k=k; a.list_index=0; a.capture=1;
    pthread_mutex_lock(&g_pq_mu); pso_collect_success(&a); pso_collect_success(&a); pthread_mutex_unlock(&g_pq_mu);
    KTEST(g_pso_count==1 && g_pso_records[0].frequency==2 && g_pso_records[0].capture==1);
    g_pw_total=9; memset(g_pw_digest,0x51,32); strcpy(g_pw_path,"pso-marker.bin");
    g_pw_done[0]=5; g_pw_done[1]=1;
    f=fopen("pso-cache.bin","wb"); KTEST(f!=NULL); if (f) { fputs("synthetic opaque cache envelope",f); fclose(f); }
    void *snapshot=vk_pipeline_keys_snapshot();
    g_pw_done[0]|=2; /* a concurrent completion after the sampled generation must stay dirty */
    vk_pipeline_keys_saved("pso-cache.bin",snapshot);
    KTEST(vk_pipeline_keys_dirty());
    memset(g_pw_done,0,sizeof g_pw_done); g_pw_completed=0;
    pso_restore_from("pso-cache.bin");
    KTEST(g_pw_completed==3 && g_pw_done[0]==5 && g_pw_done[1]==1);
    KTEST(!vk_pipeline_keys_dirty());
    memset(g_pw_done,0,sizeof g_pw_done); g_pw_completed=0; g_pw_digest[0]^=1;
    pso_restore_from("pso-cache.bin"); KTEST(g_pw_completed==0); /* changed list */
    g_pw_digest[0]^=1;
    f=fopen("pso-cache.bin","ab"); if (f) { fputc('x',f); fclose(f); }
    pso_restore_from("pso-cache.bin"); KTEST(g_pw_completed==0); /* stale/missing/replaced cache cannot acknowledge keys */
    unlink("pso-cache.bin"); pso_restore_from("pso-cache.bin"); KTEST(g_pw_completed==0);
    unlink("pso-marker.bin"); unlink("pso-roundtrip.bin");
    printf("pso-keylist: %u checks, %u failed\n",checks,failed);
#undef KTEST
    return failed != 0;
}
#endif
#ifdef TOTK_VK_TESTS
static int g_pso_test_screen_started, g_pso_test_order_bad;
int vk_pipeline_test_key_order(void) {
    pthread_mutex_lock(&g_pq_mu); int ok = !g_pso_test_order_bad; pthread_mutex_unlock(&g_pq_mu); return ok;
}
#endif
