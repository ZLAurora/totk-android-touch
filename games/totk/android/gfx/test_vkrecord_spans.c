/* A queued packet must keep its old pool-index version while the walk captures a newer generation. */
#include "vk_record.h"
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

extern void dm_guest_write_prepare(void *addr, size_t size);

static uint64_t projection_now(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static int projection_equal(const vk_record_spans *s, VkBuffer buffer, uint64_t offset, uint64_t size,
                            vk_record_lag_intent **indexed, uint32_t *ci, vk_record_lag_intent **reference, uint32_t *cr,
                            uint32_t *emitted) {
    uint32_t ni = 0, nr = 0, reason = NVN_DIRTY_LAG_READONLY | NVN_DIRTY_LAG_COLD;
    int a = vk_record_test_project(s, buffer, offset, size, reason, 0, indexed, &ni, ci);
    int b = vk_record_test_project(s, buffer, offset, size, reason, 1, reference, &nr, cr);
    *emitted = ni;
    if (a != b || ni != nr) return 0;
    for (uint32_t i = 0; i < ni; i++)
        if ((*indexed)[i].lo != (*reference)[i].lo || (*indexed)[i].hi != (*reference)[i].hi ||
            (*indexed)[i].reason != (*reference)[i].reason || (*indexed)[i].max_range != (*reference)[i].max_range) return 0;
    return 1;
}
static int projection_fixture(int benchmark) {
    enum { UNRELATED = 3000, EXTRA = 8, RANGES = 7000, UNITS = 2000, UNIT = 65536, ROUNDS = 8 };
    const uint64_t base = 0x10000007ull, alias = 0x30000007ull, bytes = (uint64_t)UNITS * UNIT + 777;
    VkBuffer buffer = (VkBuffer)(uintptr_t)1, union_buffer = (VkBuffer)(uintptr_t)2;
    vk_record_test_span *input = calloc(UNRELATED + EXTRA, sizeof *input);
    if (!input) return 1;
    input[0] = (vk_record_test_span){base, base + bytes, buffer, 0};
    input[1] = (vk_record_test_span){base + 17, base + (uint64_t)1000 * UNIT, buffer, 17}; /* nested physical view */
    input[2] = (vk_record_test_span){alias, alias + bytes, buffer, 0};
    input[3] = (vk_record_test_span){0x50000013, 0x50000013 + 2 * UNIT + 31, buffer, (uint64_t)999 * UNIT + 17};
    input[4] = (vk_record_test_span){0x60000000, 0x60000000, buffer, 0}; /* empty */
    input[5] = (vk_record_test_span){0x60010000, 0x60010010, buffer, UINT64_MAX - 8}; /* invalid backing extent */
    input[6] = (vk_record_test_span){0x60020000, 0x60020008, union_buffer, 0};
    input[7] = (vk_record_test_span){0x60020010, 0x60020018, union_buffer, 8};
    for (unsigned i = 0; i < UNRELATED; i++) {
        uint64_t lo = 0x100000000ull + (uint64_t)i * UNIT;
        input[i + EXTRA] = (vk_record_test_span){lo, lo + UNIT, (VkBuffer)(uintptr_t)(i + 3), 0};
    }
    vk_record_lag_intent *indexed = NULL, *reference = NULL;
    uint32_t ci = 0, cr = 0, emitted = 0; int errors = 0;
    for (int linear = 0; linear < 2; linear++) {
        uint64_t start = projection_now();
        vk_record_spans *s = vk_record_test_spans_create(input, UNRELATED + EXTRA, linear);
        uint64_t build_ns = projection_now() - start;
        if (!s) { errors++; break; }
        for (unsigned i = 0; i < RANGES; i++) {
            uint64_t offset = (uint64_t)(i * 37 % UNITS) * UNIT + 19 + i % 97;
            if (!projection_equal(s, buffer, offset, 64 + i % 65, &indexed, &ci, &reference, &cr, &emitted)) errors++;
        }
        struct { VkBuffer buffer; uint64_t offset, size; int ok; uint32_t count; } cases[] = {
            {buffer, 19, 64, 1, 3}, {buffer, bytes - 31, 31, 1, 2},
            {buffer, bytes - 31, 32, 0, 2}, {buffer, bytes, 1, 0, 0},
            {buffer, UINT64_MAX - 7, 16, 0, 0}, {buffer, 0, 0, 1, 0},
            {union_buffer, 0, 16, 0, 2}, /* two partial spans must not invent a covering source */
            {(VkBuffer)(uintptr_t)UINT32_MAX, 0, 16, 0, 0}, {VK_NULL_HANDLE, 0, 16, 0, 0}
        };
        for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
            uint32_t count = 0;
            int ok = vk_record_test_project(s, cases[i].buffer, cases[i].offset, cases[i].size,
                NVN_DIRTY_LAG_READONLY, 0, &indexed, &count, &ci);
            if (ok != cases[i].ok || count != cases[i].count ||
                !projection_equal(s, cases[i].buffer, cases[i].offset, cases[i].size,
                                  &indexed, &ci, &reference, &cr, &emitted)) errors++;
        }
        uint32_t count = 0;
        if (!vk_record_test_project(s, buffer, 19, 64, NVN_DIRTY_LAG_WRITABLE, 0, &indexed, &count, &ci) ||
            count != 3 || indexed[0].lo != base + 19 || indexed[1].lo != base + 19 || indexed[2].lo != alias + 19 ||
            indexed[2].hi != alias + 83 || indexed[2].max_range != 64 ||
            indexed[2].reason != (NVN_DIRTY_LAG_WRITABLE | NVN_DIRTY_LAG_ALIAS)) errors++;
        /* A reused alias address belongs to a new buffer only in the new frozen version. */
        vk_record_test_span saved = input[2]; input[2].buffer = union_buffer;
        vk_record_spans *recycled = vk_record_test_spans_create(input, UNRELATED + EXTRA, linear); input[2] = saved;
        if (!recycled) errors++;
        else {
            count = 0; vk_record_test_project(recycled, buffer, 19, 64, 0, 0, &indexed, &count, &ci);
            if (count != 2) errors++;
            count = 0; vk_record_test_project(s, buffer, 19, 64, 0, 0, &indexed, &count, &ci);
            if (count != 3 || indexed[2].lo != alias + 19) errors++;
            vk_record_test_spans_destroy(recycled);
        }
        if (benchmark) {
            uint64_t elapsed[2], checksum = 0;
            for (int ref = 0; ref < 2; ref++) {
                start = projection_now();
                for (unsigned round = 0; round < ROUNDS; round++) for (unsigned i = 0; i < RANGES; i++) {
                    uint32_t n = 0; uint64_t off = (uint64_t)(i * 37 % UNITS) * UNIT + 19 + i % 97;
                    checksum += vk_record_test_project(s, buffer, off, 64 + i % 65, 1, ref, &indexed, &n, &ci);
                    checksum += n;
                }
                elapsed[ref] = projection_now() - start;
            }
            /* Measure cold version churn separately: copies + index allocation/construction, not warm lookups alone. */
            start = projection_now();
            for (unsigned i = 0; i < 64; i++) {
                vk_record_spans *cold = vk_record_test_spans_create(input, UNRELATED + EXTRA, linear);
                if (!cold) { errors++; break; }
                vk_record_test_spans_destroy(cold);
            }
            uint64_t churn_ns = projection_now() - start;
            printf("lag-bench OFF=span:%d: %d ranges/%d units, %d unrelated spans; indexed %.1f ns/range, full-scan %.1f ns/range; first copy+index %.3f ms, cold copy+index+free %.3f ms/version; checksum %llu\n",
                linear, RANGES, UNITS, UNRELATED, (double)elapsed[0] / (ROUNDS * RANGES),
                (double)elapsed[1] / (ROUNDS * RANGES), build_ns / 1e6, churn_ns / 64e6, (unsigned long long)checksum);
        }
        vk_record_test_spans_destroy(s);
    }
    free(indexed); free(reference); free(input);
    printf("%s: all-alias indexed projection, exact coverage, clipped tails and frozen recycled versions\n", errors ? "FAILED" : "ok");
    return errors;
}

static int same_descriptor(VkDescriptorBufferInfo a, VkDescriptorBufferInfo b) {
    return a.buffer == b.buffer && a.offset == b.offset && a.range == b.range;
}
static void capture_value(VkBuffer destination, unsigned slot, VkDescriptorBufferInfo source) {
    vk_barrier_all(vk_cb());
    VkBufferCopy copy = {source.offset, (uint64_t)slot * 16, 16};
    vkCmdCopyBuffer(vk_cb(), source.buffer, destination, 1, &copy);
    vk_barrier_all(vk_cb());
}
static int lag_uniform_cover_fixture(uint64_t base, uint64_t alias, uint64_t producer,
                                     VkBuffer destination, uint8_t *result, int *checks) {
    int errors = 0;
#define SHARE_CHECK(condition, message) do { (*checks)++; if (!(condition)) { errors++; fprintf(stderr, "FAIL: lag sharing %s\n", message); } } while (0)
    uint32_t initial[4] = {1, 2, 3, 4}, overlay[4] = {13, 14, 15, 16}, native[4] = {31, 32, 33, 34};
    dm_guest_write_prepare((void *)(uintptr_t)base, 2048);
    memset((void *)(uintptr_t)base, 0, 2048);
    memcpy((void *)(uintptr_t)base, initial, 16);
    memcpy((void *)(uintptr_t)(base + 128), initial, 16);
    struct { op_hdr bind; p_bind_buf value; op_hdr end; } binding = {
        {OP_BIND_UBO, 8 + sizeof(p_bind_buf)}, {0, 0, base, 2048}, {OP_END, 8}};
    struct { op_hdr update; uint64_t address, size; uint32_t value[4]; op_hdr end; } update = {
        {OP_UPDATE_UB, 40}, base + 128, 16, {13, 14, 15, 16}, {OP_END, 8}};
    struct { op_hdr update; uint64_t address, size; uint32_t value, pad; op_hdr end; } partial = {
        {OP_UPDATE_UB, 32}, base + 132, 4, 99, 0, {OP_END, 8}};
    uint64_t handle = (uint64_t)(uintptr_t)&binding;
    VkDescriptorBufferInfo pristine, first, subset, second, same, aligned;
    vk_lag_test_stats before, after;
    mtl_sub_begin(); mtl_sub_snapshot_ubos_cap(&handle, 1, NULL);
    SHARE_CHECK(vk_lag_uniform_read(base, 256, 16, &pristine), "owned eager snapshot initially supplies the bound range");
    dm_guest_write_prepare((void *)(uintptr_t)base, 16); memcpy((void *)(uintptr_t)base, native, 16);
    mtl_exec((const uint8_t *)&update);
    vk_lag_test_stats_get(&before);
    SHARE_CHECK(vk_lag_uniform_read(base + 1024, 256, 16, &same), "unrelated subrange still binds its owned snapshot");
    vk_lag_test_stats_get(&after);
    SHARE_CHECK(after.kind[VK_LAG_UNIFORM].snapshot_allocations == before.kind[VK_LAG_UNIFORM].snapshot_allocations &&
        after.kind[VK_LAG_UNIFORM].uniform_snapshot_binds == before.kind[VK_LAG_UNIFORM].uniform_snapshot_binds + 1,
        "a broad snapshot.updated hint cannot force an unrelated subrange allocation");
    vk_lag_test_stats_get(&before);
    SHARE_CHECK(vk_lag_uniform_read(base, 256, 16, &first) &&
        vk_lag_uniform_read(base + 64, 192, 16, &subset) && subset.buffer == first.buffer &&
        subset.offset == first.offset + 64, "overlapping bound extents share an owned immutable covering window");
    vk_lag_test_stats_get(&after);
    SHARE_CHECK(after.kind[VK_LAG_UNIFORM].snapshot_allocations == before.kind[VK_LAG_UNIFORM].snapshot_allocations + 1 &&
        after.kind[VK_LAG_UNIFORM].snapshot_bytes - before.kind[VK_LAG_UNIFORM].snapshot_bytes <= 2 * 256 &&
        after.kind[VK_LAG_UNIFORM].uniform_cover_hits > before.kind[VK_LAG_UNIFORM].uniform_cover_hits,
        "cover allocation is bounded to twice the consumed span, not a mostly-unused 64 KiB unit");
    SHARE_CHECK(vk_lag_uniform_read(base + 128, 16, 16, &same), "small updated subrange reuses the covering version");
    capture_value(destination, 36, same);
    capture_value(destination, 37, first); /* old-frame native base, not the CPU rewrite */
    uint64_t queued = vk_submit_serial();
    vk_lag_test_stats_get(&before); vk_submit_chunk();
    SHARE_CHECK(vk_submit_serial() == queued + 1 &&
        vk_lag_uniform_read(base, 256, 16, &same) && same_descriptor(first, same) &&
        vk_lag_uniform_read(base + 64, 192, 16, &subset) && subset.buffer == first.buffer &&
        subset.offset == first.offset + 64,
        "owned exact and covering versions survive a genuine intermediate chunk in the same snapshot arena");
    vk_lag_test_stats_get(&after);
    SHARE_CHECK(after.kind[VK_LAG_UNIFORM].snapshot_allocations == before.kind[VK_LAG_UNIFORM].snapshot_allocations &&
        after.kind[VK_LAG_UNIFORM].snapshot_bytes == before.kind[VK_LAG_UNIFORM].snapshot_bytes,
        "a Vulkan chunk alone neither recaptures nor recopies immutable game-submission bytes");
    mtl_exec((const uint8_t *)&partial);
    SHARE_CHECK(vk_lag_uniform_read(base, 256, 16, &second) && !same_descriptor(first, second),
        "partial inline overwrite appends a new immutable semantic version");
    capture_value(destination, 38, (VkDescriptorBufferInfo){first.buffer, first.offset + 128, 16});
    capture_value(destination, 39, (VkDescriptorBufferInfo){second.buffer, second.offset + 128, 16});
    SHARE_CHECK(vk_lag_uniform_read(base + 128, 16, 256, &aligned) && !(aligned.offset % 256),
        "a later stricter alignment cannot borrow a misaligned covering subrange");
    update.address = base + 512; mtl_exec((const uint8_t *)&update);
    vk_lag_test_stats_get(&before);
    SHARE_CHECK(vk_lag_uniform_read(base, 256, 16, &same) && same_descriptor(second, same),
        "disjoint overlay preserves an exact binding's semantic content version");
    SHARE_CHECK(vk_lag_uniform_read(base + 64, 192, 16, &subset) && subset.buffer == second.buffer &&
        subset.offset == second.offset + 64, "disjoint overlay preserves unchanged subrange proof inside an older cover");
    vk_lag_test_stats_get(&after);
    SHARE_CHECK(after.kind[VK_LAG_UNIFORM].snapshot_allocations == before.kind[VK_LAG_UNIFORM].snapshot_allocations,
        "sharing never recopies a query solely because a different part of its window changed");
    mtl_sub_copy(producer, alias + 128, 16);
    SHARE_CHECK(!vk_lag_uniform_read(base, 256, 16, &same), "GPU-writable alias revokes the owned snapshot and every covering version");
    mtl_sub_copy(producer, alias + 128, 16);
    SHARE_CHECK(!vk_lag_uniform_read(base, 256, 16, &same), "repeated real alias producer cannot recover stale covering authority");
    SHARE_CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "overlay versions and alias producer submission completes");
    uint32_t combined[4] = {13, 99, 15, 16};
    SHARE_CHECK(!memcmp(result + 36 * 16, overlay, 16) && !memcmp(result + 37 * 16, initial, 16) &&
        !memcmp(result + 38 * 16, overlay, 16) && !memcmp(result + 39 * 16, combined, 16),
        "all partial logical versions and the immutable old-frame base retain their exact earlier-draw bytes");
    /* Four snapshot arenas recycle repeatedly. Reuse the same semantic addresses and cover geometry while
       changing the overlay every frame: an old descriptor or cached native pointer must never survive reset. */
    uint32_t frame_values[10][4];
    update.address = base + 128;
    vk_lag_test_stats_get(&before);
    for (unsigned frame = 0; frame < 10; frame++) {
        for (unsigned i = 0; i < 4; i++) update.value[i] = frame_values[frame][i] = 100 + frame * 4 + i;
        dm_guest_write_prepare((void *)(uintptr_t)base, 2048); memset((void *)(uintptr_t)base, 0, 2048);
        mtl_sub_begin(); mtl_sub_snapshot_ubos_cap(&handle, 1, NULL); mtl_exec((const uint8_t *)&update);
        SHARE_CHECK(vk_lag_uniform_read(base, 256, 16, &same), "fresh frame owns a fresh covering version after snapshot reset");
        capture_value(destination, 50 + frame, (VkDescriptorBufferInfo){same.buffer, same.offset + 128, 16});
        SHARE_CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "arena recycle frame completes");
    }
    vk_lag_test_stats_get(&after);
    SHARE_CHECK(after.kind[VK_LAG_UNIFORM].metadata_cross_snapshot > before.kind[VK_LAG_UNIFORM].metadata_cross_snapshot &&
        after.kind[VK_LAG_UNIFORM].copy_snapshot_misses > before.kind[VK_LAG_UNIFORM].copy_snapshot_misses,
        "cross-frame metadata hits remain separate from mandatory recycled-arena copy misses");
    for (unsigned frame = 0; frame < 10; frame++)
        SHARE_CHECK(!memcmp(result + (50 + frame) * 16, frame_values[frame], 16),
            "arena wrap never returns a prior frame's covering snapshot bytes");
#undef SHARE_CHECK
    return errors;
}

typedef struct {
    _Atomic int ready, resume;
    void *address; uint64_t donor, size;
} producer_remap;
static void *producer_remap_run(void *arg) {
    producer_remap *r = arg;
    while (!atomic_load_explicit(&r->ready, memory_order_acquire)) sched_yield();
    vk_record_test_alias(r->address, r->donor, r->size);
    atomic_store_explicit(&r->resume, 1, memory_order_release);
    return NULL;
}
static int lag_cache_fixture(int *checks) {
    if (!vk_record_lag_enabled() || !vk_lag_uniform_private()) {
        printf("lag cache: protected rename-OFF wait policy required; policy-specific cases not exercised\n");
        return 0;
    }
    enum { BYTES = 4 * 1024 * 1024, REPEATS = 32, CHUNKS = 10 };
    int errors = 0;
#define LAG_CHECK(condition, message) do { (*checks)++; if (!(condition)) { errors++; fprintf(stderr, "FAIL: lag cache %s\n", message); } } while (0)
    uint8_t *physical = mmap(NULL, BYTES, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    uint8_t *alias = mmap(NULL, BYTES, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (physical == MAP_FAILED || alias == MAP_FAILED) DIE("lag cache fixture mapping failed");
    mtl_pool_add(physical, BYTES); nvn_dirty_add_range(physical, BYTES);
    if (!mtl_pool_alias(alias, physical, BYTES) &&
        mremap(physical, 0, BYTES, MREMAP_MAYMOVE | MREMAP_FIXED, alias) != alias)
        DIE("lag cache fixture shared alias failed");
    /* Keep one backing VkBuffer, as Android K_VIRT aliases do. On POOLS_HOST, mtl_vpool_add would instead
       create an independent K_OWN import over these shared pages: its larger/tied span can win lookup over
       the synthetic donor alias, so it would not exercise canonical donor-buffer producers or partial remaps. */
    nvn_dirty_add_range(alias, BYTES);
    vk_record_test_alias(alias, (uint64_t)(uintptr_t)physical, BYTES);
    VkBuffer physical_buffer, alias_buffer; uint64_t physical_offset, alias_offset;
    LAG_CHECK(vk_resolve((uint64_t)(uintptr_t)physical, BYTES, &physical_buffer, &physical_offset) &&
        vk_resolve((uint64_t)(uintptr_t)alias, BYTES, &alias_buffer, &alias_offset) &&
        physical_buffer == alias_buffer && physical_offset == alias_offset,
        "real shared-page fixture alias resolves through the donor backing buffer");
    uintptr_t lo, hi;
    if (!nvn_dirty_lag_bounds(physical, 1, &lo, &hi) || hi - lo > BYTES / 4)
        DIE("lag cache fixture needs four dirty units within 4 MiB");
    uint64_t unit = hi - lo, address = (uint64_t)(uintptr_t)physical + 128;
    uint64_t cold = address + unit, unrelated = address + 2 * unit, producer = address + 3 * unit;
    uint32_t expected[7][4] = {
        {1, 2, 3, 4}, {13, 14, 15, 16}, {21, 22, 23, 24}, {31, 32, 33, 34},
        {41, 42, 43, 44}, {41, 42, 43, 44}, {51, 52, 53, 54}
    };
    memcpy((void *)(uintptr_t)address, expected[0], 16);
    memcpy((void *)(uintptr_t)cold, expected[2], 16);
    memcpy((void *)(uintptr_t)producer, expected[0], 16);
    VkBuffer destination; VkDeviceMemory memory; uint8_t *result;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 1024, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VKCHECK(vkCreateBuffer(V.dev, &bi, NULL, &destination));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, destination, &req);
    int type = vk_pick_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type < 0) DIE("lag cache fixture requires coherent readback memory");
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = (uint32_t)type};
    VKCHECK(vkAllocateMemory(V.dev, &ai, NULL, &memory)); VKCHECK(vkBindBufferMemory(V.dev, destination, memory, 0));
    VKCHECK(vkMapMemory(V.dev, memory, 0, 1024, 0, (void **)&result));
    struct { op_hdr update; uint64_t address, size; uint32_t value[4]; op_hdr end; } update = {
        {OP_UPDATE_UB, 40}, unrelated, 16, {13, 14, 15, 16}, {OP_END, 8}};
    VkDescriptorBufferInfo first, same, changed; uint32_t reason;
    uint64_t before[VK_LAG_TEST_N], after[VK_LAG_TEST_N];
    mtl_sub_begin();
    vk_record_test_lag_cache_stats(before, VK_LAG_TEST_N);
    vk_lag_uniform_prepare(address, 16);
    vk_record_test_lag_cache_stats(after, VK_LAG_TEST_N);
    LAG_CHECK(after[VK_LAG_TEST_PRODUCER_SCANS] == before[VK_LAG_TEST_PRODUCER_SCANS], "prepare with no updates does no producer scan");
    LAG_CHECK(vk_lag_uniform_read(address, 16, 16, &first), "initial private capture");
    capture_value(destination, 0, first);
    vk_record_test_lag_cache_stats(before, VK_LAG_TEST_N);
    for (int i = 0; i < REPEATS; i++)
        LAG_CHECK(vk_lag_uniform_read(address, 16, 16, &same) && same_descriptor(first, same), "unchanged descriptor reuses immutable capture");
    vk_record_test_lag_cache_stats(after, VK_LAG_TEST_N);
    LAG_CHECK(after[VK_LAG_TEST_UNIFORM_HITS] - before[VK_LAG_TEST_UNIFORM_HITS] == REPEATS &&
        after[VK_LAG_TEST_PRODUCER_SCANS] == before[VK_LAG_TEST_PRODUCER_SCANS], "warm private hits avoid producer/original scans");
    LAG_CHECK(!vk_lag_read_record(cold, 16, 16, &same, &reason, VK_LAG_VERTEX) && (reason & NVN_DIRTY_LAG_COLD), "cold descriptor remains guarded live memory");
    vk_record_test_lag_cache_stats(before, VK_LAG_TEST_N);
    for (int i = 0; i < REPEATS; i++)
        LAG_CHECK(!vk_lag_read_record(cold, 16, 16, &same, &reason, VK_LAG_VERTEX) && (reason & NVN_DIRTY_LAG_COLD), "unchanged cold selection retains registration reason");
    vk_record_test_lag_cache_stats(after, VK_LAG_TEST_N);
    LAG_CHECK(after[VK_LAG_TEST_COLD_HITS] - before[VK_LAG_TEST_COLD_HITS] == REPEATS &&
        after[VK_LAG_TEST_PRODUCER_SCANS] == before[VK_LAG_TEST_PRODUCER_SCANS], "warm cold hits avoid producer/original scans");
    mtl_exec((const uint8_t *)&update);
    vk_record_test_lag_cache_stats(before, VK_LAG_TEST_N);
    LAG_CHECK(vk_lag_uniform_read(address, 16, 16, &same) && same_descriptor(first, same), "unrelated inline update preserves private capture");
    LAG_CHECK(!vk_lag_read_record(cold, 16, 16, &same, &reason, VK_LAG_VERTEX) && !(reason & NVN_DIRTY_LAG_GPU), "unrelated update preserves negative producer classification");
    vk_record_test_lag_cache_stats(after, VK_LAG_TEST_N);
    LAG_CHECK(after[VK_LAG_TEST_PRODUCER_SCANS] == before[VK_LAG_TEST_PRODUCER_SCANS], "range journal preserves unrelated negative checks");
    vk_record_test_lag_cache_stats(before, VK_LAG_TEST_N);
    for (unsigned i = 0; i < 300; i++) mtl_exec((const uint8_t *)&update);
    LAG_CHECK(!vk_lag_read_record(cold, 16, 16, &same, &reason, VK_LAG_VERTEX) && !(reason & NVN_DIRTY_LAG_GPU),
        "logical-only journal overflow does not introduce an original-memory producer");
    vk_record_test_lag_cache_stats(after, VK_LAG_TEST_N);
    LAG_CHECK(after[VK_LAG_TEST_PRODUCER_SCANS] == before[VK_LAG_TEST_PRODUCER_SCANS],
        "producer-only negative survives hundreds of unrelated logical versions");
    update.address = address; mtl_exec((const uint8_t *)&update);
    LAG_CHECK(vk_lag_uniform_read(address, 16, 16, &changed) && !same_descriptor(first, changed), "overlapping inline update revokes capture");
    capture_value(destination, 1, changed);
    LAG_CHECK(vk_lag_uniform_read(address, 16, 256, &same) && !(same.offset % 256), "alignment is part of capture key");
    LAG_CHECK(vk_lag_uniform_read(address, 32, 16, &same) && same.range == 32, "bound extent is part of capture key");
    mtl_sub_copy(producer, address + 8, 8);
    LAG_CHECK(!vk_lag_uniform_read(address, 16, 16, &same), "true GPU producer revokes private capture");
    LAG_CHECK(!vk_lag_read_record(address, 16, 16, &same, &reason, VK_LAG_VERTEX) && (reason & NVN_DIRTY_LAG_GPU), "true GPU producer retains live classification");
    update.address = address + 64; mtl_exec((const uint8_t *)&update);
    LAG_CHECK(vk_ub_pending(address + 64, 16), "smaller writable neighbour leaves disjoint private overlay deferred");
    vk_lag_uniform_prepare(address, 256);
    LAG_CHECK(!vk_ub_pending(address + 64, 16) && !vk_lag_uniform_read(address, 256, 16, &same),
        "broad UBO prepare materializes overlay when smaller writable neighbour is live");
    LAG_CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "producer submission completes");

    /* No explicit submit snapshot here: these are mutable native bases. Both writer VAs are watched, so
       no alias VA generation change is needed for a physical writer to invalidate a virtual descriptor. */
    mtl_sub_begin();
    vk_record_test_lag_cache_stats(before, VK_LAG_TEST_N);
    LAG_CHECK(vk_lag_uniform_read(address, 16, 16, &same), "completed positive producer expires across the next actual submission");
    vk_record_test_lag_cache_stats(after, VK_LAG_TEST_N);
    LAG_CHECK(after[VK_LAG_TEST_PRODUCER_SCANS] > before[VK_LAG_TEST_PRODUCER_SCANS],
        "completion invalidates a positive even without a new producer revision");
    LAG_CHECK(vk_lag_uniform_read(cold, 16, 16, &first), "standalone native private selection");
    capture_value(destination, 2, first);
    uint64_t dirty = nvn_dirty_now();
    dm_guest_write_prepare((void *)(uintptr_t)cold, 16); memcpy((void *)(uintptr_t)cold, expected[3], 16);
    LAG_CHECK(nvn_dirty_now() != dirty && vk_lag_uniform_read(cold, 16, 16, &changed) &&
        !same_descriptor(first, changed), "physical CPU writer invalidates standalone private capture");
    capture_value(destination, 3, changed);
    uint64_t virtual_address = (uint64_t)(uintptr_t)alias + unit + 128;
    dirty = nvn_dirty_now();
    dm_guest_write_prepare((void *)(uintptr_t)virtual_address, 16); memcpy((void *)(uintptr_t)virtual_address, expected[4], 16);
    LAG_CHECK(nvn_dirty_now() != dirty && vk_lag_uniform_read(cold, 16, 16, &same) &&
        !same_descriptor(changed, same), "virtual CPU writer invalidates physical private capture");
    capture_value(destination, 4, same);
    LAG_CHECK(vk_lag_uniform_read(virtual_address, 16, 16, &first), "virtual descriptor captures shared physical bytes");
    capture_value(destination, 5, first);
    dm_guest_write_prepare((void *)(uintptr_t)cold, 16); memcpy((void *)(uintptr_t)cold, expected[6], 16);
    LAG_CHECK(vk_lag_uniform_read(virtual_address, 16, 16, &changed) && !same_descriptor(first, changed),
        "physical CPU writer invalidates unchanged virtual binding");
    capture_value(destination, 6, changed);

    uint64_t clean = unrelated + 1024, ticket = vk_record_lag_ticket();
    LAG_CHECK(vk_lag_uniform_read(clean, 16, 16, &first), "pre-chunk immutable selection");
    for (int i = 0; i < CHUNKS; i++) {
        capture_value(destination, 16 + (unsigned)i, first); /* each chunk must contain real work, including ring wrap */
        vk_submit_chunk();
        LAG_CHECK(vk_record_lag_ticket() > ticket && vk_lag_uniform_read(clean, 16, 16, &changed) &&
            !same_descriptor(first, changed), "actual ticket/slot reuse cannot recycle selection memo");
        ticket = vk_record_lag_ticket(); first = changed;
    }
    nvn_dirty_remove_range(alias);
    uint64_t unwatched = vk_lag_copy_unwatched();
    LAG_CHECK(vk_lag_uniform_read(virtual_address, 16, 16, &first) &&
        vk_lag_uniform_read(virtual_address, 16, 16, &changed) && !same_descriptor(first, changed) &&
        vk_lag_copy_unwatched() == unwatched + 2, "removed writer incarnation forbids copy and outer-stage memo reuse");
    nvn_dirty_add_range(alias, BYTES);
    LAG_CHECK(vk_lag_uniform_read(virtual_address, 16, 16, &first) &&
        vk_lag_uniform_read(virtual_address, 16, 16, &same) && same_descriptor(first, same),
        "recycled dirty slot establishes new watched authority");
    vk_record_test_alias(alias, (uint64_t)(uintptr_t)physical, BYTES);
    LAG_CHECK(vk_lag_uniform_read(virtual_address, 16, 16, &changed) && !same_descriptor(first, changed),
        "live span generation revokes canonical binding memo");
    LAG_CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "CPU rewrite and slot reuse submission completes");
    uint32_t zero[4] = {0};
    for (unsigned i = 0; i < CHUNKS; i++)
        LAG_CHECK(!memcmp(result + (16 + i) * 16, zero, 16), "submission arena captures survive actual ring-slot reuse");

    /* In contrast, an explicit submission snapshot owns the old frame's base. Native CPU reuse, including an
       unrelated write in the same dirty unit, cannot revoke that authority or the later CPU-known overlay. */
    struct { op_hdr bind; p_bind_buf value; op_hdr end; } bindings = {
        {OP_BIND_UBO, 8 + sizeof(p_bind_buf)}, {0, 0, cold, 16}, {OP_END, 8}};
    uint64_t handle = (uint64_t)(uintptr_t)&bindings;
    mtl_sub_begin(); mtl_sub_snapshot_ubos_cap(&handle, 1, NULL);
    LAG_CHECK(vk_lag_uniform_read(cold, 16, 16, &first), "explicit snapshot private selection");
    capture_value(destination, 7, first);
    dm_guest_write_prepare((void *)(uintptr_t)cold, 16); memcpy((void *)(uintptr_t)cold, expected[3], 16);
    LAG_CHECK(vk_lag_uniform_read(cold, 16, 16, &same) && same_descriptor(first, same),
        "explicit snapshot authority survives CPU native-base rewrite");
    capture_value(destination, 8, same);
    update.address = cold; memcpy(update.value, expected[0], 16); mtl_exec((const uint8_t *)&update);
    dm_guest_write_prepare((void *)(uintptr_t)(virtual_address + 64), 4);
    memset((void *)(uintptr_t)(virtual_address + 64), 0x5a, 4);
    uint32_t logical[4]; uint64_t pointer_authority = vk_lag_copy_unwatched();
    vk_ub_read_record(cold, logical, sizeof logical);
    LAG_CHECK(!memcmp(logical, expected[0], 16) && vk_lag_uniform_read(cold, 16, 16, &changed) &&
        vk_lag_copy_unwatched() == pointer_authority, "snapshot overlay remains stable recorder-parent authority after same-unit CPU write");
    capture_value(destination, 9, changed);
    LAG_CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "explicit snapshot authority submission completes");
    LAG_CHECK(!memcmp(result + 7 * 16, expected[6], 16) && !memcmp(result + 8 * 16, expected[6], 16) &&
        !memcmp(result + 9 * 16, expected[0], 16), "snapshot old-frame bytes and newer logical overlay remain distinct from CPU reuse");
    for (unsigned i = 0; i < 7; i++)
        LAG_CHECK(!memcmp(result + i * 16, expected[i], 16), "queued immutable capture contains exact pre/post rewrite bytes");
    uint64_t cover_base = (unrelated + 16384 + 1023) & ~UINT64_C(1023);
    errors += lag_uniform_cover_fixture(cover_base, (uint64_t)(uintptr_t)alias + cover_base - (uint64_t)(uintptr_t)physical,
        producer, destination, result, checks);
    const char *hybrid = getenv("TOTK_VK_LAG_HYBRID");
    if (!(hybrid && !atoi(hybrid))) {
        uint64_t hot = unrelated + 4096, hot_alias = (uint64_t)(uintptr_t)alias + 2 * unit + 128 + 4096;
        for (int frame = 64; frame <= 65; frame++) {
            nvn_dirty_lag_frame((uint64_t)frame); nvn_dirty_lag_watch((void *)(uintptr_t)hot, 16);
            dm_guest_write_prepare((void *)(uintptr_t)hot, 16); memcpy((void *)(uintptr_t)hot, expected[1], 16);
        }
        LAG_CHECK(nvn_dirty_lag_hot((void *)(uintptr_t)hot, 16), "native read range becomes genuinely hot in two distinct frames");
        mtl_sub_begin();
        LAG_CHECK(vk_lag_read_record(hot, 16, 16, &first, &reason, VK_LAG_VERTEX) && !reason, "hot readonly selection captures immutable native bytes");
        capture_value(destination, 10, first);
        vk_record_test_lag_cache_stats(before, VK_LAG_TEST_N);
        for (int i = 0; i < REPEATS; i++)
            LAG_CHECK(vk_lag_read_record(hot, 16, 16, &same, &reason, VK_LAG_VERTEX) && same_descriptor(first, same) && !reason,
                "unchanged hot readonly selection reuses capture");
        vk_record_test_lag_cache_stats(after, VK_LAG_TEST_N);
        LAG_CHECK(after[VK_LAG_TEST_COPY_HITS] - before[VK_LAG_TEST_COPY_HITS] == REPEATS &&
            after[VK_LAG_TEST_PRODUCER_SCANS] == before[VK_LAG_TEST_PRODUCER_SCANS], "hot copy hits avoid producer/original scans");
        dm_guest_write_prepare((void *)(uintptr_t)hot, 16); memcpy((void *)(uintptr_t)hot, expected[2], 16);
        LAG_CHECK(vk_lag_read_record(hot, 16, 16, &changed, &reason, VK_LAG_VERTEX) && !same_descriptor(first, changed),
            "physical CPU writer revokes hot readonly capture");
        capture_value(destination, 11, changed);
        dm_guest_write_prepare((void *)(uintptr_t)hot_alias, 16); memcpy((void *)(uintptr_t)hot_alias, expected[3], 16);
        LAG_CHECK(vk_lag_read_record(hot, 16, 16, &same, &reason, VK_LAG_VERTEX) && !same_descriptor(changed, same),
            "virtual CPU writer revokes hot readonly capture");
        capture_value(destination, 12, same);
        VkDescriptorBufferInfo adjacent, adjacent_changed;
        vk_lag_test_stats unit_before, unit_after;
        vk_lag_test_stats_get(&unit_before);
        LAG_CHECK(vk_lag_read_record(hot + 32, 24, 16, &adjacent, &reason, VK_LAG_INDEX) && !reason,
            "different index offset/size in learned-hot unit captures fresh bytes");
        capture_value(destination, 32, adjacent);
        dm_guest_write_prepare((void *)(uintptr_t)(hot_alias + 32), 16);
        memcpy((void *)(uintptr_t)(hot_alias + 32), expected[0], 16);
        LAG_CHECK(vk_lag_read_record(hot + 32, 24, 16, &adjacent_changed, &reason, VK_LAG_INDEX) &&
            !same_descriptor(adjacent, adjacent_changed), "alias write revokes exact native index bytes after contained-unit classification reuse");
        capture_value(destination, 33, adjacent_changed);
        vk_lag_test_stats_get(&unit_after);
        LAG_CHECK(unit_after.kind[VK_LAG_INDEX].decisions - unit_before.kind[VK_LAG_INDEX].decisions == 2 &&
            unit_after.kind[VK_LAG_INDEX].hot_copies - unit_before.kind[VK_LAG_INDEX].hot_copies == 2 &&
            unit_after.kind[VK_LAG_INDEX].hot_bytes - unit_before.kind[VK_LAG_INDEX].hot_bytes == 48 &&
            unit_after.kind[VK_LAG_INDEX].snapshot_allocations - unit_before.kind[VK_LAG_INDEX].snapshot_allocations == 2,
            "index hot-copy and snapshot allocation counters count actual distinct captures, not decisions alone");
        nvn_dirty_remove_range(alias);
        uint64_t unit_unwatched = vk_lag_copy_unwatched();
        LAG_CHECK(vk_lag_read_record(hot + 64, 16, 16, &adjacent, &reason, VK_LAG_INDEX) &&
            vk_lag_read_record(hot + 64, 16, 16, &adjacent_changed, &reason, VK_LAG_INDEX) &&
            !same_descriptor(adjacent, adjacent_changed) && vk_lag_copy_unwatched() == unit_unwatched + 2,
            "untracked writer alias forbids unit-projected watch and exact hot-copy authority");
        nvn_dirty_add_range(alias, BYTES);
        LAG_CHECK(vk_lag_read_record(hot + 64, 16, 16, &adjacent, &reason, VK_LAG_INDEX) &&
            vk_lag_read_record(hot + 64, 16, 16, &same, &reason, VK_LAG_INDEX) && same_descriptor(adjacent, same),
            "re-added writer alias requires fresh complete watch before hot-copy reuse");
        vk_record_test_alias(alias, (uint64_t)(uintptr_t)physical, BYTES);
        LAG_CHECK(vk_lag_read_record(hot + 64, 16, 16, &adjacent_changed, &reason, VK_LAG_INDEX) &&
            !same_descriptor(adjacent, adjacent_changed), "span change revokes cached projection and native hot-copy selection");
        LAG_CHECK(vk_resolve(hot_alias + 32, 24, &alias_buffer, &alias_offset) &&
            vk_resolve(hot + 32, 24, &physical_buffer, &physical_offset) &&
            alias_buffer == physical_buffer && alias_offset == physical_offset,
            "virtual hot producer and physical capture name the same backing-buffer extent");
        mtl_sub_copy(producer, hot_alias + 32, 16);
        LAG_CHECK(!vk_lag_read_record(hot + 32, 24, 16, &same, &reason, VK_LAG_INDEX) && (reason & NVN_DIRTY_LAG_GPU),
            "virtual GPU producer invalidates physical contained-range capture before reuse");
        uint64_t repeated_revision = vk_record_uniform_revision();
        mtl_sub_copy(producer, hot_alias + 32, 16);
        LAG_CHECK(!vk_record_uniform_unchanged(repeated_revision, hot + 32, 24) &&
            !vk_lag_read_record(hot + 32, 24, 16, &same, &reason, VK_LAG_INDEX) && (reason & NVN_DIRTY_LAG_GPU),
            "repeated alias GPU producer invalidates content even after producer deduplication");
        mtl_sub_copy(producer, hot, 16);
        LAG_CHECK(!vk_lag_read_record(hot, 16, 16, &same, &reason, VK_LAG_VERTEX) && (reason & NVN_DIRTY_LAG_GPU),
            "new true GPU producer revokes hot-copy classification before memo hit");
        LAG_CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "hot readonly capture submission completes");
        LAG_CHECK(!memcmp(result + 10 * 16, expected[1], 16) && !memcmp(result + 11 * 16, expected[2], 16) &&
            !memcmp(result + 12 * 16, expected[3], 16), "hot readonly captures retain exact physical/virtual pre/post bytes");
        LAG_CHECK(!memcmp(result + 32 * 16, zero, 16) && !memcmp(result + 33 * 16, expected[0], 16),
            "distinct contained hot ranges retain pre/post alias-write bytes");
        mtl_sub_begin();
        vk_record_test_lag_cache_stats(before, VK_LAG_TEST_N);
        LAG_CHECK(vk_lag_read_record(hot, 16, 16, &first, &reason, VK_LAG_VERTEX) && !reason,
            "completed GPU producer returns a still-hot unit to fresh native capture");
        vk_record_test_lag_cache_stats(after, VK_LAG_TEST_N);
        LAG_CHECK(after[VK_LAG_TEST_PRODUCER_SCANS] > before[VK_LAG_TEST_PRODUCER_SCANS],
            "hot positive producer memo also expires with GPU completion");
        uint64_t history_frame = nvn_dirty_lag_frame_now() + 8;
        vk_lag_test_stats_get(&unit_before);
        nvn_dirty_lag_frame(history_frame);
        LAG_CHECK(!vk_lag_read_record(hot, 16, 16, &same, &reason, VK_LAG_VERTEX) &&
            (reason & NVN_DIRTY_LAG_COLD), "same exact metadata key demotes after eight real quiet frames");
        for (unsigned i = 1; i <= 2; i++) {
            nvn_dirty_lag_frame(history_frame + i);
            nvn_dirty_lag_watch((void *)(uintptr_t)hot, 16);
            dm_guest_write_prepare((void *)(uintptr_t)hot, 16); memcpy((void *)(uintptr_t)hot, expected[1], 16);
            int captured = vk_lag_read_record(hot, 16, 16, &changed, &reason, VK_LAG_VERTEX);
            LAG_CHECK(captured == (i == 2), "same exact cold key refreshes CPU history and promotes only on the second writing frame");
        }
        vk_lag_test_stats_get(&unit_after);
        LAG_CHECK(unit_after.kind[VK_LAG_VERTEX].hot_refreshes - unit_before.kind[VK_LAG_VERTEX].hot_refreshes >= 3,
            "real-frame demotion and CPU-history promotion are observable independently of queue tickets");
        LAG_CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "hot-history refresh submission completes");
    }
    /* A tracked child does not authenticate the bytes that selected it. An untracked stack parent may change
       without any dirty transition, so each read must independently prevent the enclosing stage-freeze hit. */
    uint64_t child0 = unrelated + 8192, child1 = child0 + 64, parent = child0, selected;
    uint64_t tracked_parent = cold + 1024, pointer_source = producer + 8192;
    dm_guest_write_prepare((void *)(uintptr_t)child0, 80);
    memcpy((void *)(uintptr_t)child0, expected[0], 16); memcpy((void *)(uintptr_t)child1, expected[1], 16);
    dm_guest_write_prepare((void *)(uintptr_t)tracked_parent, 16); memcpy((void *)(uintptr_t)tracked_parent, &child0, 8);
    dm_guest_write_prepare((void *)(uintptr_t)pointer_source, 8); memcpy((void *)(uintptr_t)pointer_source, &child1, 8);
    mtl_sub_begin();
    LAG_CHECK(!nvn_dirty_tracked(&parent, sizeof parent), "pointer-parent probe is genuinely untracked");
    uint64_t unsafe_parent = vk_lag_copy_unwatched();
    vk_ub_read_record((uint64_t)(uintptr_t)&parent, &selected, sizeof selected);
    LAG_CHECK(selected == child0 && vk_lag_uniform_read(selected, 16, 16, &first) &&
        vk_lag_copy_unwatched() == unsafe_parent + 1, "untracked parent rejects outer freeze even with fully tracked child");
    capture_value(destination, 13, first);
    dirty = nvn_dirty_now(); parent = child1;
    vk_ub_read_record((uint64_t)(uintptr_t)&parent, &selected, sizeof selected);
    LAG_CHECK(nvn_dirty_now() == dirty && selected == child1 && vk_lag_uniform_read(selected, 16, 16, &changed) &&
        vk_lag_copy_unwatched() == unsafe_parent + 2, "same binding re-reads untracked parent rewrite without a fake dirty-generation change");
    capture_value(destination, 14, changed);
    unsafe_parent = vk_lag_copy_unwatched();
    vk_ub_read_record(tracked_parent, &selected, sizeof selected);
    LAG_CHECK(selected == child0 && vk_lag_copy_unwatched() == unsafe_parent, "watched tracked native parent can retain freeze authority");
    update.address = tracked_parent; memcpy(update.value, &child1, 8); update.value[2] = update.value[3] = 0;
    mtl_exec((const uint8_t *)&update);
    vk_ub_read_record(tracked_parent, &selected, sizeof selected);
    LAG_CHECK(selected == child1 && vk_lag_copy_unwatched() == unsafe_parent, "complete CPU-known log coverage authenticates parent bytes");
    dm_guest_write_prepare((void *)(uintptr_t)tracked_parent, 8); memcpy((void *)(uintptr_t)tracked_parent, &child0, 8);
    vk_ub_read_record(tracked_parent, &selected, sizeof selected);
    LAG_CHECK(selected == child1 && vk_lag_copy_unwatched() == unsafe_parent, "native CPU dirty transition cannot discard logical parent overlay authority");
    mtl_sub_copy(pointer_source, tracked_parent, 8);
    vk_ub_read_record(tracked_parent, &selected, sizeof selected);
    LAG_CHECK(vk_lag_copy_unwatched() == unsafe_parent + 1, "true GPU-native parent cannot be certified by a tracked child");
    LAG_CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "recorder parent authority submission completes");
    LAG_CHECK(!memcmp(result + 13 * 16, expected[0], 16) && !memcmp(result + 14 * 16, expected[1], 16),
        "untracked parent rewrite selects distinct exact tracked-child captures");
    /* Deliberately shift tracking relative to the physical pool and to global 64 KiB boundaries. Reuse must
       follow the real range-relative unit, including clipped tails, not an address mask or a previous VA birth. */
    nvn_dirty_remove_range(alias); nvn_dirty_remove_range(physical);
    size_t page = (size_t)getpagesize(), shift = page;
    if (!(((uintptr_t)physical + shift) & 65535)) shift += page;
    size_t tracked_bytes = BYTES - shift - page;
    uint64_t tracked = (uint64_t)(uintptr_t)physical + shift, tracked_alias = (uint64_t)(uintptr_t)alias + shift;
    nvn_dirty_add_range((void *)(uintptr_t)tracked, tracked_bytes);
    nvn_dirty_add_range((void *)(uintptr_t)tracked_alias, tracked_bytes);
    LAG_CHECK((tracked & 65535) && nvn_dirty_lag_bounds((void *)(uintptr_t)tracked, 1, &lo, &hi) &&
        lo == tracked && hi - lo == unit, "dirty-unit identity starts at shifted tracked range, not global 64 KiB boundary");
    uint64_t small = tracked + 128, untouched = small + 2 * unit, producer_alias = tracked_alias + 512;
    uint64_t unit_producer = tracked + 3 * unit + 128;
    vk_lag_test_stats counts_before, counts_after;
    mtl_sub_begin();
    vk_lag_test_stats_get(&counts_before);
    vk_record_test_lag_cache_stats(before, VK_LAG_TEST_N);
    for (unsigned i = 0; i < 16; i++)
        LAG_CHECK(!vk_lag_read_record(small + i * 32, 16 + (i % 4) * 4, 16, &same, &reason, VK_LAG_VERTEX) &&
            (reason & NVN_DIRTY_LAG_COLD) && !(reason & NVN_DIRTY_LAG_GPU), "unique small offsets/sizes in one real unit stay cold and guarded");
    vk_record_test_lag_cache_stats(after, VK_LAG_TEST_N);
    vk_lag_test_stats_get(&counts_after);
    LAG_CHECK(counts_after.kind[VK_LAG_VERTEX].decisions - counts_before.kind[VK_LAG_VERTEX].decisions == 16 &&
        counts_after.kind[VK_LAG_VERTEX].binding_misses - counts_before.kind[VK_LAG_VERTEX].binding_misses == 16 &&
        counts_after.kind[VK_LAG_VERTEX].cold_guards - counts_before.kind[VK_LAG_VERTEX].cold_guards == 16 &&
        counts_after.kind[VK_LAG_VERTEX].snapshot_allocations == counts_before.kind[VK_LAG_VERTEX].snapshot_allocations,
        "cold unique-range counters expose exact misses without counting fake snapshot allocations");
    LAG_CHECK(vk_off("span") || (counts_after.kind[VK_LAG_VERTEX].class_hits - counts_before.kind[VK_LAG_VERTEX].class_hits == 15 &&
        after[VK_LAG_TEST_PRODUCER_SCANS] - before[VK_LAG_TEST_PRODUCER_SCANS] == 1),
        "contained unit decisions reuse mapping/tracking/hot state and one whole-unit negative producer scan");
    LAG_CHECK(counts_after.kind[VK_LAG_VERTEX].sample_count - counts_before.kind[VK_LAG_VERTEX].sample_count ==
        (counts_after.kind[VK_LAG_VERTEX].decisions + 63) / 64 - (counts_before.kind[VK_LAG_VERTEX].decisions + 63) / 64,
        "decision clocks are sampled once per 64 per-kind decisions");
    uint64_t unit_ticket = vk_record_lag_ticket();
    LAG_CHECK(!nvn_dirty_lag_registered((void *)(uintptr_t)small, 16, unit_ticket),
        "unit classification and producer lookup never arm a future ticket");
    /* Same-sized streams separated by exactly 64 KiB used to collide systematically in the exact binding hash. */
    vk_lag_test_stats_get(&counts_before);
    for (unsigned round = 0; round < 2; round++) for (unsigned lane = 0; lane < 8; lane++)
        LAG_CHECK(!vk_lag_read_record(tracked + 1024 + (uint64_t)lane * 65536, 20, 16, &same, &reason, VK_LAG_INDEX) &&
            (reason & NVN_DIRTY_LAG_READONLY), "same-size 64 KiB-separated index lanes retain live guards");
    vk_lag_test_stats_get(&counts_after);
    LAG_CHECK(counts_after.kind[VK_LAG_INDEX].binding_hits - counts_before.kind[VK_LAG_INDEX].binding_hits >= 6,
        "well-folded exact keys retain most independently interleaved 64 KiB lanes");
    LAG_CHECK(vk_resolve(producer_alias, 16, &alias_buffer, &alias_offset) &&
        vk_resolve(tracked + 512, 16, &physical_buffer, &physical_offset) &&
        alias_buffer == physical_buffer && alias_offset == physical_offset,
        "virtual whole-unit producer and physical query name the same backing-buffer extent");
    mtl_sub_copy(unit_producer, producer_alias, 16);
    LAG_CHECK(!vk_lag_read_record(tracked + 512, 16, 16, &same, &reason, VK_LAG_VERTEX) && (reason & NVN_DIRTY_LAG_GPU),
        "producer outside first exact range but inside cached unit invalidates whole-unit negative classification");
    uint64_t unit_revision = vk_record_uniform_revision();
    vk_gpu_write_record(producer_alias, 16);
    LAG_CHECK(!vk_record_uniform_unchanged(unit_revision, tracked + 512, 16) &&
        !vk_lag_read_record(tracked + 512, 16, 16, &same, &reason, VK_LAG_VERTEX) && (reason & NVN_DIRTY_LAG_GPU),
        "repeated GPU producer cannot hide behind contained-unit classification or writer deduplication");
    LAG_CHECK(!vk_lag_read_record(untouched, 16, 16, &same, &reason, VK_LAG_INDEX) && !(reason & NVN_DIRTY_LAG_GPU),
        "separate real unit establishes an independent producer negative");
    vk_record_test_lag_cache_stats(before, VK_LAG_TEST_N);
    for (unsigned i = 0; i < 4100; i++) vk_gpu_write_record(unit_producer, 16);
    LAG_CHECK(!vk_lag_read_record(untouched, 16, 16, &same, &reason, VK_LAG_INDEX) && !(reason & NVN_DIRTY_LAG_GPU),
        "producer journal wrap preserves safety through a new whole-unit scan");
    vk_record_test_lag_cache_stats(after, VK_LAG_TEST_N);
    LAG_CHECK(after[VK_LAG_TEST_PRODUCER_SCANS] > before[VK_LAG_TEST_PRODUCER_SCANS],
        "overwritten producer journal cannot certify an old unit negative");
    /* A partial virtual remap changes the physical producer identity only inside its winning span. */
    uint64_t piece = tracked_alias + 2 * unit + 1024;
    LAG_CHECK(!vk_lag_read_record(piece, 16, 16, &same, &reason, VK_LAG_INDEX) && !(reason & NVN_DIRTY_LAG_GPU),
        "initial virtual piece resolves to untouched physical unit");
    vk_record_test_alias((void *)(uintptr_t)piece, unit_producer, 64);
    LAG_CHECK(vk_resolve(piece, 64, &alias_buffer, &alias_offset) &&
        vk_resolve(unit_producer, 64, &physical_buffer, &physical_offset) &&
        alias_buffer == physical_buffer && alias_offset == physical_offset,
        "partial alias remap actually selects the new producer's donor-buffer extent");
    LAG_CHECK(!vk_lag_read_record(piece, 16, 16, &same, &reason, VK_LAG_INDEX) && (reason & NVN_DIRTY_LAG_GPU),
        "partial virtual remap revokes mapping cache and resolves new physical producer");
    LAG_CHECK(!vk_lag_read_record(piece + 64, 16, 16, &same, &reason, VK_LAG_INDEX) && !(reason & NVN_DIRTY_LAG_GPU),
        "winning-span boundary cannot extend producer classification into adjacent virtual mapping");
    vk_record_test_alias(alias, (uint64_t)(uintptr_t)physical, BYTES);
    /* The producer seen-cache must not confuse a repeated virtual VA with its old donor. Use fresh
       units so a producer already indexed elsewhere cannot make a missing second insertion pass. */
    uint64_t remapped = tracked_alias + 6 * unit + 1024;
    uint64_t donor_a = tracked + 6 * unit + 128, donor_b = tracked + 7 * unit + 128;
    vk_record_test_alias((void *)(uintptr_t)remapped, donor_a, 64);
    uint64_t remap_serial = vk_submit_serial();
    mtl_sub_copy(unit_producer, remapped, 16);
    LAG_CHECK(!vk_lag_read_record(donor_a, 16, 16, &same, &reason, VK_LAG_INDEX) &&
        (reason & NVN_DIRTY_LAG_GPU), "first virtual write indexes donor A");
    LAG_CHECK(!vk_lag_read_record(donor_b, 16, 16, &same, &reason, VK_LAG_INDEX) &&
        !(reason & NVN_DIRTY_LAG_GPU), "donor B has no producer before the remapped write");
    vk_record_test_alias((void *)(uintptr_t)remapped, donor_b, 64);
    LAG_CHECK(vk_resolve(remapped, 16, &alias_buffer, &alias_offset) &&
        vk_resolve(donor_b, 16, &physical_buffer, &physical_offset) &&
        alias_buffer == physical_buffer && alias_offset == physical_offset,
        "same virtual address now resolves to donor B");
    mtl_sub_copy(unit_producer, remapped, 16);
    LAG_CHECK(vk_submit_serial() == remap_serial, "both virtual writes and donor remap retain one seal serial");
    LAG_CHECK(!vk_lag_read_record(donor_b, 16, 16, &same, &reason, VK_LAG_INDEX) &&
        (reason & NVN_DIRTY_LAG_GPU), "same-serial remapped virtual write indexes donor B");
    LAG_CHECK(!vk_lag_read_record(donor_a, 16, 16, &same, &reason, VK_LAG_INDEX) &&
        (reason & NVN_DIRTY_LAG_GPU), "remapped virtual write does not discard donor A's producer");
    /* A remap on another thread can finish after the writer selected donor A but before it records
       the current generation. The following write must still index B even with that same generation. */
    uint64_t raced = tracked_alias + 9 * unit + 1024;
    uint64_t race_a = tracked + 9 * unit + 128, race_b = tracked + 10 * unit + 128;
    vk_record_test_alias((void *)(uintptr_t)raced, race_a, 64);
    vk_gpu_write_record(raced, 16);
    producer_remap race = {.address = (void *)(uintptr_t)raced, .donor = race_b, .size = 64};
    pthread_t remapper;
    vk_record_test_gpu_write_pause(&race.ready, &race.resume);
    if (pthread_create(&remapper, NULL, producer_remap_run, &race)) DIE("producer remapper thread failed");
    vk_gpu_write_record(raced, 16);
    pthread_join(remapper, NULL); vk_record_test_gpu_write_pause(NULL, NULL);
    vk_gpu_write_record(raced, 16);
    LAG_CHECK(vk_submit_serial() == remap_serial &&
        !vk_lag_read_record(race_b, 16, 16, &same, &reason, VK_LAG_INDEX) &&
        (reason & NVN_DIRTY_LAG_GPU), "concurrent canonical-donor/generation handoff cannot hide donor B");
    /* guarded=true describes a frozen alias inventory, not just a donor. Keep the donor unchanged,
       concurrently join one more alias, and require a fresh guard intent without disabling stable dedup. */
    vk_gpu_write(remapped, 16);
    uint64_t guards = vk_record_test_gpu_write_guards();
    vk_gpu_write(remapped, 16);
    LAG_CHECK(vk_record_test_gpu_write_guards() == guards, "unchanged mapping retains guarded duplicate suppression");
    producer_remap join = {.address = (void *)(uintptr_t)(tracked_alias + 11 * unit + 1024),
                          .donor = donor_b, .size = 64};
    vk_record_test_gpu_write_pause(&join.ready, &join.resume);
    if (pthread_create(&remapper, NULL, producer_remap_run, &join)) DIE("guard alias remapper thread failed");
    vk_gpu_write(remapped, 16);
    pthread_join(remapper, NULL); vk_record_test_gpu_write_pause(NULL, NULL);
    LAG_CHECK(vk_record_test_gpu_write_guards() == guards + 1,
        "concurrent alias join revokes guarded state and forwards a fresh protection intent");
    vk_gpu_write(remapped, 16);
    LAG_CHECK(vk_record_test_gpu_write_guards() == guards + 1,
        "refreshed alias inventory permits guarded duplicate suppression again");
    vk_record_test_alias(alias, (uint64_t)(uintptr_t)physical, BYTES);
    LAG_CHECK(!vk_lag_read_record(tracked + unit - 8, 16, 16, &same, &reason, VK_LAG_INDEX) &&
        (reason & NVN_DIRTY_LAG_READONLY), "range crossing real unit boundary takes complete conservative path");
    uint64_t window_boundary = (tracked + 65535) & ~UINT64_C(65535);
    LAG_CHECK(!vk_lag_read_record(window_boundary - 8, 16, 16, &same, &reason, VK_LAG_INDEX) &&
        (reason & NVN_DIRTY_LAG_READONLY), "fixed mapping-cache window boundary cannot truncate a live input range");
    uint64_t tail = tracked + tracked_bytes, boundary_unwatched = vk_lag_copy_unwatched();
    LAG_CHECK(!vk_lag_read_record(tail - 16, 16, 16, &same, &reason, VK_LAG_INDEX) && (reason & NVN_DIRTY_LAG_COLD),
        "clipped final dirty unit accepts its exact tracked tail");
    LAG_CHECK(vk_lag_read_record(tail - 8, 16, 16, &same, &reason, VK_LAG_INDEX) && !reason &&
        vk_lag_copy_unwatched() == boundary_unwatched + 1, "partly untracked tail cannot borrow a contained unit's tracking/watch authority");
    /* Prime both exact metadata and its containing unit AFTER all mapping churn. Neither a real queue ticket
       nor a new game submission may discard this geometry or introduce a producer into the untouched unit. */
    LAG_CHECK(!vk_lag_read_record(untouched, 16, 16, &same, &reason, VK_LAG_INDEX) && !(reason & NVN_DIRTY_LAG_GPU),
        "stable mapping primes cross-ticket exact and whole-unit metadata");
    vk_lag_test_stats_get(&counts_before);
    vk_record_test_lag_cache_stats(before, VK_LAG_TEST_N);
    vkCmdFillBuffer(vk_cb(), destination, 48 * 16, 16, 0);
    vk_submit_chunk();
    LAG_CHECK(vk_record_lag_ticket() > unit_ticket &&
        !vk_lag_read_record(untouched, 16, 16, &same, &reason, VK_LAG_INDEX) && !(reason & NVN_DIRTY_LAG_GPU),
        "new actual ticket retains contained live metadata but still selects the current guard ticket");
    LAG_CHECK(!vk_lag_read_record(untouched + 32, 24, 16, &same, &reason, VK_LAG_INDEX) && !(reason & NVN_DIRTY_LAG_GPU),
        "new extent reuses a prior-ticket whole-unit negative without borrowing byte authority");
    vk_lag_test_stats_get(&counts_after);
    vk_record_test_lag_cache_stats(after, VK_LAG_TEST_N);
    LAG_CHECK(counts_after.kind[VK_LAG_INDEX].key_ticket > counts_before.kind[VK_LAG_INDEX].key_ticket &&
        counts_after.kind[VK_LAG_INDEX].metadata_cross_ticket > counts_before.kind[VK_LAG_INDEX].metadata_cross_ticket &&
        (vk_off("span") || counts_after.kind[VK_LAG_INDEX].unit_cross_ticket > counts_before.kind[VK_LAG_INDEX].unit_cross_ticket),
        "cross-ticket metadata hits and byte-identity invalidations remain separately observable");
    LAG_CHECK(after[VK_LAG_TEST_PRODUCER_SCANS] == before[VK_LAG_TEST_PRODUCER_SCANS] &&
        after[VK_LAG_TEST_PRODUCER_CROSS_TICKET] > before[VK_LAG_TEST_PRODUCER_CROSS_TICKET],
        "unchanged producer-only unit negative survives actual QueueSubmit and ticket change");
    vk_gpu_idle(); /* all real copies and registrar jobs finish before tracking retirement */
    nvn_dirty_remove_range((void *)(uintptr_t)tracked_alias); nvn_dirty_remove_range((void *)(uintptr_t)tracked);
    uint64_t retired_unwatched = vk_lag_copy_unwatched();
    vk_lag_test_stats_get(&counts_before);
    LAG_CHECK(vk_lag_read_record(untouched, 16, 16, &first, &reason, VK_LAG_INDEX) && !reason &&
        vk_lag_read_record(untouched, 16, 16, &changed, &reason, VK_LAG_INDEX) && !same_descriptor(first, changed) &&
        vk_lag_copy_unwatched() == retired_unwatched + 2, "retired tracking incarnation cannot retain unit/watch or exact native-copy authority");
    nvn_dirty_add_range((void *)(uintptr_t)tracked, tracked_bytes);
    nvn_dirty_add_range((void *)(uintptr_t)tracked_alias, tracked_bytes);
    LAG_CHECK(!vk_lag_read_record(untouched, 16, 16, &same, &reason, VK_LAG_INDEX) && (reason & NVN_DIRTY_LAG_COLD),
        "same-VA tracking rebirth starts with fresh cold classification");
    vk_lag_test_stats_get(&counts_after);
    LAG_CHECK(counts_after.kind[VK_LAG_INDEX].key_mapping - counts_before.kind[VK_LAG_INDEX].key_mapping >= 2,
        "tracking retirement and rebirth are independently visible mapping/lifetime misses");
    LAG_CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "contained-unit and retirement submission completes");
    nvn_dirty_remove_range((void *)(uintptr_t)tracked_alias); nvn_dirty_remove_range((void *)(uintptr_t)tracked);
    nvn_dirty_remove_range(alias); nvn_dirty_remove_range(physical);
    mtl_pool_del(physical); /* releases the donor and every synthetic alias after all GPU work has completed */
    munmap(alias, BYTES); munmap(physical, BYTES);
    vkUnmapMemory(V.dev, memory); vkDestroyBuffer(V.dev, destination, NULL); vkFreeMemory(V.dev, memory, NULL);
    printf("%s: lag binding memo, range revisions, producer materialization, alias CPU rewrites and ticket/slot lifetimes\n", errors ? "FAILED" : "ok");
#undef LAG_CHECK
    return errors;
}

int main(int argc, char **argv) {
    int benchmark = argc == 2 && !strcmp(argv[1], "--lag-bench");
    int projection_errors = projection_fixture(benchmark);
    if (benchmark) return projection_errors != 0; /* host-only: never initialize Vulkan */
    const char *rename = getenv("TOTK_VK_UB_RENAME");
    int renaming = rename && atoi(rename) && !vk_off("rename");
    uint8_t *a = mmap(NULL, 131072, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    uint8_t *b = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (a == MAP_FAILED || b == MAP_FAILED) return 1;
    mtl_init(); mtl_pool_add(a, 131072);
    /* Nested views exercise the exact same tie-breaking/range checks as the ordinary resolver. */
    mtl_pool_add(a + 16384, 32768);
    const vk_record_spans *old = vk_record_spans_capture(0);
    mtl_pool_add(b, 65536);
    const vk_record_spans *current = vk_record_spans_capture(0);
    int hints[64] = {0}, errors = projection_errors, checks = 0;
#define SPAN_CHECK(condition, ...) do { checks++; if (!(condition)) { errors++; fprintf(stderr, "FAIL: frozen spans "); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
    for (uint64_t off = 0; off < 131072; off += 37) {
        uint64_t address = (uint64_t)(uintptr_t)(a + off), n = 1 + (off % 8192);
        VkBuffer expected = VK_NULL_HANDLE, found = VK_NULL_HANDLE; uint64_t eo = 0, fo = 0;
        int x = vk_resolve_for(address, n, &expected, &eo, NULL, VK_LOOKUP_OTHER);
        int y = vk_record_resolve(current, address, n, &found, &fo, NULL, hints);
        SPAN_CHECK(x == y && (!x || (expected == found && eo == fo)), "current resolver agrees at offset %llu length %llu",
            (unsigned long long)off, (unsigned long long)n);
        y = vk_record_resolve(old, address, n, &found, &fo, NULL, NULL);
        SPAN_CHECK(x == y && (!x || (expected == found && eo == fo)), "old resolver agrees at offset %llu length %llu",
            (unsigned long long)off, (unsigned long long)n);
    }
    VkBuffer found; uint64_t offset;
    SPAN_CHECK(!vk_record_resolve(old, (uint64_t)(uintptr_t)b, 1, &found, &offset, NULL, NULL), "old spans exclude the later pool");
    SPAN_CHECK(vk_record_resolve(current, (uint64_t)(uintptr_t)b, 65536, &found, &offset, NULL, NULL), "current spans include the entire later pool");
    /* Reset may retire the old version only after all prior CPU jobs have been consumed. */
    vk_record_spans_reset(0);
    SPAN_CHECK(vk_record_spans_capture(0) == current, "reset preserves the current span version");
    if (vk_record_enabled()) {
        struct { op_hdr update; uint64_t address, size; uint32_t value[4]; op_hdr end; } stream = {
            {OP_UPDATE_UB, 40}, (uint64_t)(uintptr_t)a, 16, {1, 2, 3, 4}, {OP_END, 8}};
        mtl_sub_begin(); uint64_t revision = vk_record_uniform_revision(); mtl_exec((const uint8_t *)&stream);
        SPAN_CHECK(vk_record_uniform_unchanged(revision, (uint64_t)(uintptr_t)a + 4096, 16), "disjoint inline update preserves range revision");
        SPAN_CHECK(!vk_record_uniform_unchanged(revision, (uint64_t)(uintptr_t)a, 16), "overlapping inline update revokes range revision");
        for (int i = 0; i < 300; i++) { stream.value[0]++; mtl_exec((const uint8_t *)&stream); }
        SPAN_CHECK(!vk_record_uniform_unchanged(revision, (uint64_t)(uintptr_t)a + 4096, 16), "journal overflow conservatively revokes disjoint range revision");
        revision = vk_record_uniform_revision(); mtl_exec((const uint8_t *)&stream);
        SPAN_CHECK(vk_record_uniform_unchanged(revision, (uint64_t)(uintptr_t)a + 4096, 16), "fresh journal revision retains disjoint range");
        uint64_t other = (uint64_t)(uintptr_t)a + 4096;
        SPAN_CHECK(!vk_ub_pending(other, 16), "untouched range has no deferred uniform update");
        stream.address = other; mtl_exec((const uint8_t *)&stream);
        SPAN_CHECK(vk_ub_pending(other, 16) == (renaming || vk_lag_uniform_private()), "deferred uniform update state matches rename/private-lag policy");
        vk_ub_touch(other, 16);
        SPAN_CHECK(!vk_ub_pending(other, 16), "non-uniform touch materializes the deferred update");
        SPAN_CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "range revision submission completes");
    }
    {
        uint64_t address = (uint64_t)(uintptr_t)a + 8192, initial = 0x1122334455667788ull, second = 0x8877665544332211ull, result;
        memcpy((void *)(uintptr_t)(address + 8), &initial, 8); memcpy((void *)(uintptr_t)(address + 64), &second, 8);
        struct { op_hdr bind; p_bind_buf value; op_hdr end; } bindings = {
            {OP_BIND_UBO, 8 + sizeof(p_bind_buf)}, {0, 0, address, 256}, {OP_END, 8}};
        uint64_t recording = (uint64_t)(uintptr_t)&bindings;
        mtl_sub_begin(); mtl_sub_snapshot_ubos_cap(&recording, 1, NULL);
        struct { op_hdr update; uint64_t address, size; uint32_t value[4]; op_hdr end; } stream = {
            {OP_UPDATE_UB, 40}, address + 128, 16, {5, 6, 7, 8}, {OP_END, 8}};
        mtl_exec((const uint8_t *)&stream); uint64_t hits = vk_record_test_pointer_hits();
        vk_ub_read(address + 8, &result, 8);
        SPAN_CHECK(result == initial, "snapshot pointer initially reads its captured base");
        vk_ub_read(address + 8, &result, 8);
        SPAN_CHECK(result == initial, "repeated snapshot pointer read retains its captured base");
        stream.address = address + 144; mtl_exec((const uint8_t *)&stream);
        vk_ub_read(address + 8, &result, 8);
        SPAN_CHECK(result == initial, "disjoint inline update preserves snapshot pointer bytes");
        stream.address = address + 8; mtl_exec((const uint8_t *)&stream);
        vk_ub_read(address + 8, &result, 8);
        SPAN_CHECK(result == ((uint64_t)6 << 32 | 5), "overlapping inline update replaces snapshot pointer bytes");
        vk_ub_read(address + 64, &result, 8);
        SPAN_CHECK(result == second, "neighbouring snapshot pointer retains its captured base");
        SPAN_CHECK(!renaming || vk_off("snap") || !vk_record_enabled() || !vk_rec_profile_on ||
            vk_record_test_pointer_hits() - hits >= 2, "snapshot pointer memo records repeated and disjoint-update hits");
        if (vk_record_enabled()) {
            VkDescriptorBufferInfo first, same, changed;
            int private_uniform = vk_lag_uniform_read(address + 8, 8, 16, &first);
            SPAN_CHECK(private_uniform == vk_lag_uniform_private(), "private capture availability matches protected uniform policy");
            if (private_uniform) {
                /* Reuse requires the still-live explicit snapshot: a producer anywhere in its 256-byte extent
                   drops the entire snapshot, and this native pool deliberately has no dirty writer watches. */
                stream.address = address + 176; stream.value[0]++; mtl_exec((const uint8_t *)&stream);
                SPAN_CHECK(vk_lag_uniform_read(address + 8, 8, 16, &same) && same_descriptor(first, same),
                    "disjoint inline update reuses the live immutable snapshot capture");
                stream.address = address + 8; stream.value[0]++; mtl_exec((const uint8_t *)&stream);
                SPAN_CHECK(vk_lag_uniform_read(address + 8, 8, 16, &changed) && !same_descriptor(first, changed),
                    "overlapping inline update replaces the immutable snapshot capture");
                vk_ub_read(address + 8, &result, 8);
                SPAN_CHECK(result == ((uint64_t)stream.value[1] << 32 | stream.value[0]), "pointer read follows the latest private inline update");
                vk_gpu_write(address + 8, 8);
                SPAN_CHECK(!vk_lag_uniform_read(address + 8, 8, 16, &changed), "true GPU producer revokes the private snapshot capture");
                uint64_t revision = vk_record_uniform_revision();
                vk_gpu_write(address + 8, 8);
                SPAN_CHECK(!vk_record_uniform_unchanged(revision, address + 8, 8),
                    "repeated same-range GPU producer revokes range revision after snapshot and overlay are gone");
                SPAN_CHECK(!vk_lag_uniform_read(address + 8, 8, 16, &changed), "repeated same-range GPU producer retains live classification");
                uint64_t native = address + 224, unwatched = vk_lag_copy_unwatched();
                SPAN_CHECK(!nvn_dirty_tracked((void *)(uintptr_t)native, 8), "post-snapshot native probe has no dirty writer tracking");
                SPAN_CHECK(vk_lag_uniform_read(native, 8, 16, &first) && vk_lag_uniform_read(native, 8, 16, &same) &&
                    !same_descriptor(first, same) && vk_lag_copy_unwatched() == unwatched + 2,
                    "snapshot-wide invalidation permits fresh untracked native copies but forbids memo reuse");
            }
        }
        vk_gpu_write(address + 64, 8); second++; memcpy((void *)(uintptr_t)(address + 64), &second, 8);
        vk_ub_read(address + 64, &result, 8);
        SPAN_CHECK(result == second, "GPU producer revokes neighbouring snapshot pointer bytes");
        SPAN_CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "snapshot pointer submission completes");
    }
    errors += lag_cache_fixture(&checks);
    printf("%s: frozen spans %d checks, %d failed\n", errors ? "FAILED" : "ok", checks, errors);
#undef SPAN_CHECK
    return errors != 0;
}
