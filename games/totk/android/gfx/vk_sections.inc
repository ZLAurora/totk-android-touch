/* Included by nvn_vk.c: the section pools share the backend's slot lifetime,
 * but are exclusively walking-owned. Only the main pool is assembler-owned. */
#include "nvn_hmap.h"
int vk_one_primary_on;
vk_sections_token_state vk_sections_token;
_Static_assert(sizeof(vk_sections_token_state) == 64, "section token owns one complete cache line");
_Static_assert(_Alignof(vk_sections_token_state) == 64, "section token cache-line alignment");
/* Records and variable payloads are 8-byte aligned. No retained pointer refers
 * into this growable arena: sections hold offsets; replay rebuilds pointers. */
typedef struct {
    size_t bytes;
    vk_section_command command;
    union {
        VkDependencyInfo barrier;
        struct { VkBuffer src, dst; uint32_t count; } copy;
        struct { VkBuffer dst; VkDeviceSize offset, size; uint32_t data; } buffer;
        struct { VkQueryPool pool; VkPipelineStageFlags2 stage; uint32_t first, count; } query;
    };
} ordered_record;
_Static_assert(sizeof(ordered_record) % 8 == 0, "aligned deferred payload");
typedef struct {
    VkRenderingInfo info;
    VkRenderingAttachmentInfo color[8], depth, stencil;
    VkFormat formats[8], depth_format, stencil_format;
    VkSampleCountFlagBits samples;
    VkCommandBuffer cb;
    void *pass;
    size_t first_record, end_record;
    int rendering, worker, has_depth, has_stencil;
} ordered_section;
typedef struct {
    VkCommandPool pool;
    VkCommandBuffer primary, *buffers;
    uint32_t nbuffers, capbuffers, used;
    ordered_section *section;
    uint32_t count, capacity, cursor, open;
    unsigned char *arena;
    size_t arena_used, arena_capacity, arena_growths;
    int begun;
} ordered_slot;
static ordered_slot ordered_slots[8];
/* Walking ownership is serialized by vk_rec_lock, not tied to an OS thread.
 * Publishing only the stable token lets workers/assembler reject their own
 * handles without reading mutable walker state or g_cur. */
static _Atomic(ordered_slot *) ordered_current;
static _Atomic uint64_t ordered_counts[6];
static _Atomic uint64_t ordered_auxiliary_begins;
static _Alignas(64) _Atomic uint64_t ordered_walk_ns;
static _Alignas(64) _Atomic uint64_t ordered_assemble_ns;
static _Atomic uint64_t ordered_commands[VK_SECTION_COMMAND_COUNT], ordered_deferred[3];
void vk_sections_command_counts(uint64_t out[VK_SECTION_COMMAND_COUNT]) {
    for (unsigned i = 0; i < VK_SECTION_COMMAND_COUNT; i++)
        out[i] = atomic_load_explicit(&ordered_commands[i], memory_order_relaxed);
}
void vk_sections_deferred_counts(uint64_t out[3]) {
    for (unsigned i = 0; i < 3; i++)
        out[i] = atomic_load_explicit(&ordered_deferred[i], memory_order_relaxed);
}
#ifdef TOTK_VK_TESTS
void vk_sections_test_storage(unsigned slot, size_t out[3]) {
    ordered_slot *s = &ordered_slots[slot];
    out[0] = s->arena_used; out[1] = s->arena_capacity; out[2] = s->arena_growths;
}
#endif
void vk_sections_times(uint64_t out[2]) {
    out[0] = atomic_load_explicit(&ordered_walk_ns, memory_order_relaxed);
    out[1] = atomic_load_explicit(&ordered_assemble_ns, memory_order_relaxed);
}
static int ordered_fallback;
static void ordered_note_primary(int auxiliary) {
    if (auxiliary) atomic_fetch_add_explicit(&ordered_auxiliary_begins, 1, memory_order_relaxed);
    else atomic_fetch_add_explicit(&ordered_counts[0], 1, memory_order_relaxed);
}
uint64_t vk_sections_auxiliary_count(void) {
    return atomic_load_explicit(&ordered_auxiliary_begins, memory_order_relaxed);
}
static pthread_mutex_t ordered_views_mu = PTHREAD_MUTEX_INITIALIZER;
static nvn_hmap ordered_images, ordered_views;
void vk_sections_counts(uint64_t out[6]) {
    for (unsigned i = 0; i < 6; i++) out[i] = atomic_load_explicit(&ordered_counts[i], memory_order_relaxed);
}
#ifdef TOTK_VK_TESTS
void vk_sections_test_counts(uint64_t out[6]) { vk_sections_counts(out); }
#endif
void vk_sections_init(void) {
    const char *e = getenv("TOTK_VK_REC_ONE_PRIMARY");
    vk_one_primary_on = e && !strcmp(e, "1");
    /* These primary query spans cannot inherit queries with the device features
     * currently enabled. Select legacy before any slot is opened, never mid-slot. */
    const char *occlusion = getenv("TOTK_VK_OCCLUSION"), *stats = getenv("TOTK_VK_GPU_PROFILE_STATS");
    /* Reuse the recorder's actual admission result, including diagnostics and
     * thread-creation failure, instead of maintaining a second policy parser.
     * Only enabled statistics need early admission; workers cannot see a job
     * until device initialization and the first slot have completed. */
    int recorder = !vk_one_primary_on || !stats || strcmp(stats, "1") || vk_record_enabled();
    const char *framegen = getenv("TOTK_FRAMEGEN");
    int inline_capture = framegen && (!strcmp(framegen, "1") || !strcmp(framegen, "interp") ||
        !strcmp(framegen, "hybrid") || !strcmp(framegen, "locked") || !strcmp(framegen, "adaptive"));
    if (vk_one_primary_on && ((occlusion && *occlusion && strcmp(occlusion, "0")) ||
            (stats && !strcmp(stats, "1") && (!recorder || inline_capture)))) {
        vk_one_primary_on = 0;
        ordered_fallback = 1;
        LOGV("one-primary fallback: primary occlusion/inline statistics query inheritance unavailable");
    }
    if (vk_one_primary_on) {
        nvn_hm_init(&ordered_images, sizeof(VkImage));
        nvn_hm_init(&ordered_views, sizeof(VkImageView));
    }
    LOGV("TOTK_VK_REC_ONE_PRIMARY=%d (requested=%s; default=0; one main primary per slot, pre/adopted unchanged)",
        vk_one_primary_on, e ? e : "unset");
}
VkResult vk_sections_create_image(VkDevice dev, const VkImageCreateInfo *info, const VkAllocationCallbacks *alloc, VkImage *image) {
    VkResult result = (vkCreateImage)(dev, info, alloc, image);
    if (vk_one_primary_on && !result) {
        pthread_mutex_lock(&ordered_views_mu);
        nvn_hm_put(&ordered_images, image, (void *)(uintptr_t)info->samples);
        pthread_mutex_unlock(&ordered_views_mu);
    }
    return result;
}
void vk_sections_destroy_image(VkDevice dev, VkImage image, const VkAllocationCallbacks *alloc) {
    if (vk_one_primary_on) {
        pthread_mutex_lock(&ordered_views_mu); nvn_hm_del(&ordered_images, &image); pthread_mutex_unlock(&ordered_views_mu);
    }
    (vkDestroyImage)(dev, image, alloc);
}
VkResult vk_sections_create_view(VkDevice dev, const VkImageViewCreateInfo *info, const VkAllocationCallbacks *alloc, VkImageView *view) {
    VkResult result = (vkCreateImageView)(dev, info, alloc, view);
    if (vk_one_primary_on && !result) {
        pthread_mutex_lock(&ordered_views_mu);
        uintptr_t samples = (uintptr_t)nvn_hm_get(&ordered_images, &info->image);
        if (!samples) DIE("one-primary image view has no image sample metadata");
        uintptr_t metadata = (samples << 32) | (uint32_t)info->format;
        nvn_hm_put(&ordered_views, view, (void *)metadata);
        pthread_mutex_unlock(&ordered_views_mu);
    }
    return result;
}
void vk_sections_destroy_view(VkDevice dev, VkImageView view, const VkAllocationCallbacks *alloc) {
    if (vk_one_primary_on) {
        pthread_mutex_lock(&ordered_views_mu); nvn_hm_del(&ordered_views, &view); pthread_mutex_unlock(&ordered_views_mu);
    }
    (vkDestroyImageView)(dev, view, alloc);
}
static VkFormat ordered_attachment(VkImageView view, VkSampleCountFlagBits *samples) {
    if (!view) return VK_FORMAT_UNDEFINED;
    uintptr_t metadata = (uintptr_t)nvn_hm_get(&ordered_views, &view);
    if (!metadata) DIE("one-primary rendering attachment has no view metadata");
    *samples = (VkSampleCountFlagBits)(metadata >> 32);
    return (VkFormat)(uint32_t)metadata;
}
static void ordered_close(ordered_slot *s) {
    if (!s->open) return;
    uint64_t start = host_now_ns();
    ordered_section *section = &s->section[s->open - 1];
    if (section->cb) VKCHECK(vkEndCommandBuffer(section->cb));
    if (!section->rendering) atomic_fetch_add_explicit(&ordered_deferred[section->cb ? 2 : 1], 1, memory_order_relaxed);
    vk_draw_secondary_reset();
    s->open = 0;
    atomic_fetch_add_explicit(&ordered_walk_ns, host_now_ns() - start, memory_order_relaxed);
}
static ordered_section *ordered_append(ordered_slot *s) {
    if (s->count == s->capacity) {
        uint32_t capacity = s->capacity ? s->capacity * 2 : 32;
        if (capacity < s->capacity) DIE("one-primary section count overflow");
        s->section = VK_REALLOC(s->section, (size_t)capacity * sizeof *s->section); s->capacity = capacity;
    }
    ordered_section *section = &s->section[s->count++];
    memset(section, 0, sizeof *section); s->open = s->count;
    return section;
}
static VkCommandBuffer ordered_secondary(ordered_slot *s, const ordered_section *section) {
    uint64_t start = host_now_ns();
    if (s->used == s->nbuffers) {
        if (s->nbuffers == s->capbuffers) {
            uint32_t capacity = s->capbuffers ? s->capbuffers * 2 : 32;
            if (capacity < s->capbuffers) DIE("one-primary secondary count overflow");
            s->buffers = VK_REALLOC(s->buffers, (size_t)capacity * sizeof *s->buffers); s->capbuffers = capacity;
        }
        uint32_t count = s->capbuffers - s->nbuffers;
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = s->pool, .level = VK_COMMAND_BUFFER_LEVEL_SECONDARY, .commandBufferCount = count};
        VKCHECK(vkAllocateCommandBuffers(V.dev, &ai, s->buffers + s->nbuffers)); s->nbuffers += count;
    }
    VkCommandBuffer cb = s->buffers[s->used++];
    VkCommandBufferInheritanceRenderingInfo rendering = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO,
        .viewMask = section->info.viewMask, .colorAttachmentCount = section->info.colorAttachmentCount,
        .pColorAttachmentFormats = section->formats, .depthAttachmentFormat = section->depth_format,
        .stencilAttachmentFormat = section->stencil_format, .rasterizationSamples = section->samples};
    VkCommandBufferInheritanceInfo inheritance = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
        .pNext = section->rendering ? &rendering : NULL};
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT |
            (section->rendering ? VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT : 0), .pInheritanceInfo = &inheritance};
    VKCHECK(vkBeginCommandBuffer(cb, &begin));
    atomic_fetch_add_explicit(&ordered_counts[4], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&ordered_walk_ns, host_now_ns() - start, memory_order_relaxed);
    return cb;
}
void vk_sections_reset(unsigned slot, VkCommandBuffer primary) {
    ordered_slot *s = &ordered_slots[slot];
    if (atomic_load_explicit(&ordered_current, memory_order_acquire)) DIE("one-primary slot opened before previous walk sealed");
    if (!s->pool) {
        VkCommandPoolCreateInfo info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, .queueFamilyIndex = V.qfam};
        VKCHECK(vkCreateCommandPool(V.dev, &info, NULL, &s->pool));
    } else VKCHECK(vkResetCommandPool(V.dev, s->pool, 0));
    if (s->primary && s->primary != primary) DIE("one-primary slot changed its stable handle");
    if (!s->primary) s->primary = primary;
    s->count = s->cursor = s->open = s->used = 0; s->begun = 0;
    s->arena_used = 0;
    atomic_store_explicit(&ordered_current, s, memory_order_release);
    atomic_store_explicit(&vk_sections_token.cb, primary, memory_order_relaxed);
}
static void ordered_replay(ordered_slot *s, ordered_section *section, VkCommandBuffer cb) {
    for (size_t offset = section->first_record; offset < section->end_record;) {
        const ordered_record *r = (const void *)(s->arena + offset);
        const void *payload = r + 1;
        switch (r->command) {
        case VK_SECTION_PipelineBarrier2: {
            VkDependencyInfo info = r->barrier;
            info.pMemoryBarriers = payload;
            info.pBufferMemoryBarriers = (const void *)((const unsigned char *)payload +
                info.memoryBarrierCount * sizeof(VkMemoryBarrier2));
            info.pImageMemoryBarriers = (const void *)((const unsigned char *)info.pBufferMemoryBarriers +
                info.bufferMemoryBarrierCount * sizeof(VkBufferMemoryBarrier2));
            (vkCmdPipelineBarrier2)(cb, &info);
            break;
        }
        case VK_SECTION_CopyBuffer:
            (vkCmdCopyBuffer)(cb, r->copy.src, r->copy.dst, r->copy.count, payload); break;
        case VK_SECTION_UpdateBuffer:
            (vkCmdUpdateBuffer)(cb, r->buffer.dst, r->buffer.offset, r->buffer.size, payload); break;
        case VK_SECTION_FillBuffer:
            (vkCmdFillBuffer)(cb, r->buffer.dst, r->buffer.offset, r->buffer.size, r->buffer.data); break;
        case VK_SECTION_ResetQueryPool:
            (vkCmdResetQueryPool)(cb, r->query.pool, r->query.first, r->query.count); break;
        case VK_SECTION_WriteTimestamp2:
            (vkCmdWriteTimestamp2)(cb, r->query.stage, r->query.pool, r->query.first); break;
        default: DIE("one-primary unknown deferred command");
        }
        offset += r->bytes;
    }
}
static ordered_section *ordered_immediate(ordered_slot *s) {
    if (s->open) return &s->section[s->open - 1];
    ordered_section *section = ordered_append(s);
    atomic_fetch_add_explicit(&ordered_counts[2], 1, memory_order_relaxed);
    return section;
}
static void ordered_note_command(ordered_slot *s, vk_section_command command) {
    if (!s->open || !s->section[s->open - 1].rendering)
        atomic_fetch_add_explicit(&ordered_commands[command], 1, memory_order_relaxed);
}
static VkCommandBuffer ordered_materialize(ordered_slot *s, ordered_section *section) {
    if (section->worker) DIE("one-primary direct command inside a worker-rendered pass");
    if (!section->cb) {
        section->cb = ordered_secondary(s, section);
        /* A single secondary owns the whole mixed section. Replay the prefix
         * before its first unsupported command, then record everything here. */
        ordered_replay(s, section, section->cb);
        section->first_record = section->end_record = 0;
    }
    return section->cb;
}
VkCommandBuffer vk_sections_route_command(VkCommandBuffer cb, vk_section_command command) {
    ordered_slot *s = atomic_load_explicit(&ordered_current, memory_order_acquire);
    if (!s || cb != s->primary) return cb;
    ordered_note_command(s, command);
    return ordered_materialize(s, ordered_immediate(s));
}
VkCommandBuffer vk_sections_route(VkCommandBuffer cb) {
    return vk_sections_route_command(cb, VK_SECTION_External);
}
static ordered_record *ordered_allocate(ordered_slot *s, ordered_section *section, vk_section_command command, size_t payload) {
    if (payload > SIZE_MAX - sizeof(ordered_record) - 7) DIE("one-primary deferred payload overflow");
    size_t bytes = (sizeof(ordered_record) + payload + 7) & ~(size_t)7;
    if (bytes > SIZE_MAX - s->arena_used) DIE("one-primary arena size overflow");
    size_t end = s->arena_used + bytes;
    if (end > s->arena_capacity) {
        size_t capacity = s->arena_capacity ? s->arena_capacity : 4096;
        while (capacity < end) {
            if (capacity > SIZE_MAX / 2) { capacity = end; break; }
            capacity *= 2;
        }
        s->arena = VK_REALLOC(s->arena, capacity); s->arena_capacity = capacity; s->arena_growths++;
    }
    if (section->first_record == section->end_record) section->first_record = s->arena_used;
    ordered_record *r = (void *)(s->arena + s->arena_used);
    r->bytes = bytes; r->command = command;
    s->arena_used = section->end_record = end;
    atomic_fetch_add_explicit(&ordered_deferred[0], 1, memory_order_relaxed);
    return r;
}
/* Only an active walking token enters these functions. Rendering and already
 * materialized sections use the raw PFN, preserving secondary-local state. */
static ordered_section *ordered_defer(vk_section_command command, ordered_slot **out) {
    ordered_slot *s = atomic_load_explicit(&ordered_current, memory_order_acquire);
    if (!s) DIE("one-primary deferred command without walking ownership");
    ordered_note_command(s, command);
    *out = s;
    return ordered_immediate(s);
}
void vk_sections_barrier(VkCommandBuffer cb, const VkDependencyInfo *info) {
    ordered_slot *s; ordered_section *section = ordered_defer(VK_SECTION_PipelineBarrier2, &s);
    int extensions = info->pNext != NULL;
    for (uint32_t i = 0; i < info->memoryBarrierCount; i++) extensions |= info->pMemoryBarriers[i].pNext != NULL;
    for (uint32_t i = 0; i < info->bufferMemoryBarrierCount; i++) extensions |= info->pBufferMemoryBarriers[i].pNext != NULL;
    for (uint32_t i = 0; i < info->imageMemoryBarrierCount; i++) extensions |= info->pImageMemoryBarriers[i].pNext != NULL;
    if (section->rendering || section->cb || extensions) {
        (vkCmdPipelineBarrier2)(ordered_materialize(s, section), info); return;
    }
    size_t memory = (size_t)info->memoryBarrierCount * sizeof(VkMemoryBarrier2);
    size_t buffer = (size_t)info->bufferMemoryBarrierCount * sizeof(VkBufferMemoryBarrier2);
    size_t image = (size_t)info->imageMemoryBarrierCount * sizeof(VkImageMemoryBarrier2);
    ordered_record *r = ordered_allocate(s, section, VK_SECTION_PipelineBarrier2, memory + buffer + image);
    r->barrier = *info; r->barrier.pMemoryBarriers = NULL;
    r->barrier.pBufferMemoryBarriers = NULL; r->barrier.pImageMemoryBarriers = NULL;
    unsigned char *p = (void *)(r + 1);
    if (memory) memcpy(p, info->pMemoryBarriers, memory);
    if (buffer) memcpy(p + memory, info->pBufferMemoryBarriers, buffer);
    if (image) memcpy(p + memory + buffer, info->pImageMemoryBarriers, image);
}
void vk_sections_copy(VkCommandBuffer cb, VkBuffer src, VkBuffer dst, uint32_t count, const VkBufferCopy *regions) {
    ordered_slot *s; ordered_section *section = ordered_defer(VK_SECTION_CopyBuffer, &s);
    if (section->rendering || section->cb) { (vkCmdCopyBuffer)(ordered_materialize(s, section), src, dst, count, regions); return; }
    size_t bytes = (size_t)count * sizeof *regions;
    ordered_record *r = ordered_allocate(s, section, VK_SECTION_CopyBuffer, bytes);
    r->copy.src = src; r->copy.dst = dst; r->copy.count = count;
    if (bytes) memcpy(r + 1, regions, bytes);
}
void vk_sections_update(VkCommandBuffer cb, VkBuffer dst, VkDeviceSize offset, VkDeviceSize size, const void *data) {
    ordered_slot *s; ordered_section *section = ordered_defer(VK_SECTION_UpdateBuffer, &s);
    if (section->rendering || section->cb) { (vkCmdUpdateBuffer)(ordered_materialize(s, section), dst, offset, size, data); return; }
    ordered_record *r = ordered_allocate(s, section, VK_SECTION_UpdateBuffer, size);
    r->buffer.dst = dst; r->buffer.offset = offset; r->buffer.size = size;
    if (size) memcpy(r + 1, data, size);
}
void vk_sections_fill(VkCommandBuffer cb, VkBuffer dst, VkDeviceSize offset, VkDeviceSize size, uint32_t data) {
    ordered_slot *s; ordered_section *section = ordered_defer(VK_SECTION_FillBuffer, &s);
    if (section->rendering || section->cb) { (vkCmdFillBuffer)(ordered_materialize(s, section), dst, offset, size, data); return; }
    ordered_record *r = ordered_allocate(s, section, VK_SECTION_FillBuffer, 0);
    r->buffer.dst = dst; r->buffer.offset = offset; r->buffer.size = size; r->buffer.data = data;
}
void vk_sections_query_reset(VkCommandBuffer cb, VkQueryPool pool, uint32_t first, uint32_t count) {
    ordered_slot *s; ordered_section *section = ordered_defer(VK_SECTION_ResetQueryPool, &s);
    if (section->rendering || section->cb) { (vkCmdResetQueryPool)(ordered_materialize(s, section), pool, first, count); return; }
    ordered_record *r = ordered_allocate(s, section, VK_SECTION_ResetQueryPool, 0);
    r->query.pool = pool; r->query.first = first; r->query.count = count;
}
void vk_sections_timestamp(VkCommandBuffer cb, VkPipelineStageFlags2 stage, VkQueryPool pool, uint32_t query) {
    ordered_slot *s; ordered_section *section = ordered_defer(VK_SECTION_WriteTimestamp2, &s);
    if (section->rendering || section->cb) { (vkCmdWriteTimestamp2)(ordered_materialize(s, section), stage, pool, query); return; }
    ordered_record *r = ordered_allocate(s, section, VK_SECTION_WriteTimestamp2, 0);
    r->query.pool = pool; r->query.stage = stage; r->query.first = query;
}
void vk_sections_begin(VkCommandBuffer cb, const VkRenderingInfo *info) {
    ordered_slot *s = atomic_load_explicit(&ordered_current, memory_order_acquire);
    if (!vk_one_primary_on || !s || cb != s->primary) { (vkCmdBeginRendering)(cb, info); return; }
    if (s->open && s->section[s->open - 1].rendering) DIE("one-primary nested rendering");
    ordered_close(s);
    ordered_section *section = ordered_append(s);
    section->rendering = 1; section->worker = !!(info->flags & VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT);
    if (info->pNext || info->colorAttachmentCount > 8 ||
            (info->flags & (VK_RENDERING_SUSPENDING_BIT | VK_RENDERING_RESUMING_BIT)))
        DIE("one-primary unsupported rendering description");
    section->info = *info;
    if (info->colorAttachmentCount) memcpy(section->color, info->pColorAttachments, info->colorAttachmentCount * sizeof *section->color);
    if (info->pDepthAttachment) section->depth = *info->pDepthAttachment;
    if (info->pStencilAttachment) section->stencil = *info->pStencilAttachment;
    /* Store presence, never stack pointers; materialize pointers at emission. */
    section->info.pColorAttachments = NULL;
    section->has_depth = info->pDepthAttachment != NULL;
    section->has_stencil = info->pStencilAttachment != NULL;
    section->info.pDepthAttachment = section->info.pStencilAttachment = NULL;
    section->samples = VK_SAMPLE_COUNT_1_BIT;
    if (!section->worker) {
        pthread_mutex_lock(&ordered_views_mu);
        for (uint32_t i = 0; i < info->colorAttachmentCount; i++) section->formats[i] = ordered_attachment(section->color[i].imageView, &section->samples);
        section->depth_format = ordered_attachment(section->depth.imageView, &section->samples);
        section->stencil_format = ordered_attachment(section->stencil.imageView, &section->samples);
        pthread_mutex_unlock(&ordered_views_mu);
        atomic_fetch_add_explicit(&ordered_counts[3], 1, memory_order_relaxed);
    } else atomic_fetch_add_explicit(&ordered_counts[1], 1, memory_order_relaxed);
}
void vk_sections_end(VkCommandBuffer cb) {
    ordered_slot *s = atomic_load_explicit(&ordered_current, memory_order_acquire);
    if (!vk_one_primary_on || !s || cb != s->primary) { (vkCmdEndRendering)(cb); return; }
    if (!s->open || !s->section[s->open - 1].rendering) DIE("one-primary end outside rendering");
    ordered_close(s);
}
void vk_sections_park(void *pass) {
    ordered_slot *s = atomic_load_explicit(&ordered_current, memory_order_acquire);
    if (!s || !s->open || !s->section[s->open - 1].worker) DIE("one-primary worker pass missing rendering section");
    s->section[s->open - 1].pass = pass; ordered_close(s);
}
void vk_sections_seal(void) {
    ordered_slot *s = atomic_load_explicit(&ordered_current, memory_order_acquire);
    if (!s) return;
    if (s->open && s->section[s->open - 1].rendering) DIE("one-primary sealed inside rendering");
    ordered_close(s);
    atomic_store_explicit(&vk_sections_token.cb, VK_NULL_HANDLE, memory_order_relaxed);
    atomic_store_explicit(&ordered_current, NULL, memory_order_release);
}
void vk_sections_resume(unsigned slot) {
    atomic_store_explicit(&ordered_current, &ordered_slots[slot], memory_order_release);
    atomic_store_explicit(&vk_sections_token.cb, ordered_slots[slot].primary, memory_order_relaxed);
}
void vk_sections_cancel(void) {
    ordered_slot *s = atomic_load_explicit(&ordered_current, memory_order_acquire);
    if (s) {
        ordered_close(s);
        s->count = s->cursor = 0;
        atomic_store_explicit(&vk_sections_token.cb, VK_NULL_HANDLE, memory_order_relaxed);
        atomic_store_explicit(&ordered_current, NULL, memory_order_release);
    }
}
void vk_sections_until(unsigned slot, void *pass) {
    uint64_t start = host_now_ns();
    ordered_slot *s = &ordered_slots[slot];
    if (!s->begun) {
        VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        VKCHECK(vkBeginCommandBuffer(s->primary, &begin)); s->begun = 1;
        atomic_fetch_add_explicit(&ordered_counts[0], 1, memory_order_relaxed);
    }
    /* A synchronous test-only early assembly may leave the walk active. Raw
     * calls below deliberately bypass token routing, never the public macros. */
    while (s->cursor < s->count) {
        ordered_section *section = &s->section[s->cursor++];
        if (section->rendering) {
            VkRenderingInfo info = section->info;
            info.flags |= VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT;
            info.pColorAttachments = section->color;
            info.pDepthAttachment = section->has_depth ? &section->depth : NULL;
            info.pStencilAttachment = section->has_stencil ? &section->stencil : NULL;
            (vkCmdBeginRendering)(s->primary, &info);
            if (section->pass) {
                if (section->pass != pass) DIE("one-primary recorder section order mismatch");
                atomic_fetch_add_explicit(&ordered_assemble_ns, host_now_ns() - start, memory_order_relaxed);
                return; /* recorder emits its jobs, EndRendering and profiler end */
            }
            if (section->cb) (vkCmdExecuteCommands)(s->primary, 1, &section->cb);
            (vkCmdEndRendering)(s->primary);
        } else if (section->cb) (vkCmdExecuteCommands)(s->primary, 1, &section->cb);
        else ordered_replay(s, section, s->primary);
    }
    if (pass) DIE("one-primary recorder section not found");
    atomic_fetch_add_explicit(&ordered_assemble_ns, host_now_ns() - start, memory_order_relaxed);
}
void vk_sections_finish(unsigned slot) {
    ordered_slot *s = &ordered_slots[slot];
    if (!s->begun || s->cursor != s->count) DIE("one-primary queue before assembly");
    VKCHECK(vkEndCommandBuffer(s->primary));
}
