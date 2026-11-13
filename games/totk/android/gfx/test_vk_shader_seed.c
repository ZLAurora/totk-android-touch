/* No Vulkan device: exercise the production seed I/O and cache namespace. */
#include "vk_shader.c"
#include <assert.h>

int main(int argc, char **argv) {
    assert(argc == 2);
    const char *game_dir = getenv("TEST_GAME_DIR");
    if (game_dir) vk_shader_seed_startup(game_dir);
    if (getenv("TEST_POSITION_MUTATE")) {
        setenv("TOTK_VK_POS_INVARIANT", "0", 1);
        setenv("TOTK_VK_POS_NOCONTRACT", "0", 1);
        /* Every subsequent query must retain the startup arm and log only once. */
        setenv("TOTK_VK_INT_FOLD", g_int_fold ? "0" : "1", 1);
    }
    if (!strcmp(argv[1], "--unready")) {
        assert(!vk_shaders_ready()); assert(!vk_shaders_ready());
        puts(seed_dir()); return 0;
    }
    assert(vk_shaders_ready());
    char path[1024], cache[1100], prewarm[1100], imported[1100];
    snprintf(path, sizeof path, "%s/probe.spv", seed_dir());
    size_t n; char *data = read_file(path, &n);
    assert(data && n == strlen(argv[1]) && !memcmp(data, argv[1], n)); free(data);
    if (vk_seed_io_active()) { vk_seed_io_stats stats; vk_seed_io_stats_get(&stats); assert(stats.entries == 1); }
    assert(vk_shader_cache_path("state/vk-pipeline-cache.bin", cache, sizeof cache));
    assert(vk_shader_cache_path("state/vk-prewarm.bin", prewarm, sizeof prewarm));
    assert(vk_shader_cache_path("custom-import", imported, sizeof imported));
    assert(!vk_shader_cache_path("0", path, sizeof path));
    assert(!vk_shader_cache_path("oversize", path, 4));
    puts(seed_dir()); puts(cache); puts(prewarm); puts(imported);
    uint32_t w[] = {0x07230203, 0x00010300, 0, 1, 0, 2u << 16 | 17, 55, 2u << 16 | 17, 56};
    assert(spirv_image_features_ok(w, 9, 1, 1));
    assert(!spirv_image_features_ok(w, 9, 0, 1));
    assert(!spirv_image_features_ok(w, 9, 1, 0));
    assert(spirv_image_features_ok(w, 5, 0, 0));
    assert(!spirv_image_features_ok(w, 8, 1, 1));
    puts("seed paths and formatless image capability checks passed");
    return 0;
}
