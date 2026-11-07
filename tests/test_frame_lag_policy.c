/* Fresh-process observation of production startup policy: no Vulkan, game data or device.
   GPU selection is included verbatim from nvn_vk.c by test_frame_lag_policy.py. */
#include "../games/totk/runtime/frame_lag.h"
#include "../games/totk/android/gfx/vk_gpu_time.h"
#include <stdio.h>

static int g_gpu_time_hardware;
static int g_gpu_time_fps_median;
#define LOGV(...) do { printf(__VA_ARGS__); putchar('\n'); } while (0)
static void gpu_time_policy(void) {
#include "gpu_time_policy.inc"
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--gpu-time")) {
        gpu_time_policy();
        printf("GPU_TIME=%s\n", g_gpu_time_hardware ? "hardware" : "latency");
        return 0;
    }
    printf("FRAME_LAG=%d\n", runtime_frame_lag_requested());
    return 0;
}
