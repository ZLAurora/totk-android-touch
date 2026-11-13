/* No Vulkan device: production shader/program caches and compact dictionary,
 * with only driver calls and unrelated renderer classification replaced. */
#include "vk_shader.c"
#include "vk_shader_xlate.c"
#include "vk_gpuprof_compact.h"
#include <assert.h>

vk_dev V;
vk_state S;
int g_hitch_on;
vk_hitch g_hk;
static unsigned modules, destroyed, layouts, compute_prepares;
static int fail_module, fail_layout;

static VKAPI_ATTR VkResult VKAPI_CALL create_module(VkDevice device,
        const VkShaderModuleCreateInfo *info, const VkAllocationCallbacks *allocator, VkShaderModule *out) {
    (void)device; (void)allocator;
    assert(info->codeSize >= 20 && info->pCode[0] == 0x07230203);
    if (fail_module) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *out = (VkShaderModule)(uintptr_t)++modules;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL destroy_module(VkDevice device, VkShaderModule module,
        const VkAllocationCallbacks *allocator) {
    (void)device; (void)allocator;
    assert(module); destroyed++;
}
static VKAPI_ATTR VkResult VKAPI_CALL create_layout(VkDevice device,
        const VkDescriptorSetLayoutCreateInfo *info, const VkAllocationCallbacks *allocator,
        VkDescriptorSetLayout *out) {
    (void)device; (void)allocator;
    assert(!info->bindingCount);
    if (fail_layout) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *out = (VkDescriptorSetLayout)(uintptr_t)++layouts;
    return VK_SUCCESS;
}
PFN_vkCreateShaderModule vkCreateShaderModule = create_module;
PFN_vkDestroyShaderModule vkDestroyShaderModule = destroy_module;
PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout = create_layout;

int vk_off(const char *name) { (void)name; return 0; }
int vk_unwritten_mask_on(void) { return 0; }
int vk_native_ui_enabled(void) { return 0; }
int vk_canvas_ui_shader(const char *key, int after) { (void)key; (void)after; return 0; }
int vk_canvas_transfer_shader(const char *key) { (void)key; return 0; }
unsigned vk_fg_camera_layout(const char *key) { (void)key; return 0; }
int vk_fg_raster_depth_shader(const char *key) { (void)key; return 0; }
int vk_fg_plane_shader(const char *key, unsigned stage) { (void)key; (void)stage; return 0; }
int vk_treg_kind(int tid, int *format, int *target) { (void)tid; (void)format; (void)target; return 0; }
uint64_t vk_treg_revision(void) { return 1; }
int vk_fmt_class(int format) { (void)format; return 0; }
int vk_desc_layout_fits(const VkDescriptorSetLayoutBinding *bindings, uint32_t count,
        char *why, size_t size) {
    (void)bindings; (void)why; (void)size;
    assert(!count); return 1;
}
void vk_compute_prepare(vk_shader *shader) {
    shader_key key = {0}; memcpy(key.variant, shader->key, sizeof key.variant);
    key.early_tests = shader->early_tests | shader->alpha_func << 1;
    key.position_policy = g_position_policy;
    assert(shader->stage == 5 && shader->mod && shader->dsl);
    assert(nvn_hm_get(&g_shaders, &key) == shader); /* publish before handing to a worker */
    compute_prepares++;
}

/* A small empty entry point is enough for the real loader's parsing/patching.
 * The mock driver deliberately does not validate rendering semantics. */
static void seed_variant(const char *name, int stage) {
    const uint32_t model = stage == 0 ? 0 : stage == 1 ? 4 : 5;
    const uint32_t head[] = {0x07230203, 0x00010300, 0, 5, 0,
        2u << 16 | 17, 1, 3u << 16 | 14, 0, 1,
        5u << 16 | 15, model, 3, 0x6e69616d, 0};
    const uint32_t fragment[] = {3u << 16 | 16, 3, 7};
    const uint32_t compute[] = {6u << 16 | 16, 3, 17, 1, 1, 1};
    const uint32_t body[] = {2u << 16 | 19, 1, 3u << 16 | 33, 2, 1,
        5u << 16 | 54, 1, 3, 0, 2, 2u << 16 | 248, 4, 1u << 16 | 253, 1u << 16 | 56};
    char path[1100]; snprintf(path, sizeof path, "%s/%s.spv", seed_dir(), name);
    FILE *file = fopen(path, "wb"); assert(file);
    assert(fwrite(head, 1, sizeof head, file) == sizeof head);
    if (stage == 1) assert(fwrite(fragment, 1, sizeof fragment, file) == sizeof fragment);
    if (stage == 5) assert(fwrite(compute, 1, sizeof compute, file) == sizeof compute);
    assert(fwrite(body, 1, sizeof body, file) == sizeof body); assert(!fclose(file));
    snprintf(path, sizeof path, "%s/%s.vk", seed_dir(), name);
    file = fopen(path, "w"); assert(file);
    assert(fprintf(file, "stage %d\nc1 01234567\n", stage) > 0); assert(!fclose(file));
}
static void variant_list_write(const char *key, const char *name) {
    char path[1100]; snprintf(path, sizeof path, "%s/%.64s.vv", seed_dir(), key);
    FILE *file = fopen(path, "w"); assert(file);
    assert(fprintf(file, "%s\t\n", name) > 0); assert(!fclose(file));
}
static gp_compact compact;

int main(void) {
    /* Freeze precision to the ordinary arm before the production pthread_once. */
    assert(!setenv("TOTK_VK_POS_INVARIANT", "0", 1));
    assert(!setenv("TOTK_VK_POS_NOCONTRACT", "0", 1));
    assert(!setenv("TOTK_XLATE_ONDEVICE", "0", 1));
    assert(!mkdir("shaders", 0700));
    uint8_t control[0x880] = {0}, code[64] = {0};
    uint32_t length = sizeof code; memcpy(control + 0x704, &length, sizeof length);
    prog_t program = {.serial = 71, .nstage = 1};
    program.sd[0].control = control; program.sd[0].data = (uintptr_t)code;
    vk_prog *pr = prog_of(&program); assert(pr && pr->has[0]);
    char first[80], later[80], fragment[65], compute[65], failure[65];
    snprintf(first, sizeof first, "%.64s-000000000001", pr->key[0]);
    snprintf(later, sizeof later, "%.64s-000000000002", pr->key[0]);
    memset(fragment, 'b', 64); fragment[64] = 0;
    memset(compute, 'c', 64); compute[64] = 0;
    memset(failure, 'd', 64); failure[64] = 0;
    seed_variant(first, 0); seed_variant(fragment, 1); seed_variant(compute, 5); seed_variant(failure, 0);
    variant_list_write(pr->key[0], first);
    char game[4096]; assert(getcwd(game, sizeof game)); vk_shader_seed_startup(game);
    int indexed = vk_seed_io_active();
    const char *io = getenv("TOTK_VK_SEED_IO");
    assert(indexed == (!io || atoi(io) != 0));
    vk_shader *bound[6]; assert(!vk_prog_resolve(&program, bound) && bound[0]);
    vk_shader *vs = bound[0], *fs = vk_shader_seed(fragment, 1, 0);
    vk_shader *early = vk_shader_seed(fragment, 1, 1), *cs = vk_shader_seed(compute, 5, 0);
    assert(fs && early && early != fs && cs && compute_prepares == 1);
    variant_list *list = variants_of(pr->key[0]); assert(list && list->n == 1);
    assert(vs->key != list->v[0].name && !strcmp(vs->key, list->v[0].name));
    gp_compact_key pending = {.pipeline = 0x100, .shader = {vs->key, fs->key, NULL}};
    gp_compact_key pending_compute = {.pipeline = 0x200, .compute = 1, .shader = {NULL, NULL, cs->key}};
    compact.start = 1;
    gp_compact_entry *original = gp_compact_get(&compact, &pending); assert(original);
    uint64_t original_id = original->id;
    unsigned capacity = g_shaders.cap, published = g_shaders.n;

    /* Failure cleanup must never publish a partial object or affect earlier names. */
    fail_module = 1; assert(!vk_shader_seed(failure, 0, 0));
    assert(g_shaders.n == published && !destroyed);
    fail_module = 0; fail_layout = 1; assert(!vk_shader_seed(failure, 0, 0));
    assert(g_shaders.n == published && destroyed == 1);
    fail_layout = 0; assert(vk_shader_seed(failure, 0, 0));
    assert(!vk_shader_alpha(fs, NVN_ALPHA_FUNC_LESS)); /* empty fragment has no colour-zero output */
    assert(g_shaders.n == published + 1);

    /* Model a successful service reply using its actual index-before-epoch protocol.
     * Refresh frees the old variant-list strings, not the published vk_shader. */
    seed_variant(later, 0); variant_list_write(pr->key[0], later);
    vk_seed_io_published(later);
    atomic_fetch_add_explicit(&xlate_epoch, 1, memory_order_release);
    list = variants_of(pr->key[0]); assert(list->n == 1 && !strcmp(list->v[0].name, later));
    assert(!vk_prog_resolve(&program, bound) && bound[0] == vs); /* ready variants survive an epoch */
    vk_shader *new_vs = vk_shader_seed(list->v[0].name, 0, 0); assert(new_vs && new_vs != vs);
    for (unsigned i = 0; i < 192; i++) {
        char name[80]; snprintf(name, sizeof name, "%.64s-%012x", pr->key[0], i + 16);
        seed_variant(name, 0); vk_seed_io_published(name);
        vk_shader *loaded = vk_shader_seed(name, 0, 0);
        assert(loaded && !strcmp(loaded->key, name));
        memset(name, 'x', sizeof name); /* caller scratch never owns the cached name */
    }
    assert(g_shaders.cap > capacity);
    mtl_prog_release(&program);
    assert(!nvn_hm_get(&g_progs, &program.serial));
    vk_seed_io_stop(); assert(!vk_seed_io_active()); /* frees index/warmed file buffers */

    /* A delayed lease/dictionary consumer still sees exact names after all churn. */
    assert(pending.shader[0] == vs->key && !strcmp(pending.shader[0], first));
    assert(!strcmp(pending.shader[1], fragment) && !strcmp(pending_compute.shader[2], compute));
    assert(vk_shader_seed(first, 0, 0) == vs && vk_shader_seed(compute, 5, 0) == cs);
    assert(compute_prepares == 1);
    assert(gp_compact_get(&compact, &pending)->id == original_id);
    gp_compact_key same_text = pending; same_text.shader[1] = early->key;
    assert(early->key != fs->key && !strcmp(early->key, fs->key));
    assert(gp_compact_get(&compact, &same_text)->id == original_id);
    gp_compact_key reused_handle = pending; reused_handle.shader[0] = new_vs->key;
    gp_compact_entry *replacement = gp_compact_get(&compact, &reused_handle);
    assert(replacement && replacement->id != original_id);
    assert(!strcmp(original->shader[0], first) && !strcmp(replacement->shader[0], later));
    gp_compact_entry *compute_entry = gp_compact_get(&compact, &pending_compute);
    assert(compute_entry && !strcmp(compute_entry->shader[2], compute));
    assert(destroyed == 1); /* only the failed, unpublished layout's module was destroyed */
    puts("borrowed shader lifetime passed");
    return 0;
}
