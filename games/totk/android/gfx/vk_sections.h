/* Ordered one-primary sections. The walking main handle remains a stable token:
 * helpers may retain it across pass boundaries. Only that thread's active token
 * is routed. Worker, pre, adopted and independently owned buffers are untouched.
 * Raw PFNs remain available as (vkCmdFoo)(...), notably to the assembler. */
#pragma once
extern int vk_one_primary_on;
#include "vk_load.h"
#include <stdint.h>
#include <stdatomic.h>
#include <stddef.h>
/* Keep command identities shared by routing, deferred replay and diagnostics. */
#define VK_SECTION_COMMANDS(X) \
    X(PipelineBarrier2) X(CopyBuffer) X(UpdateBuffer) X(FillBuffer) \
    X(ResetQueryPool) X(WriteTimestamp2) X(WaitEvents) X(BeginQuery) X(EndQuery) \
    X(CopyQueryPoolResults) X(CopyBufferToImage) X(CopyImageToBuffer) X(CopyImage) \
    X(BlitImage) X(ClearColorImage) X(ClearDepthStencilImage) X(ClearAttachments) \
    X(PushConstants) X(BindPipeline) X(BindDescriptorSets) X(BindVertexBuffers2) \
    X(BindIndexBuffer) X(Draw) X(DrawIndexed) X(Dispatch) X(SetViewport) \
    X(SetScissor) X(SetDepthBias) X(SetBlendConstants) X(SetStencilCompareMask) \
    X(SetStencilWriteMask) X(SetStencilReference) X(SetCullMode) X(SetFrontFace) \
    X(SetPrimitiveTopology) X(SetDepthTestEnable) X(SetDepthWriteEnable) \
    X(SetDepthCompareOp) X(SetStencilTestEnable) X(SetStencilOp) \
    X(SetDepthBiasEnable) X(SetPrimitiveRestartEnable) X(SetRasterizerDiscardEnable) \
    X(SetLineWidth) X(External)
typedef enum {
#define VK_SECTION_ENUM(name) VK_SECTION_##name,
    VK_SECTION_COMMANDS(VK_SECTION_ENUM)
#undef VK_SECTION_ENUM
    VK_SECTION_COMMAND_COUNT
} vk_section_command;
void vk_sections_command_counts(uint64_t out[VK_SECTION_COMMAND_COUNT]);
void vk_sections_deferred_counts(uint64_t out[3]);
VkCommandBuffer vk_sections_route_command(VkCommandBuffer cb, vk_section_command command);
/* A full cache line, not just aligned storage: counter updates must never
 * invalidate the token line read by non-walking command-buffer owners. */
typedef struct {
    _Alignas(64) _Atomic(VkCommandBuffer) cb;
    unsigned char padding[64 - sizeof(_Atomic(VkCommandBuffer))];
} vk_sections_token_state;
extern vk_sections_token_state vk_sections_token;
void vk_sections_init(void);
void vk_sections_reset(unsigned slot, VkCommandBuffer primary);
void vk_sections_seal(void);
void vk_sections_cancel(void);
void vk_sections_resume(unsigned slot); /* synchronous test-only early assembly */
void vk_sections_park(void *pass);
void vk_sections_until(unsigned slot, void *pass);
void vk_sections_finish(unsigned slot);
VkCommandBuffer vk_sections_route(VkCommandBuffer cb);
static inline __attribute__((always_inline)) VkCommandBuffer vk_sections_command_buffer(VkCommandBuffer cb) {
    if (vk_one_primary_on &&
            cb == atomic_load_explicit(&vk_sections_token.cb, memory_order_relaxed))
        return vk_sections_route(cb);
    return cb;
}
static inline __attribute__((always_inline)) int vk_sections_active(VkCommandBuffer cb) {
    return vk_one_primary_on &&
        cb == atomic_load_explicit(&vk_sections_token.cb, memory_order_relaxed);
}
static inline __attribute__((always_inline)) VkCommandBuffer vk_sections_command_kind(VkCommandBuffer cb, vk_section_command command) {
    return vk_sections_active(cb) ? vk_sections_route_command(cb, command) : cb;
}
void vk_sections_barrier(VkCommandBuffer cb, const VkDependencyInfo *info);
void vk_sections_copy(VkCommandBuffer cb, VkBuffer src, VkBuffer dst, uint32_t count, const VkBufferCopy *regions);
void vk_sections_update(VkCommandBuffer cb, VkBuffer dst, VkDeviceSize offset, VkDeviceSize size, const void *data);
void vk_sections_fill(VkCommandBuffer cb, VkBuffer dst, VkDeviceSize offset, VkDeviceSize size, uint32_t data);
void vk_sections_query_reset(VkCommandBuffer cb, VkQueryPool pool, uint32_t first, uint32_t count);
void vk_sections_timestamp(VkCommandBuffer cb, VkPipelineStageFlags2 stage, VkQueryPool pool, uint32_t query);
void vk_sections_begin(VkCommandBuffer cb, const VkRenderingInfo *info);
void vk_sections_end(VkCommandBuffer cb);
void vk_sections_counts(uint64_t out[6]);
uint64_t vk_sections_auxiliary_count(void);
void vk_sections_times(uint64_t out[2]); /* cumulative walker secondary / assembler until ns */
void vk_draw_secondary_reset(void);
#ifdef TOTK_VK_TESTS
void vk_sections_test_counts(uint64_t out[6]);
void vk_sections_test_storage(unsigned slot, size_t out[3]);
#endif
VkResult vk_sections_create_image(VkDevice dev, const VkImageCreateInfo *info, const VkAllocationCallbacks *alloc, VkImage *image);
void vk_sections_destroy_image(VkDevice dev, VkImage image, const VkAllocationCallbacks *alloc);
VkResult vk_sections_create_view(VkDevice dev, const VkImageViewCreateInfo *info, const VkAllocationCallbacks *alloc, VkImageView *view);
void vk_sections_destroy_view(VkDevice dev, VkImageView view, const VkAllocationCallbacks *alloc);
#define VK_SECTION_CB(cb) vk_sections_command_buffer(cb)
#define vkCmdWaitEvents(cb, ...) vkCmdWaitEvents(vk_sections_command_kind(cb, VK_SECTION_WaitEvents), __VA_ARGS__)
#define vkCmdResetQueryPool(cb, pool, first, count) do { VkCommandBuffer _cb = (cb); if (vk_sections_active(_cb)) vk_sections_query_reset(_cb, pool, first, count); else (vkCmdResetQueryPool)(_cb, pool, first, count); } while (0)
#define vkCmdBeginQuery(cb, ...) vkCmdBeginQuery(vk_sections_command_kind(cb, VK_SECTION_BeginQuery), __VA_ARGS__)
#define vkCmdEndQuery(cb, ...) vkCmdEndQuery(vk_sections_command_kind(cb, VK_SECTION_EndQuery), __VA_ARGS__)
#define vkCmdCopyQueryPoolResults(cb, ...) vkCmdCopyQueryPoolResults(vk_sections_command_kind(cb, VK_SECTION_CopyQueryPoolResults), __VA_ARGS__)
#define vkCmdWriteTimestamp2(cb, stage, pool, query) do { VkCommandBuffer _cb = (cb); if (vk_sections_active(_cb)) vk_sections_timestamp(_cb, stage, pool, query); else (vkCmdWriteTimestamp2)(_cb, stage, pool, query); } while (0)
#define vkCmdPipelineBarrier2(cb, info) do { VkCommandBuffer _cb = (cb); if (vk_sections_active(_cb)) vk_sections_barrier(_cb, info); else (vkCmdPipelineBarrier2)(_cb, info); } while (0)
#define vkCmdCopyBuffer(cb, src, dst, count, regions) do { VkCommandBuffer _cb = (cb); if (vk_sections_active(_cb)) vk_sections_copy(_cb, src, dst, count, regions); else (vkCmdCopyBuffer)(_cb, src, dst, count, regions); } while (0)
#define vkCmdUpdateBuffer(cb, dst, offset, size, data) do { VkCommandBuffer _cb = (cb); if (vk_sections_active(_cb)) vk_sections_update(_cb, dst, offset, size, data); else (vkCmdUpdateBuffer)(_cb, dst, offset, size, data); } while (0)
#define vkCmdFillBuffer(cb, dst, offset, size, data) do { VkCommandBuffer _cb = (cb); if (vk_sections_active(_cb)) vk_sections_fill(_cb, dst, offset, size, data); else (vkCmdFillBuffer)(_cb, dst, offset, size, data); } while (0)
#define vkCmdCopyBufferToImage(cb, ...) vkCmdCopyBufferToImage(vk_sections_command_kind(cb, VK_SECTION_CopyBufferToImage), __VA_ARGS__)
#define vkCmdCopyImageToBuffer(cb, ...) vkCmdCopyImageToBuffer(vk_sections_command_kind(cb, VK_SECTION_CopyImageToBuffer), __VA_ARGS__)
#define vkCmdCopyImage(cb, ...) vkCmdCopyImage(vk_sections_command_kind(cb, VK_SECTION_CopyImage), __VA_ARGS__)
#define vkCmdBlitImage(cb, ...) vkCmdBlitImage(vk_sections_command_kind(cb, VK_SECTION_BlitImage), __VA_ARGS__)
#define vkCmdClearColorImage(cb, ...) vkCmdClearColorImage(vk_sections_command_kind(cb, VK_SECTION_ClearColorImage), __VA_ARGS__)
#define vkCmdClearDepthStencilImage(cb, ...) vkCmdClearDepthStencilImage(vk_sections_command_kind(cb, VK_SECTION_ClearDepthStencilImage), __VA_ARGS__)
#define vkCmdClearAttachments(cb, ...) vkCmdClearAttachments(vk_sections_command_kind(cb, VK_SECTION_ClearAttachments), __VA_ARGS__)
#define vkCmdPushConstants(cb, ...) vkCmdPushConstants(vk_sections_command_kind(cb, VK_SECTION_PushConstants), __VA_ARGS__)
#define vkCmdBeginRendering(cb, info) (vk_one_primary_on ? vk_sections_begin(cb, info) : (vkCmdBeginRendering)(cb, info))
#define vkCmdEndRendering(cb) (vk_one_primary_on ? vk_sections_end(cb) : (vkCmdEndRendering)(cb))
#define vkCmdBindPipeline(cb, ...) vkCmdBindPipeline(vk_sections_command_kind(cb, VK_SECTION_BindPipeline), __VA_ARGS__)
#define vkCmdBindDescriptorSets(cb, ...) vkCmdBindDescriptorSets(vk_sections_command_kind(cb, VK_SECTION_BindDescriptorSets), __VA_ARGS__)
#define vkCmdBindVertexBuffers2(cb, ...) vkCmdBindVertexBuffers2(vk_sections_command_kind(cb, VK_SECTION_BindVertexBuffers2), __VA_ARGS__)
#define vkCmdBindIndexBuffer(cb, ...) vkCmdBindIndexBuffer(vk_sections_command_kind(cb, VK_SECTION_BindIndexBuffer), __VA_ARGS__)
#define vkCmdDraw(cb, ...) vkCmdDraw(vk_sections_command_kind(cb, VK_SECTION_Draw), __VA_ARGS__)
#define vkCmdDrawIndexed(cb, ...) vkCmdDrawIndexed(vk_sections_command_kind(cb, VK_SECTION_DrawIndexed), __VA_ARGS__)
#define vkCmdDispatch(cb, ...) vkCmdDispatch(vk_sections_command_kind(cb, VK_SECTION_Dispatch), __VA_ARGS__)
#define vkCmdSetViewport(cb, ...) vkCmdSetViewport(vk_sections_command_kind(cb, VK_SECTION_SetViewport), __VA_ARGS__)
#define vkCmdSetScissor(cb, ...) vkCmdSetScissor(vk_sections_command_kind(cb, VK_SECTION_SetScissor), __VA_ARGS__)
#define vkCmdSetDepthBias(cb, ...) vkCmdSetDepthBias(vk_sections_command_kind(cb, VK_SECTION_SetDepthBias), __VA_ARGS__)
#define vkCmdSetBlendConstants(cb, ...) vkCmdSetBlendConstants(vk_sections_command_kind(cb, VK_SECTION_SetBlendConstants), __VA_ARGS__)
#define vkCmdSetStencilCompareMask(cb, ...) vkCmdSetStencilCompareMask(vk_sections_command_kind(cb, VK_SECTION_SetStencilCompareMask), __VA_ARGS__)
#define vkCmdSetStencilWriteMask(cb, ...) vkCmdSetStencilWriteMask(vk_sections_command_kind(cb, VK_SECTION_SetStencilWriteMask), __VA_ARGS__)
#define vkCmdSetStencilReference(cb, ...) vkCmdSetStencilReference(vk_sections_command_kind(cb, VK_SECTION_SetStencilReference), __VA_ARGS__)
#define vkCmdSetCullMode(cb, ...) vkCmdSetCullMode(vk_sections_command_kind(cb, VK_SECTION_SetCullMode), __VA_ARGS__)
#define vkCmdSetFrontFace(cb, ...) vkCmdSetFrontFace(vk_sections_command_kind(cb, VK_SECTION_SetFrontFace), __VA_ARGS__)
#define vkCmdSetPrimitiveTopology(cb, ...) vkCmdSetPrimitiveTopology(vk_sections_command_kind(cb, VK_SECTION_SetPrimitiveTopology), __VA_ARGS__)
#define vkCmdSetDepthTestEnable(cb, ...) vkCmdSetDepthTestEnable(vk_sections_command_kind(cb, VK_SECTION_SetDepthTestEnable), __VA_ARGS__)
#define vkCmdSetDepthWriteEnable(cb, ...) vkCmdSetDepthWriteEnable(vk_sections_command_kind(cb, VK_SECTION_SetDepthWriteEnable), __VA_ARGS__)
#define vkCmdSetDepthCompareOp(cb, ...) vkCmdSetDepthCompareOp(vk_sections_command_kind(cb, VK_SECTION_SetDepthCompareOp), __VA_ARGS__)
#define vkCmdSetStencilTestEnable(cb, ...) vkCmdSetStencilTestEnable(vk_sections_command_kind(cb, VK_SECTION_SetStencilTestEnable), __VA_ARGS__)
#define vkCmdSetStencilOp(cb, ...) vkCmdSetStencilOp(vk_sections_command_kind(cb, VK_SECTION_SetStencilOp), __VA_ARGS__)
#define vkCmdSetDepthBiasEnable(cb, ...) vkCmdSetDepthBiasEnable(vk_sections_command_kind(cb, VK_SECTION_SetDepthBiasEnable), __VA_ARGS__)
#define vkCmdSetPrimitiveRestartEnable(cb, ...) vkCmdSetPrimitiveRestartEnable(vk_sections_command_kind(cb, VK_SECTION_SetPrimitiveRestartEnable), __VA_ARGS__)
#define vkCmdSetRasterizerDiscardEnable(cb, ...) vkCmdSetRasterizerDiscardEnable(vk_sections_command_kind(cb, VK_SECTION_SetRasterizerDiscardEnable), __VA_ARGS__)
#define vkCmdSetLineWidth(cb, ...) vkCmdSetLineWidth(vk_sections_command_kind(cb, VK_SECTION_SetLineWidth), __VA_ARGS__)
#define vkCreateImage(dev, info, alloc, image) (vk_one_primary_on ? vk_sections_create_image(dev, info, alloc, image) : (vkCreateImage)(dev, info, alloc, image))
#define vkDestroyImage(dev, image, alloc) (vk_one_primary_on ? vk_sections_destroy_image(dev, image, alloc) : (vkDestroyImage)(dev, image, alloc))
#define vkCreateImageView(dev, info, alloc, view) (vk_one_primary_on ? vk_sections_create_view(dev, info, alloc, view) : (vkCreateImageView)(dev, info, alloc, view))
#define vkDestroyImageView(dev, view, alloc) (vk_one_primary_on ? vk_sections_destroy_view(dev, view, alloc) : (vkDestroyImageView)(dev, view, alloc))
