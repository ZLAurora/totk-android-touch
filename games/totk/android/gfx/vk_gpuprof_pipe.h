/* Pipeline sampling grows its power-of-two domain after observing a larger frame.
 * Uniform launch bits plus frame*k modulo the domain preserve k/N marginals;
 * systematic rotation is correlated, not IID. A growing frame's uncovered tail
 * remains explicit until the next frame can include it. */
#pragma once
#include <stdint.h>
#define VK_GPUPROF_PIPE_ORDINALS 2048u
#define VK_GPUPROF_PIPE_BUDGET 16u
static inline uint32_t vk_gpuprof_pipe_population(uint32_t previous, uint32_t observed) {
    while (previous < observed && previous < (UINT32_C(1) << 31)) previous <<= 1;
    return previous;
}
static inline int vk_gpuprof_pipe_selected(uint32_t ordinal, uint32_t phase, uint32_t budget, uint32_t population) {
    return population >= VK_GPUPROF_PIPE_ORDINALS && ordinal < population &&
        budget && budget <= VK_GPUPROF_PIPE_BUDGET &&
        ((ordinal + population - phase) & (population - 1)) < budget;
}
/* Counts submitted input primitives, not post-clipping/rasterized primitives.
 * Indexed restart strips cannot be counted without reading the index payload. */
static inline int vk_gpuprof_pipe_primitives(int prim, uint32_t count, uint32_t instances,
                                            int indexed, uint64_t *out) {
    if (indexed && (prim == 3 || prim == 5)) return 0;
    uint64_t n;
    switch (prim) {
        case 0: n = count; break;
        case 1: n = count / 2; break;
        case 3: n = count > 1 ? count - 1 : 0; break;
        case 5: case 6: n = count > 2 ? count - 2 : 0; break;
        case 7: n = indexed ? count / 3 : count / 4 * 2; break;
        default: n = count / 3; break;
    }
    *out = n * instances;
    return 1;
}
