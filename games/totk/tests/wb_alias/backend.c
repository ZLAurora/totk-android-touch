/* MIT. Link the real totk_vpool.c and nvn_vk.c registry/mapping/Site2 paths
 * with fd-backed owners, without creating a Vulkan device. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#define TOTK_VK_TESTS 1
#include "../../android/gfx/vk_int.h"
#include "../../third_party/d2r_gfx/gfx/nvn/nvn_dirty.c"
#include "../../third_party/d2r_gfx/gfx/nvn/totk_vpool.h"
#include "backing.h"
#include <signal.h>
#include <sys/wait.h>
/* Optional only so the content regression can also link the old production
 * mapping algorithm with the identical no-device registration bridge. */
void vk_wb_test_alias_replace_hook(void (*hook)(void *), void *opaque) __attribute__((weak));
static int (*fixture_fault)(void *, int, void *);
static void (*fixture_prep)(void *, size_t);
void dm_fault_hook_register(int (*fn)(void *, int, void *)) { fixture_fault = fn; }
void dm_guest_write_prep_register(void (*fn)(void *, size_t)) { fixture_prep = fn; }
static void handle_fault(int number, siginfo_t *info, void *context) {
    if (number != SIGSEGV || !fixture_fault || !fixture_fault(info->si_addr, 1, context)) _exit(91);
}
static void wait_for(_Atomic int *flag, const char *label) {
    uint64_t deadline = mach_absolute_time() + 5000000000ull;
    while (!atomic_load_explicit(flag, memory_order_acquire)) {
        if (mach_absolute_time() > deadline) { fprintf(stderr, "timeout: %s\n", label); exit(1); }
        sched_yield();
    }
}
static uint8_t *reserve(size_t bytes) {
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    required(p != MAP_FAILED, "reserve fixture guest"); return p;
}
static uint8_t *span(uint64_t identity, void *address, size_t requested, size_t expected, int supported) {
    required(vk_wb_pool_lock(identity, address, requested), "pin real pool registration");
    size_t available = SIZE_MAX; uint8_t *result = vk_wb_alias_span_locked((uintptr_t)address, requested, &available);
    CHECK((result != NULL) == supported, "alias span support matches exact owner");
    CHECK(available == expected, "alias span clips at precise owner/view/virtual transition");
    vk_wb_pool_unlock(); return result;
}
static int bytes_are(const uint8_t *p, size_t n, uint8_t value) {
    for (size_t i = 0; i < n; i++) if (p[i] != value) return 0;
    return 1;
}
static uint64_t digest_bytes(uint64_t digest, const void *data, size_t size) {
    const uint8_t *p = data;
    for (size_t i = 0; i < size; i++) { digest ^= p[i]; digest *= UINT64_C(1099511628211); }
    return digest;
}
static pool_t physical_pool(uint8_t *mem, size_t size) {
    pool_t p = {.magic = MAGIC('P', 'O', 'O', 'L'), .flags = NVN_MEMORY_POOL_FLAGS_PHYSICAL,
                .mem = mem, .size = size};
    required(totk_vpool_init(&p), "initialize real physical pool object"); return p;
}
static uint64_t virtual_identity(const pool_t *v) {
    uint64_t id = vk_wb_pool_identity(v, v->mem, v->size);
    required(id != 0, "real virtual registration has a lifetime identity"); return id;
}
static void retired(uint64_t id, const void *mem, size_t size) {
    int held = vk_wb_pool_lock(id, mem, size);
    CHECK(!held, "retired registration cannot authorize a borrowed writer");
    if (held) vk_wb_pool_unlock();
}
static pool_t virtual_open(size_t size) {
    pool_t v = {.magic = MAGIC('P', 'O', 'O', 'L'), .flags = NVN_MEMORY_POOL_FLAGS_VIRTUAL, .size = size};
    required(totk_vpool_init(&v), "initialize real virtual pool storage");
    mtl_vpool_add(v.mem, v.size); nvn_dirty_add_range(v.mem, v.size);
    (void)virtual_identity(&v); return v;
}
static void virtual_close(pool_t *v) {
    uint64_t id = virtual_identity(v);
    nvn_dirty_remove_range(v->mem); mtl_vpool_del(v->mem);
    retired(id, v->mem, v->size);
    required(totk_vpool_fini(v), "release real virtual pool storage");
}
static totk_mapping_request mapping(const pool_t *p, size_t po, size_t vo, size_t size) {
    return (totk_mapping_request){.physical_pool = (const NVNmemoryPool *)(const void *)p,
                                 .physical_offset = (int64_t)po, .virtual_offset = (int64_t)vo, .size = size};
}
static uint64_t map_requests(pool_t *v, int count, const totk_mapping_request *requests) {
    uint64_t old = virtual_identity(v);
    required(totk_vpool_map(v, count, requests), "apply real production MapVirtual batch");
    uint64_t fresh = virtual_identity(v);
    CHECK(fresh != old, "MapVirtual re-registration renews virtual lifetime");
    retired(old, v->mem, v->size); return fresh;
}
static void publish32(uint8_t *guest, uint32_t value, int add) {
    CHECK(vk_wb_test_alias_store(guest, &value, sizeof value, 1, add, 1), "actual Site2 event/add32 selector completes");
}
static void publish_bytes(uint8_t *guest, const void *data, size_t size) {
    CHECK(vk_wb_test_alias_store(guest, data, size, 0, 0, 1), "actual Site2 report/copy selector completes");
}
static uint32_t word_at(const uint8_t *p) { uint32_t word; memcpy(&word, p, sizeof word); return word; }
static void owner_view(backing *a, backing *b, int enabled) {
    size_t page = (size_t)getpagesize();
    uint8_t *view = reserve(3 * page);
    uint64_t id = vk_wb_test_alias_view(view, 3 * page, a->guest, 5 * page);
    required(id != 0, "real fd-backed KVIEW registration");
    span(id, view + 13, 3 * page - 13, enabled ? 3 * page - 13 : 0, enabled);
    publish32(view + 128, 0x6937a185, 0);
    CHECK(word_at(a->guest + 5 * page + 128) == 0x6937a185, "KVIEW publication adds exact owner-relative offset");
    CHECK(!vk_wb_test_alias_view(view + page, page, b->guest, b->bytes), "out-of-bounds KVIEW rejected");
    vk_wb_test_alias_remove(view); required(!munmap(view, 3 * page), "release view mapping");
}
static uint64_t map_then_anonymous(backing *a, backing *b, int enabled) {
    size_t unit = TOTK_VPOOL_PAGE;
    pool_t donor = physical_pool(a->guest, a->bytes), v[2] = {virtual_open(6 * unit), virtual_open(6 * unit)};
    uint8_t *before_a = malloc(a->bytes), *before_b = malloc(b->bytes), *expected = calloc(2, unit);
    required(before_a && before_b && expected, "allocate exact MapVirtual content oracle");
    memcpy(before_a, a->guest, a->bytes); memcpy(before_b, b->guest, b->bytes);
    uint8_t report[97]; for (size_t i = 0; i < sizeof report; i++) report[i] = (uint8_t)(i * 13 + 7);
    uint32_t event = 0x19ac7253, add = 23, final = event + add;
    memcpy(expected + 16, &final, sizeof final); memcpy(expected + unit - 31, report, sizeof report);
    totk_mapping_request requests[] = {mapping(&donor, 2 * unit, 0, 4 * unit),
                                      mapping(&donor, 3 * unit, 5 * unit, unit),
                                      mapping(NULL, 0, unit, 2 * unit)};
    for (unsigned split = 0; split < 2; split++) {
        uint64_t id;
        if (!split) id = map_requests(&v[split], 3, requests);
        else { map_requests(&v[split], 2, requests); id = map_requests(&v[split], 1, requests + 2); }
        uint8_t *hole = v[split].mem + unit, *other_alias = v[split].mem + 5 * unit;
        CHECK(bytes_are(hole, 2 * unit, 0), "production anonymous unmap supplies every fresh zero byte");
        span(id, hole + 7, 2 * unit - 7, enabled ? 2 * unit - 7 : 0, 0);
        /* A second batch may conservatively retire authority for surviving
         * mappings along with the old KVIRT registration. Only current bytes,
         * and absence of authority for the anonymous hole, are mandatory. */
        if (!split) {
            span(id, v[split].mem + 13, 4 * unit - 13, enabled ? unit - 13 : 0, enabled);
            span(id, v[split].mem + 3 * unit + 9, unit - 9, enabled ? unit - 9 : 0, enabled);
        }
        void *watch = nvn_dirty_wb_watch(v[split].mem, v[split].size, nvn_dirty_now());
        required(watch != NULL, "watch actual virtual publication destination");
        uint64_t stamp = nvn_dirty_now();
        publish32(hole + 16, event, 0); publish32(hole + 16, add, 1);
        publish_bytes(hole + unit - 31, report, sizeof report);
        CHECK(word_at(hole + 16) == final, "event/add32 is visible at current anonymous guest address, not old donor");
        CHECK(!memcmp(hole, expected, 2 * unit), "all anonymous guest bytes match exact event/report oracle");
        CHECK(!memcmp(a->guest, before_a, a->bytes), "anonymous publications never modify the old physical donor");
        CHECK(!memcmp(b->guest, before_b, b->bytes), "unrelated physical owner remains byte-for-byte untouched");
        CHECK(!memcmp(other_alias, before_a + 3 * unit, unit), "still-live other alias of old donor remains untouched");
        CHECK(!nvn_dirty_since(v[split].mem, v[split].size, stamp) && !nvn_dirty_wb_written(watch),
              "anonymous fallback publications preserve GPU ownership and virtual CPU generation");
        nvn_dirty_wb_unwatch(watch);
    }
    CHECK(!memcmp(v[0].mem, v[1].mem, v[0].size), "one-batch and two-batch MapVirtual have identical complete guest contents");
    uint64_t digest = digest_bytes(UINT64_C(1469598103934665603), v[0].mem, v[0].size);
    digest = digest_bytes(digest, v[1].mem, v[1].size);
    digest = digest_bytes(digest, a->guest, a->bytes); digest = digest_bytes(digest, b->guest, b->bytes);
    virtual_close(&v[0]); virtual_close(&v[1]); free(before_a); free(before_b); free(expected);
    printf("MAPVIRTUAL_ANONYMOUS PASS real-batch two-batch fresh-zero current-guest donor-untouched other-alias resolver-hole digest=%016" PRIx64 "\n", digest);
    return digest;
}
static uint64_t overlapping_maps(backing *a, backing *b, int enabled, uint64_t digest) {
    size_t unit = TOTK_VPOOL_PAGE;
    pool_t pa = physical_pool(a->guest, a->bytes), pb = physical_pool(b->guest, b->bytes), v = virtual_open(8 * unit);
    totk_mapping_request requests[] = {mapping(&pa, unit, 0, 6 * unit), mapping(&pb, 2 * unit, 2 * unit, 3 * unit),
                                      mapping(&pa, 10 * unit, 3 * unit, unit)};
    uint64_t id = map_requests(&v, 3, requests);
    size_t dst[] = {0, 2 * unit, 3 * unit, 4 * unit, 5 * unit}, off[] = {unit, 2 * unit, 10 * unit, 4 * unit, 6 * unit};
    size_t length[] = {2 * unit, unit, unit, unit, unit};
    uint8_t *owner[] = {a->guest, b->guest, a->guest, b->guest, a->guest};
    for (unsigned i = 0; i < 5; i++) {
        span(id, v.mem + dst[i], 6 * unit - dst[i], enabled ? length[i] : 0, enabled);
        uint32_t value = 0x9a531700 + i; publish32(v.mem + dst[i] + 64, value, 0);
        CHECK(word_at(owner[i] + off[i] + 64) == value, "overlap publication reaches exact last-writer owner offset");
        CHECK(!memcmp(v.mem + dst[i], owner[i] + off[i], length[i]), "overlap mappings match complete physical slices");
    }
    required(vk_wb_pool_lock(id, v.mem, v.size), "pin cross-owner production mapping");
    CHECK(vk_wb_alias_range_locked((uintptr_t)v.mem, 6 * unit) == enabled, "range walker supports adjacent distinct owners");
    CHECK(!vk_wb_alias_range_locked((uintptr_t)v.mem, 7 * unit), "range walker rejects a mixed supported/hole extent");
    CHECK(!vk_wb_alias_range_locked(UINTPTR_MAX - 3, 8) && !vk_wb_alias_range_locked((uintptr_t)v.mem, 0), "overflow/empty extents remain unsupported");
    vk_wb_pool_unlock();
    uint8_t report[97]; memset(report, 0xb7, sizeof report); publish_bytes(v.mem + 2 * unit - 31, report, sizeof report);
    CHECK(!memcmp(a->guest + 3 * unit - 31, report, 31) && !memcmp(b->guest + 2 * unit, report + 31, sizeof report - 31),
          "actual Site2 copy splits precisely across two physical owners");
    uint8_t mixed[41]; memset(mixed, 0x6d, sizeof mixed); publish_bytes(v.mem + 6 * unit - 13, mixed, sizeof mixed);
    CHECK(!memcmp(v.mem + 6 * unit - 13, mixed, sizeof mixed) && !memcmp(a->guest + 7 * unit - 13, mixed, 13),
          "actual Site2 copy preserves supported prefix and current anonymous suffix");
    totk_mapping_request invalid = mapping(&pa, a->bytes, 0, unit);
    CHECK(!totk_vpool_map(&v, 1, &invalid) && virtual_identity(&v) == id, "invalid donor request is rejected before registry/mapping mutation");
    digest = digest_bytes(digest, v.mem, v.size); digest = digest_bytes(digest, a->guest, a->bytes);
    digest = digest_bytes(digest, b->guest, b->bytes); virtual_close(&v);
    puts("BACKEND_OFFSETS PASS KOWN KVIEW KVIRT real-MapVirtual byte-offset cross-owner last-writer mixed-copy");
    return digest;
}
static uint64_t fallback_mapping(backing *a, int enabled, uint64_t digest) {
    size_t unit = TOTK_VPOOL_PAGE;
    uint8_t *shared = mmap(NULL, 4 * unit, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    required(shared != MAP_FAILED, "create real unregistered shareable mremap source"); memset(shared, 0x76, 4 * unit);
    pool_t donor = physical_pool(a->guest, a->bytes), plain = physical_pool(shared, 4 * unit), v = virtual_open(4 * unit);
    uint8_t *before = malloc(a->bytes); required(before != NULL, "snapshot fallback old donor"); memcpy(before, a->guest, a->bytes);
    totk_mapping_request requests[] = {mapping(&donor, 0, 0, 4 * unit), mapping(&plain, unit, unit, 2 * unit)};
    uint64_t id = map_requests(&v, 2, requests);
    span(id, v.mem + unit + 9, 3 * unit - 9, enabled ? 2 * unit - 9 : 0, 0);
    CHECK(!memcmp(v.mem + unit, shared + unit, 2 * unit), "production mremap fallback installs actual unregistered shared source");
    publish32(v.mem + unit + 32, 0x71e3a529, 0);
    uint8_t report[97]; memset(report, 0x92, sizeof report); publish_bytes(v.mem + 2 * unit - 17, report, sizeof report);
    CHECK(word_at(shared + unit + 32) == 0x71e3a529 && !memcmp(shared + 2 * unit - 17, report, sizeof report),
          "fallback report/event writes current mremap source, never a stale private writer");
    CHECK(!memcmp(a->guest, before, a->bytes), "mremap fallback invalidates old donor authority without corrupting donor");
    CHECK(!memcmp(v.mem, a->guest, unit) && !memcmp(v.mem + 3 * unit, a->guest + 3 * unit, unit),
          "fallback replacement trims only overlapping old aliases");
    digest = digest_bytes(digest, v.mem, v.size); digest = digest_bytes(digest, shared, 4 * unit);
    virtual_close(&v); required(!munmap(shared, 4 * unit), "release fallback source"); free(before);
    puts("MAPVIRTUAL_FALLBACK PASS real-mremap unsupported-current-source overlap-trim donor-untouched"); return digest;
}
static uint64_t owner_reback(int enabled, uint64_t digest) {
    size_t unit = TOTK_VPOOL_PAGE; backing old, fresh;
    required(!backing_open(&old, 4 * unit, 0, 0) && !backing_open(&fresh, 4 * unit, 0, 0), "create destroy/reback donors");
    memset(old.guest, 0x38, old.bytes); memset(fresh.guest, 0xda, fresh.bytes);
    uint64_t old_id = vk_wb_test_alias_owner(old.guest, old.bytes, old.fd, old.offset); required(old_id != 0, "register retiring owner");
    pool_t donor = physical_pool(old.guest, old.bytes), v = virtual_open(2 * unit);
    totk_mapping_request request = mapping(&donor, 0, 0, 2 * unit); uint64_t vid = map_requests(&v, 1, &request);
    uint8_t *observer = mmap(NULL, old.bytes, PROT_READ | PROT_WRITE, MAP_SHARED, old.fd, (off_t)old.offset);
    required(observer != MAP_FAILED, "retain independent observer of old physical bytes");
    vk_wb_test_alias_remove(old.guest); retired(old_id, old.guest, old.bytes);
    span(vid, v.mem, v.size, enabled ? v.size : 0, 0);
    required(mmap(old.guest, old.bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fresh.fd, (off_t)fresh.offset) == old.guest,
             "reback same guest VA from a distinct real fd");
    uint64_t new_id = vk_wb_test_alias_owner(old.guest, old.bytes, fresh.fd, fresh.offset);
    required(new_id && new_id != old_id, "reback creates distinct owner registration identity");
    retired(old_id, old.guest, old.bytes); span(vid, v.mem, v.size, enabled ? v.size : 0, 0);
    publish32(v.mem + 128, 0x56a27139, 0);
    CHECK(word_at(observer + 128) == 0x56a27139 && bytes_are(fresh.guest, fresh.bytes, 0xda),
          "owner reback cannot revive stale virtual translation to new physical storage");
    vid = map_requests(&v, 1, &request); span(vid, v.mem, v.size, enabled ? v.size : 0, enabled);
    publish32(v.mem + 128, 0x82e71943, 0);
    CHECK(word_at(old.guest + 128) == 0x82e71943 && word_at(fresh.guest + 128) == 0x82e71943 &&
          word_at(observer + 128) == 0x56a27139, "explicit new mapping selects new owner while retired bytes remain untouched");
    digest = digest_bytes(digest, v.mem, v.size); digest = digest_bytes(digest, observer, old.bytes);
    digest = digest_bytes(digest, fresh.guest, fresh.bytes);
    virtual_close(&v); vk_wb_test_alias_remove(old.guest);
    required(!munmap(observer, old.bytes), "release retired backing observer"); backing_close(&old); backing_close(&fresh);
    puts("MAPVIRTUAL_REBACK PASS owner-destroy stale-identity same-VA-new-fd explicit-remap"); return digest;
}
static pool_t fatal_virtual;
static int fatal_channel = -1, fatal_enabled;
static void failed_mapping_authority(void) {
    size_t unit = TOTK_VPOOL_PAGE, available = SIZE_MAX;
    uint64_t id = vk_wb_pool_identity(&fatal_virtual, fatal_virtual.mem, fatal_virtual.size);
    if (!id || !vk_wb_pool_lock(id, fatal_virtual.mem, fatal_virtual.size)) _exit(94);
    void *hole = vk_wb_alias_span_locked((uintptr_t)fatal_virtual.mem + unit, unit, &available);
    int ok = !hole && available == (fatal_enabled ? unit : 0);
    void *prefix = vk_wb_alias_span_locked((uintptr_t)fatal_virtual.mem, unit, &available);
    ok &= (prefix != NULL) == fatal_enabled && available == (fatal_enabled ? unit : 0);
    vk_wb_pool_unlock();
    if (!ok) _exit(94);
    dprintf(fatal_channel, "conservative-authority");
}
static void partial_failure(backing *a, int enabled) {
    int channel[2]; required(!pipe(channel), "create fatal-batch evidence pipe"); fflush(NULL);
    pid_t child = fork(); required(child >= 0, "fork actual fatal MapVirtual batch");
    if (!child) {
        close(channel[0]); fatal_channel = channel[1]; fatal_enabled = enabled;
        size_t unit = TOTK_VPOOL_PAGE; fatal_virtual = virtual_open(2 * unit);
        /* old_size=0 cannot duplicate MAP_PRIVATE anonymous memory: a real
         * EINVAL, without replacing mmap/mremap or bypassing apply_request. */
        uint8_t *private = reserve(unit);
        pool_t good = physical_pool(a->guest, a->bytes), bad = physical_pool(private, unit);
        totk_mapping_request requests[] = {mapping(&good, 2 * unit, 0, 2 * unit), mapping(&bad, 0, unit, unit)};
        required(!atexit(failed_mapping_authority), "install fatal exit authority oracle");
        totk_vpool_map(&fatal_virtual, 2, requests); _exit(95);
    }
    close(channel[1]); int status; required(waitpid(child, &status, 0) == child, "join actual fatal MapVirtual batch");
    char evidence[64] = {0}; ssize_t count = read(channel[0], evidence, sizeof evidence - 1);
    required(count >= 0 && !close(channel[0]), "read fatal-batch authority evidence");
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 2, "production mapping failure retains existing fatal-batch exit policy");
    CHECK(!strcmp(evidence, "conservative-authority"), "failed replacement loses alias authority before fatal batch returns");
    puts("MAPVIRTUAL_PARTIAL PASS real-private-source-EINVAL fatal-batch conservative-authority");
}
typedef struct { uint8_t *address; uint32_t value; _Atomic int done; int result; } completion;
static void *completion_run(void *argument) {
    completion *c = argument;
    c->result = vk_wb_test_alias_store(c->address, &c->value, sizeof c->value, 1, 0, 1);
    atomic_store_explicit(&c->done, 1, memory_order_release); return NULL;
}
static void site2(backing *b, uint64_t identity, int enabled) {
    nvn_dirty_add_range(b->guest, b->bytes);
    void *watch = nvn_dirty_wb_watch(b->guest, b->bytes, nvn_dirty_now()); CHECK(watch != NULL, "Site2 watch arms");
    uint64_t stamp = nvn_dirty_now(), calls = atomic_load(&g_mp_calls); uint32_t value = 0x194ac325, got = 0;
    CHECK(vk_wb_test_alias_store(b->guest + 128, &value, sizeof value, 1, 0, 1), "production Site2 selector stores without lock contention");
    memcpy(&got, b->guest + 128, sizeof got); CHECK(got == value, "Site2 exact report value");
    CHECK(enabled ? atomic_load(&g_mp_calls) == calls : atomic_load(&g_mp_calls) > calls, "uncontended alias has no guest mprotect; legacy exercises production permission calls");
    required(vk_wb_pool_lock(identity, b->guest, b->bytes), "hold registry for same-thread Site2 contention");
    calls = atomic_load(&g_mp_calls); value++;
    CHECK(vk_wb_test_alias_store(b->guest + 128, &value, sizeof value, 1, 1, 1), "same-thread held registry uses nonblocking legacy add32 fallback");
    CHECK(atomic_load(&g_mp_calls) > calls, "same-thread contention reaches actual legacy dirty store");
    completion c = {.address = b->guest + 256, .value = 0x97a45621}; pthread_t worker;
    calls = atomic_load(&g_mp_calls); required(!pthread_create(&worker, NULL, completion_run, &c), "create contended Site2 worker");
    wait_for(&c.done, "Site2 must finish while another thread retains registry");
    CHECK(c.result && atomic_load(&g_mp_calls) > calls, "second-thread fallback completes via real legacy mprotect before registry unlock");
    vk_wb_pool_unlock(); required(!pthread_join(worker, NULL), "join Site2 worker");
    memcpy(&got, b->guest + 256, sizeof got); CHECK(got == c.value, "contended Site2 exact contents");
    CHECK(!nvn_dirty_since(b->guest, b->bytes, stamp) && !nvn_dirty_wb_written(watch), "all Site2 variants retain GPU ownership and unchanged CPU generation");
    nvn_dirty_wb_unwatch(watch); nvn_dirty_remove_range(b->guest);
    puts("SITE2 PASS uncontended same-thread-trylock second-thread-trylock no-deadlock exact-report generations");
}
typedef struct { backing *b; _Atomic int checked, done; int unexpectedly_acquired; } removal;
static void *remove_run(void *argument) {
    removal *r = argument; r->unexpectedly_acquired = vk_wb_test_alias_registry_trylock();
    atomic_store_explicit(&r->checked, 1, memory_order_release);
    vk_wb_test_alias_remove(r->b->guest); atomic_store_explicit(&r->done, 1, memory_order_release); return NULL;
}
static void pinned_lifetime(backing *b, uint64_t identity, int enabled) {
    nvn_dirty_add_range(b->guest, b->bytes); nvn_dirty_protect_exact(b->guest, b->bytes);
    required(vk_wb_pool_lock(identity, b->guest, b->bytes), "pin production registry for landing lifetime");
    size_t available; uint8_t *alias = vk_wb_alias_span_locked((uintptr_t)b->guest, b->bytes, &available);
    void *token = enabled ? nvn_dirty_gpu_write_alias_begin(b->guest, b->bytes) : nvn_dirty_gpu_write_begin(b->guest, b->bytes);
    CHECK(token != NULL, "pinned landing token owns guest pages");
    removal r = {.b = b}; pthread_t thread; required(!pthread_create(&thread, NULL, remove_run, &r), "create real remove worker");
    wait_for(&r.checked, "remove worker actually attempts production registry lock");
    CHECK(!r.unexpectedly_acquired && !atomic_load(&r.done), "pool lock excludes actual concurrent registry admission and teardown");
    CHECK(!enabled || (alias && available == b->bytes), "private backing remains usable while remove is blocked");
    memset(enabled ? alias : b->guest, 0x8e, b->bytes);
    nvn_dirty_gpu_write_end(token); vk_wb_pool_unlock();
    required(!pthread_join(thread, NULL), "join remove after landing unlock");
    CHECK(atomic_load(&r.done), "real pool removal completes after pinned landing releases lifetime");
    int equal = 1; for (size_t i = 0; i < b->bytes; ++i) equal &= b->guest[i] == 0x8e;
    CHECK(equal, "real owner teardown preserves every landed byte in restored guest backing");
    CHECK(!vk_wb_pool_lock(identity, b->guest, b->bytes), "retired registration identity cannot authorize stale alias");
    nvn_dirty_remove_range(b->guest); puts("LIFETIME PASS pinned-token removal-excluded release-remove preserved-contents stale-identity-rejected");
}
typedef struct {
    pool_t *v; totk_mapping_request request;
    _Atomic int entered, checked, done;
    int unexpectedly_acquired, result;
} replacement;
static void replacement_enter(void *argument) {
    replacement *r = argument;
    if (!atomic_exchange_explicit(&r->entered, 1, memory_order_acq_rel)) {
        r->unexpectedly_acquired = vk_wb_test_alias_registry_trylock();
        atomic_store_explicit(&r->checked, 1, memory_order_release);
    }
}
static void *replacement_run(void *argument) {
    replacement *r = argument; r->result = totk_vpool_map(r->v, 1, &r->request);
    atomic_store_explicit(&r->done, 1, memory_order_release); return NULL;
}
static uint64_t pinned_replacements(backing *a, backing *b, int enabled, uint64_t digest) {
    if (!vk_wb_test_alias_replace_hook) {
        puts("PINNED_MAPVIRTUAL UNAVAILABLE old-backend-without-contention-hook"); return digest;
    }
    size_t unit = TOTK_VPOOL_PAGE;
    pool_t pa = physical_pool(a->guest, a->bytes), pb = physical_pool(b->guest, b->bytes);
    for (int unmap = 0; unmap < 2; unmap++) {
        pool_t v = virtual_open(unit); totk_mapping_request first = mapping(&pa, 8 * unit, 0, unit);
        uint64_t id = map_requests(&v, 1, &first);
        uint8_t *physical = a->guest + 8 * unit;
        nvn_dirty_add_range(a->guest, a->bytes); nvn_dirty_protect_exact(physical, unit);
        required(vk_wb_pool_lock(id, v.mem, v.size), "pin real virtual translation for landing");
        size_t available = 0; uint8_t *alias = vk_wb_alias_span_locked((uintptr_t)v.mem, unit, &available);
        required(!enabled || (alias && available == unit), "borrow exact private writer while registration pinned");
        /* Own the physical guest view of the same pages. The virtual view has
         * no BUSY token, so the worker reaches the actual backend registry lock,
         * rather than merely waiting inside dirty_remove_range first. */
        void *token = enabled ? nvn_dirty_gpu_write_alias_begin(physical, unit) : nvn_dirty_gpu_write_begin(physical, unit);
        required(token != NULL, "pinned landing obtains real dirty ownership");
        replacement r = {.v = &v, .request = mapping(unmap ? NULL : &pb, unmap ? 0 : 8 * unit, 0, unit)};
        vk_wb_test_alias_replace_hook(replacement_enter, &r);
        pthread_t worker; required(!pthread_create(&worker, NULL, replacement_run, &r), "start actual concurrent MapVirtual replacement");
        wait_for(&r.checked, "actual MapVirtual reaches backend registry admission");
        CHECK(!r.unexpectedly_acquired && !atomic_load_explicit(&r.done, memory_order_acquire),
              "production remap/unmap cannot acquire registry or complete while private landing is pinned");
        uint8_t value = (uint8_t)(0x5c + unmap); memset(enabled ? alias : physical, value, unit);
        nvn_dirty_gpu_write_end(token); vk_wb_pool_unlock();
        required(!pthread_join(worker, NULL), "join real replacement after landing releases pin");
        vk_wb_test_alias_replace_hook(NULL, NULL);
        CHECK(r.result && atomic_load(&r.done), "actual MapVirtual completes only after release");
        CHECK(bytes_are(physical, unit, value), "last pinned store remains in original donor through concurrent replacement");
        if (unmap) CHECK(bytes_are(v.mem, unit, 0), "concurrent production unmap installs fresh zeros after pinned store");
        else CHECK(!memcmp(v.mem, b->guest + 8 * unit, unit), "concurrent production remap installs new owner after pinned store");
        retired(id, v.mem, v.size);
        span(virtual_identity(&v), v.mem, unit, enabled ? unit : 0, enabled && !unmap);
        digest = digest_bytes(digest, v.mem, v.size); digest = digest_bytes(digest, physical, unit);
        virtual_close(&v); nvn_dirty_remove_range(a->guest);
    }
    puts("PINNED_MAPVIRTUAL PASS real-remap real-unmap admission-blocked token-end registry-release"); return digest;
}
typedef uint64_t path_counts[VK_WB_ALIAS_SITE_COUNT][VK_WB_ALIAS_PATH_COUNT][2];
static const char *const counter_sites[] = {"landing", "store", "publication"};
static const char *const counter_reasons[] = {"alias", "registry", "ownership", "mixed", "non-dmabuf", "retired", "other"};
static void counter_delta(const path_counts before, const path_counts after, const path_counts expected,
                          int enabled, const char *label) {
    for (int site = 0; site < VK_WB_ALIAS_SITE_COUNT; site++)
        for (int reason = 0; reason < VK_WB_ALIAS_PATH_COUNT; reason++)
            for (int metric = 0; metric < 2; metric++) {
                uint64_t delta = after[site][reason][metric] - before[site][reason][metric];
                uint64_t want = enabled ? expected[site][reason][metric] : 0;
                if (delta != want)
                    fprintf(stderr, "counter %s %s/%s/%s: got=%" PRIu64 " expected=%" PRIu64 "\n",
                            label, counter_sites[site], counter_reasons[reason], metric ? "bytes" : "ops", delta, want);
                CHECK(delta == want, "snapshot delta contains only the selected logical operation and byte partition");
            }
}
static void counter_store(vk_wb_alias_site site, uint8_t *guest, const void *data, size_t size,
                          int event32, int add32, int enabled, size_t alias_bytes,
                          vk_wb_alias_reason fallback, size_t fallback_bytes, const char *label) {
    path_counts before, after, expected = {0};
    CHECK(alias_bytes + fallback_bytes == size, "counter oracle partitions every requested store byte");
    if (alias_bytes) { expected[site][VK_WB_ALIAS_PATH_ALIAS][0] = 1; expected[site][VK_WB_ALIAS_PATH_ALIAS][1] = alias_bytes; }
    if (fallback_bytes) { expected[site][fallback][0] = 1; expected[site][fallback][1] = fallback_bytes; }
    uint32_t word = 0;
    if (event32) { memcpy(&word, data, sizeof word); if (add32) word += word_at(guest); }
    vk_wb_test_alias_counts(before);
    int ok = site == VK_WB_ALIAS_STORE ?
        vk_wb_test_alias_store(guest, data, size, event32, add32, 1) :
        vk_wb_test_alias_store_site(site, guest, data, size, event32, add32, 1);
    vk_wb_test_alias_counts(after);
    CHECK(ok, "actual production Site2 selector completes counter sample");
    CHECK(event32 ? word_at(guest) == word : !memcmp(guest, data, size),
          "counter sample publishes every exact requested guest byte");
    counter_delta(before, after, expected, enabled, label);
}
static int counter_paths(int enabled) {
    path_counts initial, zero = {0};
    memset(initial, 0xa5, sizeof initial); vk_wb_test_alias_counts(initial);
    counter_delta(zero, initial, zero, enabled, "initial");
    size_t unit = TOTK_VPOOL_PAGE;
    backing a, b, old;
    required(!backing_open(&a, 8 * unit, 0, 0) && !backing_open(&b, 8 * unit, 0, 0) &&
             !backing_open(&old, unit, 0, 0), "create no-device counter donors");
    memset(a.guest, 0x31, a.bytes); memset(b.guest, 0x52, b.bytes); memset(old.guest, 0x73, old.bytes);
    uint64_t aid = vk_wb_test_alias_owner(a.guest, a.bytes, a.fd, a.offset);
    uint64_t bid = vk_wb_test_alias_owner(b.guest, b.bytes, b.fd, b.offset);
    uint64_t old_id = vk_wb_test_alias_owner(old.guest, old.bytes, old.fd, old.offset);
    required(aid && bid && old_id, "register real fd owners for path counters");
    void *alt = reserve(128 * 1024); stack_t stack = {.ss_sp = alt, .ss_size = 128 * 1024};
    required(!sigaltstack(&stack, NULL), "counter alternate signal stack");
    struct sigaction action = {.sa_sigaction = handle_fault, .sa_flags = SA_SIGINFO | SA_ONSTACK};
    sigemptyset(&action.sa_mask); required(!sigaction(SIGSEGV, &action, NULL), "counter real dirty fault hook");
    nvn_dirty_lag_enable();
    pool_t pa = physical_pool(a.guest, a.bytes), pb = physical_pool(b.guest, b.bytes);
    pool_t po = physical_pool(old.guest, old.bytes), v = virtual_open(8 * unit), stale = virtual_open(unit);
    totk_mapping_request requests[] = {
        mapping(&pa, 0, 0, unit), mapping(&pb, 0, unit, unit),
        mapping(&pa, unit, 2 * unit, unit), mapping(&pb, unit, 3 * unit, unit),
        mapping(NULL, 0, 4 * unit, unit), mapping(&pa, 2 * unit, 5 * unit, unit),
        mapping(NULL, 0, 6 * unit, unit), mapping(&pa, 3 * unit, 7 * unit, unit),
    };
    uint64_t vid = map_requests(&v, sizeof requests / sizeof requests[0], requests);
    totk_mapping_request old_request = mapping(&po, 0, 0, unit);
    uint64_t stale_id = map_requests(&stale, 1, &old_request);
    vk_wb_test_alias_remove(old.guest); retired(old_id, old.guest, old.bytes);
    uint8_t *cpu = reserve(3 * unit);
    uint64_t cpu_id = vk_wb_test_alias_cpu(cpu, unit);
    required(cpu_id != 0, "register actual known-CPU range without private alias");
    required(vk_wb_pool_lock(vid, v.mem, v.size), "pin real virtual range for full-extent classifier");
    if (enabled) {
        CHECK(vk_wb_alias_reason_locked((uintptr_t)v.mem, 4 * unit) == VK_WB_ALIAS_PATH_ALIAS,
              "full supported extent remains alias across four owner segments");
        CHECK(vk_wb_alias_reason_locked((uintptr_t)v.mem, v.size) == VK_WB_ALIAS_PATH_MIXED,
              "full extent classifier sees alternating supported and anonymous segments");
        CHECK(vk_wb_alias_reason_locked((uintptr_t)v.mem + 4 * unit, unit) == VK_WB_ALIAS_PATH_RETIRED,
              "real MapVirtual anonymous hole is retired rather than inferred non-dmabuf");
        CHECK(vk_wb_alias_reason_locked(UINTPTR_MAX - 3, 8) == VK_WB_ALIAS_PATH_OTHER &&
              vk_wb_alias_reason_locked((uintptr_t)v.mem, 0) == VK_WB_ALIAS_PATH_OTHER,
              "overflow and empty classifier extents remain other");
    }
    vk_wb_pool_unlock();
    required(vk_wb_pool_lock(cpu_id, cpu, unit), "pin actual known-CPU registration");
    if (enabled) {
        CHECK(vk_wb_alias_reason_locked((uintptr_t)cpu, unit) == VK_WB_ALIAS_PATH_NON_DMABUF,
              "known CPU registry proof classifies non-dmabuf");
        CHECK(vk_wb_alias_reason_locked((uintptr_t)cpu + 2 * unit, unit) == VK_WB_ALIAS_PATH_OTHER,
              "valid unregistered address has no owner/backing proof");
        CHECK(vk_wb_alias_reason_locked((uintptr_t)cpu + unit - 13, 97) == VK_WB_ALIAS_PATH_MIXED,
              "different unsupported classes classify as a mixed full extent");
    }
    vk_wb_pool_unlock();
    required(vk_wb_pool_lock(stale_id, stale.mem, stale.size), "pin surviving virtual registration after donor retirement");
    if (enabled) CHECK(vk_wb_alias_reason_locked((uintptr_t)stale.mem, stale.size) == VK_WB_ALIAS_PATH_RETIRED,
                       "removed physical donor leaves known retired virtual authority");
    vk_wb_pool_unlock();
    size_t capacity = 4 * unit + 42; uint8_t *data = malloc(capacity);
    required(data != NULL, "allocate full logical counter store span");
    for (size_t i = 0; i < capacity; i++) data[i] = (uint8_t)(i * 17 + 9);
    for (vk_wb_alias_site site = VK_WB_ALIAS_STORE; site <= VK_WB_ALIAS_PUBLICATION; site++) {
        uint32_t value = 0x571c8a23, add = 19;
        counter_store(site, a.guest + 128, data, 0, 0, 0, enabled, 0, VK_WB_ALIAS_PATH_OTHER, 0, "empty-no-store");
        counter_store(site, a.guest + 128, data, 97, 0, 0, enabled, 97, VK_WB_ALIAS_PATH_OTHER, 0, "fd-report");
        counter_store(site, a.guest + 256, &value, sizeof value, 1, 0, enabled, sizeof value, VK_WB_ALIAS_PATH_OTHER, 0, "fd-event");
        counter_store(site, a.guest + 256, &add, sizeof add, 1, 1, enabled, sizeof add, VK_WB_ALIAS_PATH_OTHER, 0, "fd-add32");
        counter_store(site, v.mem + unit - 31, data, 2 * unit + 62, 0, 0, enabled, 2 * unit + 62,
                      VK_WB_ALIAS_PATH_OTHER, 0, "four-supported-owner-segments");
        CHECK(!memcmp(a.guest + unit - 31, data, 31) && !memcmp(b.guest, data + 31, unit) &&
              !memcmp(a.guest + unit, data + unit + 31, unit) &&
              !memcmp(b.guest + unit, data + 2 * unit + 31, 31), "cross-owner counter sample reaches every donor offset");
        counter_store(site, v.mem + 3 * unit - 13, data, capacity, 0, 0, enabled, 2 * unit + 42,
                      VK_WB_ALIAS_PATH_MIXED, 2 * unit, "alternating-alias-hole-once-per-path");
        counter_store(site, v.mem + 4 * unit + 128, data, 97, 0, 0, enabled, 0,
                      VK_WB_ALIAS_PATH_RETIRED, 97, "real-anonymous-hole");
        counter_store(site, stale.mem + 128, data, 97, 0, 0, enabled, 0,
                      VK_WB_ALIAS_PATH_RETIRED, 97, "retired-physical-owner");
        CHECK(bytes_are(old.guest, old.bytes, 0x73), "retired publication never writes the restored former owner guest");
        counter_store(site, cpu + 128, data, 97, 0, 0, enabled, 0,
                      VK_WB_ALIAS_PATH_NON_DMABUF, 97, "known-cpu");
        counter_store(site, cpu + 2 * unit + 128, data, 97, 0, 0, enabled, 0,
                      VK_WB_ALIAS_PATH_OTHER, 97, "valid-unregistered");
        counter_store(site, cpu + unit - 13, data, 97, 0, 0, enabled, 0,
                      VK_WB_ALIAS_PATH_MIXED, 97, "cpu-and-unregistered-once-per-fallback");
        required(vk_wb_pool_lock(aid, a.guest, a.bytes), "hold actual registry throughout contended counter samples");
        counter_store(site, a.guest + 256, &add, sizeof add, 1, 1, enabled, 0,
                      VK_WB_ALIAS_PATH_REGISTRY, sizeof add, "held-registry-event");
        counter_store(site, a.guest + 512, data, 97, 0, 0, enabled, 0,
                      VK_WB_ALIAS_PATH_REGISTRY, 97, "held-registry-report");
        vk_wb_pool_unlock();
    }
    free(data); virtual_close(&v); virtual_close(&stale);
    vk_wb_test_alias_remove(cpu); required(!munmap(cpu, 3 * unit), "release known CPU and unregistered spans");
    vk_wb_test_alias_remove(a.guest); vk_wb_test_alias_remove(b.guest);
    backing_close(&old); backing_close(&a); backing_close(&b);
    stack.ss_flags = SS_DISABLE; required(!sigaltstack(&stack, NULL), "disable counter alternate signal stack");
    required(!munmap(alt, 128 * 1024), "release counter alternate signal stack");
    path_counts counts, after; vk_wb_test_alias_counts(counts);
    for (int site = 0; site < VK_WB_ALIAS_SITE_COUNT; site++)
        for (int reason = 0; reason < VK_WB_ALIAS_PATH_COUNT; reason++)
            printf("WB_ALIAS_COUNTS_SNAPSHOT %s %s %" PRIu64 " %" PRIu64 "\n",
                   counter_sites[site], counter_reasons[reason], counts[site][reason][0], counts[site][reason][1]);
    vk_wb_alias_report("periodic"); vk_wb_test_alias_counts(after);
    counter_delta(counts, after, zero, enabled, "report-does-not-reset-cumulative");
    printf("WB_ALIAS_COUNTERS %s alias=%d unit=%zu actual-selector store-publication cross-owner mixed-once retired-hole known-cpu held-registry\n",
           failures ? "FAIL" : "PASS", enabled, unit);
    return failures != 0;
}
static pthread_barrier_t pause_init_start;
static void *pause_during_init(void *unused) {
    (void)unused;
    vk_wb_alias_report("pause"); /* Real reporter before initialization must be safe and silent. */
    pthread_barrier_wait(&pause_init_start);
    for (unsigned i = 0; i < 256; i++) vk_wb_alias_report("pause");
    return NULL;
}
static int pause_init_race(int enabled) {
    required(!pthread_barrier_init(&pause_init_start, NULL, 2), "create pause/init rendezvous");
    pthread_t reporter;
    required(!pthread_create(&reporter, NULL, pause_during_init, NULL), "start real pause reporter");
    pthread_barrier_wait(&pause_init_start);
    vk_wb_test_alias_init(); /* Calls the actual wb_alias_init, without initializing Vulkan. */
    required(!pthread_join(reporter, NULL), "join pause reporter after concurrent initialization");
    required(!pthread_barrier_destroy(&pause_init_start), "retire pause/init rendezvous");
    CHECK(vk_wb_alias_enabled() == enabled, "concurrent reporter does not change switch selection");
    vk_wb_alias_report("pause");
    path_counts counts; vk_wb_test_alias_counts(counts);
    for (int site = 0; site < VK_WB_ALIAS_SITE_COUNT; site++)
        for (int reason = 0; reason < VK_WB_ALIAS_PATH_COUNT; reason++)
            CHECK(!counts[site][reason][0] && !counts[site][reason][1], "pause/init does not invent writer traffic");
    printf("WB_ALIAS_PAUSE_INIT %s alias=%d real-reporter concurrent-real-init\n",
           failures ? "FAIL" : "PASS", enabled);
    return failures != 0;
}
int main(int argc, char **argv) {
    int gem = argc > 1 && !strcmp(argv[1], "i915"); const char *option = getenv("TOTK_WB_ALIAS"); int enabled = option && !strcmp(option, "1");
    if (argc > 1 && !strcmp(argv[1], "pause-init")) return pause_init_race(enabled);
    CHECK(!vk_wb_alias_enabled(), "backend getter does not lazily initialize switch"); vk_wb_test_alias_init();
    CHECK(vk_wb_alias_enabled() == enabled, "backend exact startup environment switch");
    required(!setenv("TOTK_WB_ALIAS", enabled ? "0" : "1", 1), "change post-init environment");
    CHECK(vk_wb_alias_enabled() == enabled, "backend switch frozen after initialization");
    if (argc > 1 && !strcmp(argv[1], "counters")) return counter_paths(enabled);
    size_t bytes = 16 * TOTK_VPOOL_PAGE; backing a, b; int result = backing_open(&a, bytes, gem, 0);
    if (result) { backing_close(&a); return result; }
    result = backing_open(&b, bytes, gem, 0); if (result) { backing_close(&a); backing_close(&b); return result; }
    memset(a.guest, 0x21, a.bytes); memset(b.guest, 0x43, b.bytes);
    uint64_t initial_rows = guest_rows(&a);
    uint64_t aid = vk_wb_test_alias_owner(a.guest, a.bytes, a.fd, a.offset), bid = vk_wb_test_alias_owner(b.guest, b.bytes, b.fd, b.offset);
    CHECK(aid && bid && aid != bid, "real-fd synthetic owners have distinct registration identities");
    CHECK(guest_rows(&a) == initial_rows, "retaining backend-private alias never creates a guest mapping row");
    uint64_t memory[2]; vk_wb_test_alias_memory(memory); CHECK(memory[0] == (enabled ? 2u : 0u) && memory[1] == (enabled ? a.bytes + b.bytes : 0), "private alias mapping and byte accounting is exact");
    owner_view(&a, &b, enabled);
    void *alt = reserve(128 * 1024); stack_t stack = {.ss_sp = alt, .ss_size = 128 * 1024}; required(!sigaltstack(&stack, NULL), "backend alternate stack");
    struct sigaction action = {.sa_sigaction = handle_fault, .sa_flags = SA_SIGINFO | SA_ONSTACK}; sigemptyset(&action.sa_mask); required(!sigaction(SIGSEGV, &action, NULL), "backend fault hook");
    nvn_dirty_lag_enable();
    uint64_t digest = map_then_anonymous(&a, &b, enabled);
    digest = overlapping_maps(&a, &b, enabled, digest);
    digest = fallback_mapping(&a, enabled, digest); digest = owner_reback(enabled, digest);
    partial_failure(&a, enabled); site2(&a, aid, enabled);
    digest = pinned_replacements(&a, &b, enabled, digest); pinned_lifetime(&a, aid, enabled);
    vk_wb_test_alias_memory(memory); CHECK(memory[0] == (enabled ? 1u : 0u), "one owner's remove retires precisely its private mapping");
    vk_wb_test_alias_remove(b.guest); vk_wb_test_alias_memory(memory); CHECK(!memory[0] && !memory[1], "all private mappings and bytes retire with owners");
    backing_close(&a); backing_close(&b); stack.ss_flags = SS_DISABLE; required(!sigaltstack(&stack, NULL), "disable backend altstack"); required(!munmap(alt, 128 * 1024), "release backend altstack");
    printf("MAPVIRTUAL_CONTENTS digest=%016" PRIx64 "\n", digest);
    printf("WB_ALIAS_BACKEND %s alias=%d checks=%u failures=%u\n", failures ? "FAIL" : "PASS", enabled, checks, failures); return failures != 0;
}
