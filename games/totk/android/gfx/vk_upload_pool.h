/* Parallel deswizzle for texture uploads (TOTK_VK_UPLOAD_THREADS, vk_res.c). No Vulkan: host-testable
   (test_upload_pool.c). One upload at a time owns the pool; helpers start on the first parallel upload and
   inherit that thread's nice and CPU affinity. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../../third_party/d2r_gfx/gfx/nvn/nvn_tex.h"

#define UPLOAD_POOL_MAX_HELPERS 3
#define UPLOAD_POOL_MIN_BYTES (512u << 10)   /* smaller uploads stay serial: waking helpers costs more (host break-even ~1 MiB) */
#define UPLOAD_POOL_RUN_BYTES (64u << 10)    /* runs are whole block rows of at least this much storage */

typedef struct { unsigned level, layer; void *dst; size_t pitch; } upload_sub;   /* one level/layer, as nvn_deswizzle */
typedef struct { uint64_t jobs, runs, helper_runs; } upload_pool_stats;

/* Launch-time helper count, clamped to 0..UPLOAD_POOL_MAX_HELPERS; 0 keeps every upload serial. Call before uploads. */
void upload_pool_configure(int helpers);
int upload_pool_helpers(void);
/* Decode every block-linear subresource of d from src into its dst, byte-identical to nvn_deswizzle per subresource.
   Splits into runs when helpers are configured and the subresources hold at least min_bytes of texels; the caller
   claims runs too and returns after all of them. 0, or -1 (nothing decoded) when nvn_deswizzle rejects a subresource. */
int upload_pool_deswizzle(const nvn_texdesc *d, const void *src, const upload_sub *subs, unsigned nsubs, size_t min_bytes);
void upload_pool_stats_take(upload_pool_stats *out);   /* since the last call; any thread */
