/* Shaders of the Vulkan backend (vk_int.h): the shader seed on the device, variants per bind state, SPIR-V modules,
   descriptor set layouts, pipeline layouts and compute pipelines.

   The seed (android/build_android.py --shaders) holds every shader of the game translated on the PC for its default
   bind state and for every bind state of the shared shader list, as on the iPhone:
     <variant>.spv   Vulkan SPIR-V from the Ryujinx translator (tools/totk_xlate)
     <variant>.vk    its Vulkan reflection (android/vk_reflect.py)
     <key>.vv        the shader's variants with the answers each was translated for
   A shader is identified as on the Mac: SHA-256 of its control section (ctl_key_len) and code. A variant is chosen by
   the same answers (texture slot types, image formats, vertex input and colour output classes) and the same per-stage
   signature as nvn_mtl.m prog_fit / stage_sig. A variant the seed lacks skips its draws and is recorded once under
   shader-requests/ for a later fill on the PC. If packaged, the low-priority on-device service also translates it;
   a publication epoch invalidates negative caches on the recording thread and the existing pipeline path takes over.

   Every stage of a translation uses descriptor set 0; the vertex (or compute) stage keeps it and the fragment stage's
   SPIR-V is patched to set 1, so one pipeline layout holds both. */
#include "vk_int.h"
#include "vk_alpha_test.h"
#include "vk_record.h"
#include "vk_framegen.h"
#include "vk_earlyz.h"
#include "nvn_hmap.h"
#include "vk_shader_xlate.h"
#include "vk_seed_io.h"
#include "vk_load_proof.h"
#include "vk_subgroup.h"
#include "vk_position.h"
#include <errno.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdatomic.h>

#include "host_os.h"
int totk_depth_near_minus_w(void) __attribute__((weak));

static pthread_once_t g_position_once = PTHREAD_ONCE_INIT;
static unsigned g_position_policy;
static void position_init(void) {
    g_position_policy = vk_position_policy(getenv("TOTK_VK_POS_INVARIANT"), getenv("TOTK_VK_POS_NOCONTRACT"));
    LOGV("[nvn-vk] TOTK_VK_POS_INVARIANT=%u (startup-only): vertex Position Invariant", g_position_policy & 1);
    LOGV("[nvn-vk] TOTK_VK_POS_NOCONTRACT=%u (startup-only): position-feeding float arithmetic NoContraction", g_position_policy >> 1);
}

/* Folder selection is process-lifetime. Preserve legacy cache paths for the
   canonical game seed; only alternate folders use a folder/content namespace. */
static pthread_once_t g_seed_once = PTHREAD_ONCE_INIT;
static const char *g_seed_dir;
static char g_seed_tag[65];
static int g_seed_default;
static char g_seed_base[4096], g_seed_path[4096];
static void seed_init(void) {
    const char *selected = getenv("TOTK_SHADER_SEED_DIR");
    if (!selected || !*selected) { selected = "shaders"; g_seed_default = 1; }
    if (!g_seed_base[0] && !getcwd(g_seed_base, sizeof g_seed_base)) {
        g_seed_dir = "(game folder unavailable)"; return;
    }
    int n = selected[0] == '/' ? snprintf(g_seed_path, sizeof g_seed_path, "%s", selected) :
        snprintf(g_seed_path, sizeof g_seed_path, "%s/%s", g_seed_base, selected);
    if (n < 0 || (size_t)n >= sizeof g_seed_path) { g_seed_dir = "(shader seed path too long)"; return; }
    char canonical[4096], standard[4096], marker[4096], id[65] = {0};
    if (realpath(g_seed_path, canonical)) snprintf(g_seed_path, sizeof g_seed_path, "%s", canonical);
    g_seed_dir = g_seed_path;
#if defined(TOTK_GAME_V143)
    const char *versioned_default = "shaders/v143";
#else
    const char *versioned_default = "shaders/v0";
#endif
    const char *defaults[] = {"shaders", versioned_default};
    for (unsigned i = 0; i < sizeof defaults / sizeof *defaults; i++) {
        n = snprintf(standard, sizeof standard, "%s/%s", g_seed_base, defaults[i]);
        if (n >= 0 && (size_t)n < sizeof standard &&
            !strcmp(g_seed_dir, realpath(standard, canonical) ? canonical : standard)) g_seed_default = 1;
    }
    if (g_seed_default) return;
    if (snprintf(marker, sizeof marker, "%s/seed-id.txt", g_seed_dir) < (int)sizeof marker) {
        FILE *f = fopen(marker, "rb");
        if (f) { size_t size = fread(id, 1, 64, f); fclose(f); if (size != 64) memset(id, 0, sizeof id); }
    }
    host_sha256_ctx ctx; unsigned char digest[32];
    host_sha256_init(&ctx); host_sha256_update(&ctx, g_seed_dir, strlen(g_seed_dir) + 1);
    host_sha256_update(&ctx, id, sizeof id); host_sha256_final(&ctx, digest);
    for (int i = 0; i < 32; i++) snprintf(g_seed_tag + 2 * i, 3, "%02x", digest[i]);
}
static const char *seed_dir(void) { pthread_once(&g_seed_once, seed_init); return g_seed_dir; }
/* Alternate seeds and position precision arms namespace explicit cache/import
   paths too. Off/off with the canonical seed retains legacy paths. Caller owns out. */
const char *vk_shader_cache_path(const char *base, char *out, size_t size) {
    pthread_once(&g_seed_once, seed_init);
    pthread_once(&g_position_once, position_init);
    if (!base || !strcmp(base, "0")) return NULL;
    const char *suffix = vk_position_cache_suffix(g_position_policy);
    int n = g_seed_default ? snprintf(out, size, "%s%s", base, suffix) : snprintf(out, size, "%s.%s%s", base, g_seed_tag, suffix);
    return n >= 0 && (size_t)n < size ? out : NULL;
}
static void seed_io_start(void) {
    char path[1100];
    mkdir("state", 0700);
    vk_seed_io_start(seed_dir(), vk_shader_cache_path("state/vk-seed-used.txt", path, sizeof path));
}
static int g_seed_ok = -1;
int vk_shaders_ready(void) {
    if (g_seed_ok < 0) {
        if (vk_seed_io_enabled()) {
            seed_io_start();
            if (vk_seed_io_active()) {
                g_seed_ok = vk_seed_io_ready();
                if (!g_seed_ok) LOGV("【红】shader seed error: %s: indexed seed contains no .spv modules; nothing is drawn", seed_dir());
                else LOGV("shader seed: %s (indexed IO)", seed_dir());
                return g_seed_ok;
            }
        }
        const char *path = seed_dir(); DIR *dir = opendir(path); int opened = dir != NULL; g_seed_ok = 0;
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir))) {
                size_t n = strlen(entry->d_name); struct stat st;
                if (n > 4 && !strcmp(entry->d_name + n - 4, ".spv") &&
                    !fstatat(dirfd(dir), entry->d_name, &st, 0) && S_ISREG(st.st_mode)) { g_seed_ok = 1; break; }
            }
            closedir(dir);
        }
        if (!g_seed_ok) LOGV("【红】shader seed error: %s: %s; nothing is drawn", path,
            opened ? "no .spv modules" : "directory missing or inaccessible");
        else LOGV("shader seed: %s", path);
    }
    return g_seed_ok;
}
/* Frontend calls before renderer startup: relative selection must not depend on
   a later process-wide chdir by the guest/runtime. Keep this base process-owned. */
void vk_shader_seed_startup(const char *game_dir) {
    pthread_once(&g_position_once, position_init);
    if (!g_seed_dir && !realpath(game_dir, g_seed_base))
        snprintf(g_seed_base, sizeof g_seed_base, "%s", game_dir);
    if (vk_seed_io_enabled()) seed_io_start(); /* index/warming overlaps remaining boot work */
    else (void)vk_shaders_ready();
}
static uint64_t g_io_n, g_io_ns, g_io_max_ns;   /* seed files read since the last vk_shader_io_take (the stutter log) */
static _Atomic uint64_t g_io_cum_ns;            /* all seed reads (the long-frame log) */
void vk_shader_io_take(uint64_t *n, uint64_t *ns, uint64_t *max_ns) { *n = g_io_n; *ns = g_io_ns; *max_ns = g_io_max_ns; g_io_n = g_io_ns = g_io_max_ns = 0; vk_seed_io_report(); vk_compute_report(); }
uint64_t vk_shader_io_cum_ns(void) { return atomic_load(&g_io_cum_ns); }
static char *read_file_(const char *path, size_t *len);
static char *read_file(const char *path, size_t *len) {
    uint64_t t0 = host_now_ns();
    const char *name = strrchr(path, '/'); if (name) name++; else name = path;
    char *r = vk_seed_io_active() ? vk_seed_io_read(name, len) : read_file_(path, len);
    uint64_t dt = host_now_ns() - t0;
    g_io_n++; g_io_ns += dt; g_io_cum_ns += dt; if (dt > g_io_max_ns) g_io_max_ns = dt;
    return r;
}
static char *read_file_(const char *path, size_t *len) {
    return vk_seed_file_read(path, len);
}

/* ---------- answers (nvn_mtl.m rt_answers) as flat "kind:slot=value" items ---------- */
static const char *target_name(int t) {
    switch (t) { case 0: return "1d"; case 2: return "3d"; case 3: return "1darray"; case 4: return "2darray"; case 5: return "2dms";
                 case 6: return "2dmsarray"; case 8: return "cube"; case 9: return "cubearray"; case 10: return "buffer"; default: return "2d"; }
}
static const char *image_format(int f) {
    switch (f) {
        case 0x01: return "R8Unorm"; case 0x0D: return "R8G8Unorm"; case 0x25: return "R8G8B8A8Unorm"; case 0x27: return "R8G8B8A8Uint";
        case 0x05: return "R16Float"; case 0x11: return "R16G16Float"; case 0x29: return "R16G16B16A16Float"; case 0x08: return "R16Uint"; case 0x2C: return "R16G16B16A16Uint";
        case 0x0A: return "R32Float"; case 0x16: return "R32G32Float"; case 0x2E: return "R32G32B32A32Float"; case 0x0B: return "R32Uint"; case 0x0C: return "R32Sint";
        case 0x17: return "R32G32Uint"; case 0x2F: return "R32G32B32A32Uint"; case 0x3D: return "R10G10B10A2Unorm"; case 0x3F: return "R11G11B10Float";
    }
    return NULL;
}
static const char *class_name(int f) { int c = vk_fmt_class(f); return c == 1 ? "uint" : c == 2 ? "sint" : "float"; }
typedef struct { char tex[128][24]; char img[32][48]; char attr[16][8]; char fout[8][8]; uint8_t has_tex[128], has_img[32], has_attr[16], has_fout[8]; } answers_t;
static void answers_of(int st, answers_t *a) {
    memset(a, 0, sizeof *a);
    for (int r = 0; r < 128; r++) if (S.tex[st][r]) {
        int f, tg; if (!vk_treg_kind((int)(S.tex[st][r] & 0xfffff), &f, &tg)) continue;
        snprintf(a->tex[r], sizeof a->tex[r], "%s%s", target_name(tg), f >= 0x32 && f <= 0x36 ? "_depth" : ""); a->has_tex[r] = 1;
    }
    for (int r = 0; r < 32; r++) if (S.img[st][r]) {
        int f, tg; if (!vk_treg_kind((int)(S.img[st][r] & 0xfffff), &f, &tg)) continue;
        const char *fn = image_format(f);
        snprintf(a->img[r], sizeof a->img[r], "%s%s%s", target_name(tg), fn ? "/" : "", fn ? fn : ""); a->has_img[r] = 1;
    }
    if (st == 0) for (int i = 0; i < S.nattr && i < 16; i++) { snprintf(a->attr[i], sizeof a->attr[i], "%s", class_name(S.attr[i].fmt)); a->has_attr[i] = 1; }
    if (st == 1 && S.has_rt) for (int i = 0; i < S.rt.n && i < 8; i++) if (S.rt.color[i]) {
        int f = S.rt.has_cview[i] && S.rt.cview[i].set_format ? S.rt.cview[i].format : S.rt.color[i]->d.format;
        snprintf(a->fout[i], sizeof a->fout[i], "%s", class_name(f)); a->has_fout[i] = 1;
    }
}
/* one flat dependency "kind:slot=value" against the answers (deps_match: a missing answer is the default) */
static int dep_ok(const answers_t *a, const char *kind, int slot, const char *want) {
    const char *have;
    if (!strcmp(kind, "tex")) {
        have = slot >= 0 && slot < 128 && a->has_tex[slot] ? a->tex[slot] : "2d";
        /* Shared-list variants can bind a texture without querying its dimension.
           Those depend only on depth class, not on the concrete target kind. */
        if (!strcmp(want, "*") || !strcmp(want, "*_depth")) {
            size_t n = strlen(have);
            int depth = n >= 6 && !strcmp(have + n - 6, "_depth");
            return depth == !strcmp(want, "*_depth");
        }
    }
    else if (!strcmp(kind, "img")) have = slot >= 0 && slot < 32 && a->has_img[slot] ? a->img[slot] : "";
    else if (!strcmp(kind, "attr")) have = slot >= 0 && slot < 16 && a->has_attr[slot] ? a->attr[slot] : "float";
    else if (!strcmp(kind, "fout")) have = slot >= 0 && slot < 8 && a->has_fout[slot] ? a->fout[slot] : "float";
    else return 0;
    return !strcmp(have, want);
}
static int deps_match(const char *deps, const answers_t *a) {
    char buf[4096]; snprintf(buf, sizeof buf, "%s", deps);
    for (char *save = NULL, *it = strtok_r(buf, ";", &save); it; it = strtok_r(NULL, ";", &save)) {
        char *colon = strchr(it, ':'), *eq = colon ? strchr(colon, '=') : NULL;
        if (!colon || !eq) return 0;
        *colon = 0; *eq = 0;
        if (!dep_ok(a, it, atoi(colon + 1), eq + 1)) return 0;
    }
    return 1;
}

/* ---------- shaders ---------- */
typedef struct { char variant[80]; uint32_t early_tests, position_policy; } shader_key;
/* Published objects and their inline key never move or die. Variant-list refresh,
   program release and seed-IO shutdown do not own them; pipeline workers and the
   asynchronous profiler borrow them for the process lifetime. */
static nvn_hmap g_shaders = {.klen = sizeof(shader_key)};
static int earlyz_on(void) { static int v = -1; if (v < 0) v = vk_earlyz_enabled(getenv("TOTK_VK_EARLYZ")); return v; }
static inline uint32_t ctl_key_len(const uint8_t *control) { uint32_t n; memcpy(&n, control + 0x20, 4); return n >= 0x7cc && n < 0x880 ? n : 0x880; }
static VkShaderStageFlags stage_flags(int st) { return st == 0 ? VK_SHADER_STAGE_VERTEX_BIT : st == 1 ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_COMPUTE_BIT; }
static void patch_set(uint32_t *w, size_t nw, uint32_t set) {
    for (size_t i = 5; i < nw; ) {
        uint32_t op = w[i] & 0xffff, n = w[i] >> 16; if (!n) break;
        if (op == 71 && n == 4 && w[i + 2] == 34) w[i + 3] = set;   /* OpDecorate <id> DescriptorSet <set> */
        i += n;
    }
}
/* Storage bindings the shader writes (TOTK, 2026-10-22). The reflection's rw was never set for storage buffers (the
   translator tested the wrong flag bit; nvn_mtl.m msl_stores_through found the same on the Mac), so every storage
   buffer counted as read-only: no "GPU writes here" mark, and uniform snapshots taken at submit kept the CPU's older
   bytes of buffers the GPU writes. The material shaders store a per-tile class flag that the next frame's class marker
   reads to decide which lighting draws light a pixel. Here the SPIR-V itself is read: a binding is rw when an OpStore,
   OpCopyMemory target, atomic or function-call argument reaches it through access chains. */
static int spirv_mark_stores(vk_shader *sh, const uint32_t *w, size_t nw) {
    static int on = -1; if (on < 0) on = !vk_off("storagewrites"); if (!on) return 0;
    if (nw < 5 || w[0] != 0x07230203) return 0;
    uint32_t bound = w[3]; if (!bound || bound > (1u << 22)) return 0;
    int32_t *bind = malloc(bound * sizeof *bind); uint32_t *root = malloc(bound * sizeof *root); uint8_t *isbuf = calloc(bound, 1), *stored = calloc(bound, 1);
    if (!bind || !root || !isbuf || !stored) { free(bind); free(root); free(isbuf); free(stored); return 0; }
    for (uint32_t i = 0; i < bound; i++) { bind[i] = -1; root[i] = 0; }
    #define ID_OK(x) ((x) && (x) < bound)
    for (size_t i = 5; i < nw; ) {
        uint32_t op = w[i] & 0xffff, n = w[i] >> 16; if (!n || i + n > nw) break;
        const uint32_t *a = w + i + 1;
        switch (op) {
            case 71: if (n >= 4 && a[1] == 33 && ID_OK(a[0])) bind[a[0]] = (int32_t)a[2]; break;   /* OpDecorate Binding */
            case 59: if (n >= 4 && ID_OK(a[1]) && (a[2] == 12 || a[2] == 2)) { isbuf[a[1]] = 1; root[a[1]] = a[1]; } break;   /* OpVariable StorageBuffer / Uniform */
            case 65: case 66: case 67: case 70: case 83: case 124:   /* access chains, OpCopyObject, OpBitcast */
                if (n >= 4 && ID_OK(a[1]) && ID_OK(a[2]) && root[a[2]]) root[a[1]] = root[a[2]]; break;
            case 62: case 63: if (n >= 3 && ID_OK(a[0]) && root[a[0]]) stored[root[a[0]]] = 1; break;   /* OpStore, OpCopyMemory target */
            case 228: if (n >= 2 && ID_OK(a[0]) && root[a[0]]) stored[root[a[0]]] = 1; break;   /* OpAtomicStore */
            case 57: for (uint32_t k = 3; k + 1 < n; k++) if (ID_OK(a[k]) && root[a[k]]) stored[root[a[k]]] = 1; break;   /* OpFunctionCall: a pointer argument may be written */
            default:
                if (((op >= 229 && op <= 242) || op == 6035 || op == 5614 || op == 5615) && n >= 4 && ID_OK(a[2]) && root[a[2]]) stored[root[a[2]]] = 1;   /* atomics */
                break;
        }
        i += n;
    }
    #undef ID_OK
    int marked = 0;
    for (uint32_t v = 1; v < bound; v++) if (isbuf[v] && stored[v] && bind[v] >= 0)
        for (int k = 0; k < sh->nb; k++) if (sh->b[k].binding == (uint16_t)bind[v] && !sh->b[k].rw && (sh->b[k].dtype == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || sh->b[k].dtype == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)) { sh->b[k].rw = 1; marked++; }
    free(bind); free(root); free(isbuf); free(stored);
    return marked;
}
/* Colour locations of the fragment shader's Output variables, arrays included: the seed's fout line names each
   variable's first location only. TOTK_VK_UNWRITTEN_MASK masks attachments outside this set, so it must not miss one. */
static uint32_t spirv_fout_locations(const uint32_t *w, size_t nw) {
    if (nw < 5 || w[0] != 0x07230203) return 0;
    uint32_t bound = w[3]; if (!bound || bound > (1u << 22)) return 0;
    int32_t *location = malloc(bound * sizeof *location); uint32_t *value = calloc(bound, sizeof *value), *count = calloc(bound, sizeof *count), *pointee = calloc(bound, sizeof *pointee);
    if (!location || !value || !count || !pointee) { free(location); free(value); free(count); free(pointee); return 0; }
    for (uint32_t i = 0; i < bound; i++) location[i] = -1;
    uint32_t mask = 0;
    #define ID_OK(x) ((x) && (x) < bound)
    for (size_t i = 5; i < nw; ) {
        uint32_t op = w[i] & 0xffff, n = w[i] >> 16; if (!n || i + n > nw) break;
        const uint32_t *a = w + i + 1;
        switch (op) {
            case 71: if (n >= 4 && a[1] == 30 && ID_OK(a[0])) location[a[0]] = (int32_t)a[2]; break;   /* OpDecorate Location */
            case 43: if (n >= 4 && ID_OK(a[1])) value[a[1]] = a[2]; break;                              /* OpConstant (low word) */
            case 28: if (n >= 4 && ID_OK(a[0]) && ID_OK(a[2]))                                          /* OpTypeArray */
                         count[a[0]] = value[a[2]] * (ID_OK(a[1]) && count[a[1]] ? count[a[1]] : 1); break;
            case 32: if (n >= 4 && ID_OK(a[0])) pointee[a[0]] = a[2]; break;                            /* OpTypePointer */
            case 59:                                                                                    /* OpVariable Output */
                if (n >= 4 && a[2] == 3 && ID_OK(a[0]) && ID_OK(a[1]) && location[a[1]] >= 0) {
                    uint32_t t = pointee[a[0]], span = ID_OK(t) && count[t] ? count[t] : 1;
                    for (uint32_t k = 0; k < span && location[a[1]] + k < 8; k++) mask |= 1u << (location[a[1]] + k);
                }
                break;
        }
        i += n;
    }
    #undef ID_OK
    free(location); free(value); free(count); free(pointee);
    return mask;
}
/* DEPTH16 nudge for depth the fragment shader writes (TOTK, 2026-10-22; vk_draw.c d16tie_on). Every store to the
   FragDepth built-in becomes value + a float specialization constant (id D16_SPEC_ID, default 0), set to the nudge in
   pipelines that draw into a DEPTH16 target. Returns a new word array (caller frees) or NULL when nothing to patch. */
static uint32_t *spirv_depth_nudge(const uint32_t *w, size_t nw, size_t *out_nw) {
    if (nw < 5 || w[0] != 0x07230203) return NULL;
    uint32_t bound = w[3], fragdepth = 0, tfloat = 0; size_t after_float = 0, first_dec = 0; int stores = 0;
    for (size_t i = 5; i < nw; ) {
        uint32_t op = w[i] & 0xffff, n = w[i] >> 16; if (!n || i + n > nw) return NULL;
        if (op == 71 && !first_dec) first_dec = i;
        if (op == 71 && n >= 4 && w[i + 2] == 11 && w[i + 3] == 22) fragdepth = w[i + 1];   /* BuiltIn FragDepth */
        if (op == 22 && n >= 3 && w[i + 2] == 32 && !tfloat) { tfloat = w[i + 1]; after_float = i + n; }   /* OpTypeFloat 32 */
        i += n;
    }
    if (!fragdepth || !tfloat || !first_dec) return NULL;
    for (size_t i = 5; i < nw; ) { uint32_t op = w[i] & 0xffff, n = w[i] >> 16; if (op == 62 && n >= 3 && w[i + 1] == fragdepth) stores++; i += n; }
    if (!stores) return NULL;
    uint32_t *o = malloc((nw + 4 + 4 + (size_t)stores * 5) * sizeof *o); size_t k = 0;
    uint32_t spec = bound++;
    for (size_t i = 0; i < 5; i++) o[k++] = w[i];
    for (size_t i = 5; i < nw; ) {
        uint32_t op = w[i] & 0xffff, n = w[i] >> 16;
        if (i == first_dec) { o[k++] = 4u << 16 | 71; o[k++] = spec; o[k++] = 1; o[k++] = D16_SPEC_ID; }   /* OpDecorate spec SpecId */
        if (op == 62 && n >= 3 && w[i + 1] == fragdepth) {
            uint32_t t = bound++;
            o[k++] = 5u << 16 | 129; o[k++] = tfloat; o[k++] = t; o[k++] = w[i + 2]; o[k++] = spec;   /* OpFAdd */
            o[k++] = w[i]; o[k++] = w[i + 1]; o[k++] = t; for (uint32_t j = 3; j < n; j++) o[k++] = w[i + j];   /* OpStore FragDepth t */
        } else for (uint32_t j = 0; j < n; j++) o[k++] = w[i + j];
        i += n;
        if (i == after_float) { o[k++] = 4u << 16 | 50; o[k++] = tfloat; o[k++] = spec; o[k++] = 0; }   /* OpSpecConstant float 0.0 */
    }
    o[3] = bound; *out_nw = k;
    return o;
}
/* The translator declares FragCoord even for shaders that only read w for
   interpolation. Only xy (or an unclassified vector/alias use) requires 1x.
   Pointer provenance covers direct/access-chain/copy aliases; unknown uses of
   the builtin fail closed. No seed/reflection format change is required. */
static int spirv_fragcoord_xy(const uint32_t *w, size_t nw) {
    if (nw < 5 || w[0] != 0x07230203 || !w[3] || w[3] > (1u << 22)) return 1;
    uint32_t bound = w[3], root = 0;
    for (size_t i = 5; i < nw; ) {
        uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
        if (!n || i + n > nw) return 1;
        if (op == 71 && n >= 4 && w[i+2] == 11 && w[i+3] == 15) root = w[i+1];
        if (op == 72 && n >= 5 && w[i+3] == 11 && w[i+4] == 15) return 1;
        i += n;
    }
    if (!root || root >= bound) return 0;
    uint8_t *ptr = calloc(bound, 1), *constant = calloc(bound, 1);
    if (!ptr || !constant) DIE("native UI: out of memory classifying FragCoord");
    ptr[root] = 15; int result = 0, function = 0;
    for (size_t i = 5; i < nw && !result; i += w[i] >> 16) {
        uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
        if (op == 54) function = 1;
        if (op == 43 && n == 4 && w[i+2] < bound && w[i+3] < 4) constant[w[i+2]] = (uint8_t)(w[i+3] + 1);
        if ((op == 65 || op == 66) && n >= 5 && w[i+2] < bound && w[i+3] < bound && ptr[w[i+3]]) {
            uint8_t mask = ptr[w[i+3]], index = w[i+4] < bound ? constant[w[i+4]] : 0;
            ptr[w[i+2]] = mask == 15 ? (index ? 1u << (index-1) : 15) : mask;
        } else if (op == 83 && n == 4 && w[i+2] < bound && w[i+3] < bound) ptr[w[i+2]] = ptr[w[i+3]];
        else if (op == 61 && n >= 4 && w[i+3] < bound && (ptr[w[i+3]] & 3)) result = 1;
        else if (function) {
            for (uint32_t j = 1; j < n; j++) if (w[i+j] < bound && (ptr[w[i+j]] & 3)) result = 1;
        }
    }
    free(ptr); free(constant); return result;
}
static int spirv_image_features_ok(const uint32_t *w, size_t nw, int can_read, int can_write) {
    if (nw < 5 || w[0] != 0x07230203) return 0;
    for (size_t i = 5; i < nw; ) {
        uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
        if (!n || n > nw - i) return 0;
        if (op == 17 && n == 2 && ((w[i + 1] == 55 && !can_read) || (w[i + 1] == 56 && !can_write))) return 0;
        i += n;
    }
    return 1;
}
static int hexval(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static vk_shader *shader_load(const char *variant, int early_tests) {
    pthread_once(&g_position_once, position_init);
    shader_key key = {0}; snprintf(key.variant, sizeof key.variant, "%s", variant);
    key.early_tests = (uint32_t)early_tests; key.position_policy = g_position_policy;
    vk_shader *sh = nvn_hm_get(&g_shaders, &key);
    if (sh) return sh;
    char path[1024]; size_t spv_len = 0, vk_len = 0;
    snprintf(path, sizeof path, "%s/%s.spv", seed_dir(), variant); char *spv = read_file(path, &spv_len);
    snprintf(path, sizeof path, "%s/%s.vk", seed_dir(), variant); char *txt = read_file(path, &vk_len);
    if (!spv || !txt || spv_len % 4 || spv_len < 20) { RED(16, "shader %s: SPIR-V or reflection missing in the seed", variant); free(spv); free(txt); return NULL; }
    if (!spirv_image_features_ok((const uint32_t *)spv, spv_len / 4, V.has_image_read_without_format, V.has_image_write_without_format)) {
        RED(16, "shader %s: malformed SPIR-V or unsupported formatless storage image capability", variant);
        free(spv); free(txt); return NULL;
    }
    sh = calloc(1, sizeof *sh); memcpy(sh->key, key.variant, sizeof sh->key); sh->early_tests = key.early_tests & 1; sh->alpha_func = key.early_tests >> 1;
    sh->spv_bytes = spv_len;
    sh->local[0] = sh->local[1] = sh->local[2] = 1;
    int capb = 0;
    for (char *save = NULL, *line = strtok_r(txt, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!strncmp(line, "stage ", 6)) sh->stage = atoi(line + 6);
        else if (!strncmp(line, "local ", 6)) sscanf(line + 6, "%u %u %u", &sh->local[0], &sh->local[1], &sh->local[2]);
        else if (!strncmp(line, "depth ", 6)) sh->writes_depth = atoi(line + 6);
        else if (!strncmp(line, "c1 ", 3)) {
            const char *h = line + 3; size_t n = strlen(h) / 2;
            sh->c1 = calloc(1, n ? n : 1); sh->c1_len = (uint32_t)n;
            for (size_t i = 0; i < n; i++) { int a = hexval(h[2 * i]), b = hexval(h[2 * i + 1]); sh->c1[i] = (uint8_t)(a < 0 || b < 0 ? 0 : a << 4 | b); }
        } else if (!strncmp(line, "vin", 3)) {
            for (char *p = line + 3; *p; ) { while (*p == ' ') p++; if (!*p) break; int loc, cls; if (sscanf(p, "%d:%d", &loc, &cls) == 2 && loc >= 0 && loc < 32) { sh->vin_mask |= 1u << loc; sh->vin_class[loc] = (uint8_t)cls; } while (*p && *p != ' ') p++; }
        } else if (!strncmp(line, "fout", 4)) {
            for (char *p = line + 4; *p; ) { while (*p == ' ') p++; if (!*p) break; int i = atoi(p); if (i >= 0 && i < 8) sh->fout_mask |= 1u << i; while (*p && *p != ' ') p++; }
        } else if (line[0] == 'b' && line[1] == ' ') {
            int bnd, cnt, dt, kind, reg, sreg, off, rw, tgt, isint; uint64_t extent = 0;   /* old ten-field seeds */
            if (sscanf(line + 2, "%d %d %d %d %d %d %d %d %d %d %" SCNu64, &bnd, &cnt, &dt, &kind, &reg, &sreg, &off, &rw, &tgt, &isint, &extent) < 10) continue;
            if (sh->nb == capb) { capb = capb ? 2 * capb : 16; sh->b = realloc(sh->b, (size_t)capb * sizeof *sh->b); }
            sh->b[sh->nb++] = (vk_bind){(uint16_t)bnd, (uint16_t)(cnt > 0 ? cnt : 1), (uint8_t)kind, (uint8_t)dt, (uint8_t)tgt, (uint8_t)rw, (uint8_t)isint, (int16_t)reg, (int16_t)sreg, (uint16_t)off, extent};
        }
    }
    free(txt);
    { vk_load_certificate proof = vk_load_certify((const uint32_t *)spv, spv_len / 4, sh->stage == 1);
      sh->load_position_attr = proof.position_attr; sh->load_full_outputs = proof.full_outputs; }
    { static uint64_t nmark; int m = spirv_mark_stores(sh, (const uint32_t *)spv, spv_len / 4);
      if (m && ++nmark <= 8) LOGV("shader %s writes %d storage binding(s) (from its SPIR-V)", variant, m); }
    if (sh->stage == 1) {   /* kept for a live TOTK_VK_UNWRITTEN_MASK; the launch switch also extends fout_mask, as before */
        sh->fout_span = spirv_fout_locations((const uint32_t *)spv, spv_len / 4);
        if (vk_unwritten_mask_on()) sh->fout_mask |= sh->fout_span;
    }
    for (int i = 0; i < sh->nb; i++) if (sh->b[i].rw &&
        (sh->b[i].dtype == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || sh->b[i].dtype == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC ||
         sh->b[i].dtype == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)) { sh->has_storage_writes = 1; break; }
    if (sh->stage == 1 && vk_native_ui_enabled()) sh->fragcoord_xy = spirv_fragcoord_xy((const uint32_t *)spv, spv_len / 4);
    sh->canvas_ui = vk_canvas_ui_shader(sh->key,0) ? 2 : vk_canvas_ui_shader(sh->key,1);
    sh->canvas_transfer = vk_canvas_transfer_shader(sh->key);
    if (sh->stage == 0) sh->framegen_camera_layout = (uint8_t)vk_fg_camera_layout(sh->key);
    if (sh->stage == 1) sh->framegen_raster_depth = (uint8_t)vk_fg_raster_depth_shader(sh->key);
    sh->framegen_plane=(uint8_t)vk_fg_plane_shader(sh->key,(unsigned)sh->stage);
    if (sh->stage==0) sh->framegen_uncertified_plane=(uint8_t)vk_fg_plane_shader(sh->key,2);
    sh->native_1x = sh->fragcoord_xy || sh->has_storage_writes;
    for (int i = 0; i < sh->nb; i++) {
        const vk_bind *b = &sh->b[i];
        if (b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && !b->rw && b->reg >= 0 && b->reg < 16)
            sh->has_storage_reads = 1;
        if (b->dtype == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && b->reg >= 0 && b->reg < 16 &&
            (b->kind == RB_CBUF || b->kind == RB_UBO_PTR)) sh->uniform_prepare_mask |= 1u << b->reg;
        if (b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) sh->native_1x = 1;
        if (b->dtype != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE && b->dtype != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE &&
            b->dtype != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) continue;
        if (b->kind == RB_IMG) { if (b->reg >= 0 && b->reg < 32) sh->native_img_mask |= 1u << b->reg; }
        else if (b->reg >= 0 && b->reg < 128) sh->native_tex_mask[b->reg/64] |= 1ull << (b->reg&63);
    }
    if (sh->stage == 0 && g_position_policy) {
        size_t nw = spv_len / 4; uint32_t *words = (uint32_t *)spv;
        if (vk_spirv_position(&words, &nw, g_position_policy) < 0) {
            RED(16, "shader %s: cannot preserve requested vertex position precision; refused", variant);
            free(spv); free(sh->b); free(sh->c1); free(sh); return NULL;
        }
        spv = (char *)words; spv_len = nw * 4;
    }
    if (sh->stage == 1) {
        size_t nw = spv_len / 4; uint32_t *words = (uint32_t *)spv;
        if (vk_spirv_early_tests(&words, &nw, sh->early_tests && earlyz_on() && !sh->alpha_func) < 0) {
            RED(16, "shader %s: invalid fragment execution modes", variant); free(spv); free(sh->b); free(sh->c1); free(sh); return NULL;
        }
        spv = (char *)words; spv_len = nw * 4;
        size_t nnw; uint32_t *pw = spirv_depth_nudge((const uint32_t *)spv, spv_len / 4, &nnw);
        if (pw) { free(spv); spv = (char *)pw; spv_len = nnw * 4; sh->depth_spec = 1; }
        patch_set((uint32_t *)spv, spv_len / 4, 1);
        if (sh->alpha_func) {
            unsigned binding=0;
            for (int i=0;i<sh->nb;i++) if (sh->b[i].binding>=binding) binding=sh->b[i].binding+1;
            pw=vk_spirv_alpha_test((const uint32_t *)spv,spv_len/4,sh->alpha_func,binding,&nnw);
            if (!pw || binding>UINT16_MAX) {
                RED(16,"shader %s: cannot build alpha-test comparison %u",variant,sh->alpha_func);
                free(pw); free(spv); free(sh->b); free(sh->c1); free(sh); return NULL;
            }
            free(spv); spv=(char *)pw; spv_len=nnw*4;
            sh->b=realloc(sh->b,(size_t)(sh->nb+1)*sizeof *sh->b);
            sh->b[sh->nb++]=(vk_bind){.binding=binding,.count=1,.kind=RB_ALPHA,
                .dtype=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.reg=-1,.sreg=-1,.extent=16};
            sh->load_full_outputs=0;
        }
    }
    if (V.subgroup_fix) {
        size_t nw = spv_len / 4; uint32_t *words = (uint32_t *)spv;
        int fixed = vk_spirv_subgroup32(&words, &nw);
        spv = (char *)words; spv_len = nw * 4;
        if (fixed > 0) {
            sh->required_subgroup_size = vk_subgroup_required(stage_flags(sh->stage), V.subgroup_control,
                V.subgroup_sizes.requiredSubgroupSizeStages, V.subgroup_sizes.minSubgroupSize, V.subgroup_sizes.maxSubgroupSize);
            uint32_t physical = sh->required_subgroup_size ? sh->required_subgroup_size : V.subgroup.subgroupSize;
            if (physical < 32 || physical > 128 || (physical & (physical - 1)) ||
                !(V.subgroup.supportedStages & stage_flags(sh->stage)) ||
                !(V.subgroup.supportedOperations & VK_SUBGROUP_FEATURE_BALLOT_BIT)) fixed = -1;
            else LOGV("subgroup shader %s: logical 32, physical %u%s", variant, physical,
                sh->required_subgroup_size ? " (pinned)" : " (fixed default)");
        }
        if (fixed < 0) {
            RED(32, "subgroup shader %s: cannot preserve 32-lane semantics on this device/module; refused", variant);
            free(spv); free(sh->b); free(sh->c1); free(sh); return NULL;
        }
    }
    VkShaderModuleCreateInfo mci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = spv_len, .pCode = (const uint32_t *)spv};
    VkResult r = vkCreateShaderModule(V.dev, &mci, NULL, &sh->mod);
    free(spv);
    if (r) { RED(16, "vkCreateShaderModule %s: %d", variant, r); free(sh->b); free(sh->c1); free(sh); return NULL; }
    VkDescriptorSetLayoutBinding *lb = calloc((size_t)(sh->nb ? sh->nb : 1), sizeof *lb);
    for (int i = 0; i < sh->nb; i++) lb[i] = (VkDescriptorSetLayoutBinding){sh->b[i].binding, (VkDescriptorType)sh->b[i].dtype, sh->b[i].count, stage_flags(sh->stage), NULL};
    VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = (uint32_t)sh->nb, .pBindings = lb};
    char why[128];
    if (!vk_desc_layout_fits(lb, (uint32_t)sh->nb, why, sizeof why)) {   /* it could never be allocated: refuse it here */
        RED(16, "descriptor set layout of %s: %s", variant, why);
        free(lb); vkDestroyShaderModule(V.dev, sh->mod, NULL); free(sh->b); free(sh->c1); free(sh); return NULL;
    }
    r = vkCreateDescriptorSetLayout(V.dev, &dci, NULL, &sh->dsl);
    free(lb);
    if (r) { RED(16, "descriptor set layout of %s: %d", variant, r); vkDestroyShaderModule(V.dev, sh->mod, NULL); free(sh->b); free(sh->c1); free(sh); return NULL; }
    sh->xlate_miss_ns = vk_shader_xlate_take_miss(variant);
    nvn_hm_put(&g_shaders, &key, sh);
    HK_ADD(shloads, 1); HK_ADD(shbytes, spv_len + vk_len);
    if (sh->stage == 5) vk_compute_prepare(sh);
    return sh;
}
vk_shader *vk_shader_alpha(vk_shader *base, unsigned func) {
    if (!base || base->stage!=1 || !func || func==NVN_ALPHA_FUNC_ALWAYS) return base;
    if (func>NVN_ALPHA_FUNC_GEQUAL) return NULL;
    return shader_load(base->key,base->early_tests | (int)(func<<1));
}
/* Startup/replay only: exact persisted or program-known variant; shader/layout maps
   remain single-owner even though compute driver calls run on a worker. */
vk_shader *vk_shader_seed(const char *variant, int stage, int early_tests) {
    size_t n = strnlen(variant, 80);
    if (n != 64 && n != 77) return NULL;
    for (size_t i = 0; i < n; i++) if (i == 64 ? variant[i] != '-' : hexval(variant[i]) < 0) return NULL;
    if (early_tests<0 || early_tests>15 || (stage!=1 && early_tests)) return NULL;
    vk_shader *s = shader_load(variant,stage==1 ? early_tests : 0);
    return s && s->stage == stage ? s : NULL;
}

/* ---------- variants of a shader ---------- */
typedef struct { char *name, *deps; } variant_t;
typedef struct { variant_t *v; int n; uint64_t epoch; } variant_list;
static nvn_hmap g_variants = {.klen = 64};   /* shader key (hex) -> variant_list */
static variant_list *variants_of(const char *key) {
    char k[64]; memcpy(k, key, 64);
    variant_list *vl = nvn_hm_get(&g_variants, k);
    uint64_t epoch = vk_shader_xlate_epoch();
    if (vl && vl->epoch == epoch) return vl;
    if (!vl) vl = calloc(1, sizeof *vl);
    else { for (int i = 0; i < vl->n; i++) { free(vl->v[i].name); free(vl->v[i].deps); } free(vl->v); vl->v = NULL; vl->n = 0; }
    vl->epoch = epoch;
    char path[1024]; snprintf(path, sizeof path, "%s/%.64s.vv", seed_dir(), key);
    char *txt = read_file(path, NULL);
    int cap = 0;
    if (txt) HK_ADD(vlists, 1);
    if (txt) for (char *save = NULL, *line = strtok_r(txt, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *tab = strchr(line, '\t'); if (tab) *tab = 0;
        if (vl->n == cap) { cap = cap ? 2 * cap : 4; vl->v = realloc(vl->v, (size_t)cap * sizeof *vl->v); }
        vl->v[vl->n].name = strdup(line); vl->v[vl->n].deps = strdup(tab ? tab + 1 : ""); vl->n++;
    }
    free(txt);
    nvn_hm_put(&g_variants, k, vl);
    return vl;
}
/* The request a PC-side fill needs (the iPhone's shader-requests/<variant>.json, ios/tools/fill_shaders.py). */
static void answers_json(const answers_t *a, char *out, size_t cap) {
    size_t o = 0;
#define PUT(...) do { int n_ = snprintf(out + o, o < cap ? cap - o : 0, __VA_ARGS__); if (n_ > 0) o += (size_t)n_; } while (0)
    /* NSJSONSerialization / json.dumps(sort_keys) order: keys sorted as strings ("10" before "2") */
    const char *kinds[4] = {"attr", "fout", "img", "tex"};
    PUT("{");
    for (int k = 0; k < 4; k++) {
        PUT("%s\"%s\":{", k ? "," : "", kinds[k]);
        int n = k == 0 ? 16 : k == 1 ? 8 : k == 2 ? 32 : 128, first = 1;
        char names[128][8]; int idx[128], m = 0;
        for (int i = 0; i < n; i++) { int has = k == 0 ? a->has_attr[i] : k == 1 ? a->has_fout[i] : k == 2 ? a->has_img[i] : a->has_tex[i]; if (has) { snprintf(names[m], 8, "%d", i); idx[m++] = i; } }
        for (int i = 1; i < m; i++) for (int j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; j--) { char t[8]; memcpy(t, names[j], 8); memcpy(names[j], names[j - 1], 8); memcpy(names[j - 1], t, 8); int ti = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = ti; }
        for (int j = 0; j < m; j++) {
            int i = idx[j];
            if (k == 2) { char ty[24] = {0}, fm[24] = {0}; const char *slash = strchr(a->img[i], '/');
                if (slash) { snprintf(ty, sizeof ty, "%.*s", (int)(slash - a->img[i]), a->img[i]); snprintf(fm, sizeof fm, "%s", slash + 1); } else snprintf(ty, sizeof ty, "%s", a->img[i]);
                if (fm[0]) PUT("%s\"%s\":{\"fmt\":\"%s\",\"type\":\"%s\"}", first ? "" : ",", names[j], fm, ty); else PUT("%s\"%s\":{\"type\":\"%s\"}", first ? "" : ",", names[j], ty); }
            else PUT("%s\"%s\":\"%s\"", first ? "" : ",", names[j], k == 0 ? a->attr[i] : k == 1 ? a->fout[i] : a->tex[i]);
            first = 0;
        }
        PUT("}");
    }
    PUT("}");
#undef PUT
}
static void request_miss(const char *key, int st, const uint8_t *control, const uint8_t *code, const answers_t *a) {
    char aj[16384]; answers_json(a, aj, sizeof aj);
    unsigned char md[32]; host_sha256(aj, strlen(aj), md);
    char variant[80]; snprintf(variant, sizeof variant, "%.64s-", key);
    for (int i = 0; i < 6; i++) snprintf(variant + 65 + 2 * i, 3, "%02x", md[i]);
    static nvn_hmap missed = {.klen = 80}; char k[80] = {0}; memcpy(k, variant, strlen(variant));
    if (nvn_hm_get(&missed, k)) return;
    nvn_hm_put(&missed, k, (void *)1);
    const char *dir = getenv("TOTK_SHADER_REQ_DIR"); if (!dir || !*dir) dir = "shader-requests";
    int background = vk_seed_io_active();
    if (!background) mkdir(dir, 0755);
    uint32_t c1o, c1t; memcpy(&c1o, control + 0x700, 4); memcpy(&c1t, control + 0x704, 4);
    size_t skip = st == 5 ? 0 : 0x30;
    if (!c1t || c1t > (16u << 20) || c1o > c1t || c1t < skip) return;
    char path[1024]; snprintf(path, sizeof path, "%s/%s.json", dir, variant);
    char *json = NULL; size_t json_size = 0;
    FILE *f = background ? open_memstream(&json, &json_size) : fopen(path, "w"); if (!f) return;
    HK_ADD(sreq, 1);
    fprintf(f, "{\"variant\":\"%s\",\"stage\":\"%s\",\"depth_minus_w\":%s", variant, st == 0 ? "vs" : st == 1 ? "ps" : "cs", totk_depth_near_minus_w && totk_depth_near_minus_w() ? "true" : "false");
    fprintf(f, ",\"early_fragment_tests\":%s", vk_nvn_early_tests(control, ctl_key_len(control), st) ? "true" : "false");
    if (st == 5) { uint32_t l[3]; memcpy(l, control + 0x748, 12); fprintf(f, ",\"local\":[%u,%u,%u]", l[0], l[1], l[2]); }
    fputs(",\"c1\":\"", f); for (uint32_t i = c1o; i < c1t; i++) fprintf(f, "%02x", code[i]);
    fputs("\",\"code_hex\":\"", f); for (size_t i = skip; i < c1t; i++) fprintf(f, "%02x", code[i]);
    fprintf(f, "\",%s\n", aj + 1);   /* the answers' own members (and their closing brace) after the request's */
    if (fclose(f)) { free(json); return; }
    if (background) {
        if (!vk_seed_io_request(dir, variant, json, json_size)) { free(json); RED(4, "shader request queue allocation failed: %s", variant); }
    } else vk_shader_xlate_enqueue(path, variant, seed_dir());
    static uint64_t n; n++;
    if (n <= 20 || n % 100 == 0) LOGV("[shader-miss] %s %s is not in the seed: its draws are skipped (request %llu %s in %s)", st == 0 ? "vs" : st == 1 ? "ps" : "cs", variant, (unsigned long long)n, background ? "queued" : "saved", dir);
}

/* ---------- programs ---------- */
/* Lookout Landing: vk_treg_kind was 8.6% of recording. Binding hooks and the registry revision cover every signature input. */
static uint64_t g_stage_sig[6], g_stage_sig_revision[6];
static unsigned g_stage_sig_valid;
void vk_stage_sig_invalidate(unsigned stages) { g_stage_sig_valid &= ~stages; }   /* S and this memo belong to the recording thread */
static uint64_t stage_sig_for(const vk_state *state, int st) {
    uint64_t sg = 0;
    if (st == 1 && state->has_rt) for (int i = 0; i < state->rt.n && i < 8; i++) if (state->rt.color[i])
        sg |= (uint64_t)(1 + vk_fmt_class(state->rt.has_cview[i] && state->rt.cview[i].set_format ? state->rt.cview[i].format : state->rt.color[i]->d.format)) << (3 * i);
    if (st == 0) for (int i = 0; i < state->nattr && i < 16; i++) sg |= (uint64_t)vk_fmt_class(state->attr[i].fmt) << (2 * i);
    uint64_t dm = 0; int hi = st >= 0 && st < 6 && state->tex_hi[st] ? state->tex_hi[st] : 128;
    for (int r = 0; r < hi && r < 128; r++) if (state->tex[st][r]) {
        int f, tg; if (!vk_treg_kind((int)(state->tex[st][r] & 0xfffff), &f, &tg)) continue;
        if (f >= 0x32 && f <= 0x36) dm ^= 0x9E3779B97F4A7C15ull * (uint64_t)(r + 1);
    }
    if (dm) sg ^= dm | 1ull << 63;
    return sg;
}
static uint64_t stage_sig_at(int st, uint64_t revision) {   /* nvn_mtl.m stage_sig */
    if ((g_stage_sig_valid & (1u << st)) && g_stage_sig_revision[st] == revision) return g_stage_sig[st];
    uint64_t sg = stage_sig_for(&S, st);
    g_stage_sig[st] = sg; g_stage_sig_revision[st] = revision; g_stage_sig_valid |= 1u << st;   /* pre-scan revision: a concurrent registration invalidates the next lookup */
    return sg;
}
static uint64_t stage_sig(int st) { return stage_sig_at(st, vk_treg_revision()); }
typedef struct { uint64_t sig; vk_shader *sh; int missing; } alt_t;
struct vk_prog {
    char key[6][65]; const uint8_t *ctl[6], *code[6]; int has[6]; unsigned stages;
    uint64_t xlate_epoch;
    uint64_t prepare_epoch; int prepared;
    alt_t *alt[6]; int nalt[6], capalt[6];
    vk_shader *cur[6]; uint64_t cur_sig[6]; int cur_ok[6];
    struct { uint64_t registry, xlate_epoch, signature[6]; vk_shader *shader[6]; int ready, result; } record;
};
static nvn_hmap g_progs = {.klen = 8};   /* prog_t serial -> vk_prog */
/* The game releases programs on its own threads (nvnProgramSetShaders / Finalize) while the replay thread looks them
   up and adds them; the map's growth frees its arrays, so every access holds this. */
static pthread_mutex_t g_progs_mu = PTHREAD_MUTEX_INITIALIZER;
/* Recording thread only (vk_prog_resolve: draw / dispatch under vk_rec_lock). A small direct-mapped cache by serial in
   front of the locked map: locking g_progs_mu for every draw was ~4 % of recording at Lookout Landing. Safe because
   serials are never reused (nvn_new_serial; ProgramSetShaders takes a new one) and a vk_prog is never freed. */
enum { PROGC_N = 64 };
static struct { uint64_t serial; vk_prog *pr; } g_progc[PROGC_N];
static vk_prog *prog_of(const prog_t *p) {
    if (!p || !p->serial) return NULL;
    unsigned h = (unsigned)(p->serial % PROGC_N);
    if (g_progc[h].serial == p->serial) return g_progc[h].pr;
    pthread_mutex_lock(&g_progs_mu); vk_prog *pr = nvn_hm_get(&g_progs, &p->serial); pthread_mutex_unlock(&g_progs_mu);
    if (pr) { g_progc[h].serial = p->serial; g_progc[h].pr = pr; return pr; }
    pr = calloc(1, sizeof *pr);
    for (unsigned i = 0; i < p->nstage && i < 6; i++) {
        const uint8_t *control = p->sd[i].control; uint32_t length = 0, st = 0;
        if (!control || !p->sd[i].data) continue;
        memcpy(&length, control + 0x704, 4); memcpy(&st, control + 0x714, 4);
        if (!length || length > (16u << 20) || (st != 0 && st != 1 && st != 5)) { RED(8, "shader stage %u (length %u) is not supported", st, length); continue; }
        host_sha256_ctx ctx; unsigned char md[32];
        host_sha256_init(&ctx); host_sha256_update(&ctx, control, ctl_key_len(control)); host_sha256_update(&ctx, (const void *)(uintptr_t)p->sd[i].data, length); host_sha256_final(&ctx, md);
        for (int j = 0; j < 32; j++) snprintf(pr->key[st] + 2 * j, 3, "%02x", md[j]);
        pr->ctl[st] = control; pr->code[st] = (const uint8_t *)(uintptr_t)p->sd[i].data; pr->has[st] = 1; pr->stages |= 1u << st;
    }
    pthread_mutex_lock(&g_progs_mu); nvn_hm_put(&g_progs, &p->serial, pr); pthread_mutex_unlock(&g_progs_mu);
    g_progc[h].serial = p->serial; g_progc[h].pr = pr;
    return pr;
}
void vk_prog_prepare(const prog_t *p) {
    if (!vk_shaders_ready()) return;
    vk_prog *pr = prog_of(p); if (!pr) return;
    uint64_t epoch = vk_shader_xlate_epoch();
    if (pr->prepared && pr->prepare_epoch == epoch) return;
    pr->prepared = 1; pr->prepare_epoch = epoch;
    for (unsigned stages = pr->stages; stages; stages &= stages - 1) {
        int st = __builtin_ctz(stages);
        /* Compute pipelines depend only on the exact shader, not dispatch-time
           resources. Compile all listed variants without choosing one using stale
           bind state. Dispatch still uses the authoritative ordinary resolver. */
        if (st == 5) {
            variant_list *vl = variants_of(pr->key[st]);
            for (int i = 0; i < vl->n; i++) (void)vk_shader_seed(vl->v[i].name, 5, 0);
        }
    }
}
void mtl_prog_release(prog_t *p) { if (!p->serial) return; pthread_mutex_lock(&g_progs_mu); nvn_hm_del(&g_progs, &p->serial); pthread_mutex_unlock(&g_progs_mu); }   /* the vk_prog leaks a little; programs are few */
int vk_prog_resolve(const prog_t *p, vk_shader *out[6]) {
    memset(out, 0, 6 * sizeof *out);
    if (!vk_shaders_ready()) return -1;
    vk_prog *pr = prog_of(p);
    if (!pr) return -1;
    uint64_t epoch = vk_shader_xlate_epoch();
    if (pr->xlate_epoch != epoch) {
        /* Shader maps and negative program caches are owned here, never by the child/worker.
           Ready variants remain valid; only misses must resolve again after publication. */
        for (int st = 0; st < 6; st++) {
            int n = 0;
            for (int i = 0; i < pr->nalt[st]; i++) if (!pr->alt[st][i].missing) pr->alt[st][n++] = pr->alt[st][i];
            pr->nalt[st] = n;
            if (!pr->cur[st]) pr->cur_ok[st] = 0;
        }
        pr->xlate_epoch = epoch;
    }
    int missing = 0;
    for (int st = 0; st < 6; st++) {
        if (!pr->has[st]) continue;
        uint64_t sg = stage_sig(st);
        if (pr->cur_ok[st] && pr->cur_sig[st] == sg) { out[st] = pr->cur[st]; if (!out[st]) missing = 1; continue; }
        alt_t *hit = NULL;
        for (int i = 0; i < pr->nalt[st]; i++) if (pr->alt[st][i].sig == sg) { hit = &pr->alt[st][i]; break; }
        if (!hit) {
            answers_t a; answers_of(st, &a);
            variant_list *vl = variants_of(pr->key[st]);
            vk_shader *sh = NULL;
            for (int i = 0; i < vl->n && !sh; i++) if (deps_match(vl->v[i].deps, &a)) sh = shader_load(vl->v[i].name, vk_nvn_early_tests(pr->ctl[st], ctl_key_len(pr->ctl[st]), st));
            if (!sh) request_miss(pr->key[st], st, pr->ctl[st], pr->code[st], &a);
            if (pr->nalt[st] == pr->capalt[st]) { pr->capalt[st] = pr->capalt[st] ? 2 * pr->capalt[st] : 2; pr->alt[st] = realloc(pr->alt[st], (size_t)pr->capalt[st] * sizeof *pr->alt[st]); }
            pr->alt[st][pr->nalt[st]++] = (alt_t){sg, sh, !sh};
            hit = &pr->alt[st][pr->nalt[st] - 1];
        }
        pr->cur[st] = hit->sh; pr->cur_sig[st] = sg; pr->cur_ok[st] = 1;
        out[st] = hit->sh; if (!hit->sh) missing = 1;
    }
    return missing ? -1 : 0;
}
/* Prediction only borrows an already-resolved variant. It never loads a shader, caches a miss,
   or changes the active program/signature/recorder memo. Programs and shader objects never move or die. */
int vk_prog_predict(const vk_state *state, vk_shader *out[6]) {
    memset(out, 0, 6 * sizeof *out);
    const prog_t *p = state->prog; if (!p || !p->serial) return 0;
    unsigned h = (unsigned)(p->serial % PROGC_N);
    vk_prog *pr = g_progc[h].serial == p->serial ? g_progc[h].pr : NULL;
    if (!pr) {
        pthread_mutex_lock(&g_progs_mu); pr = nvn_hm_get(&g_progs, &p->serial); pthread_mutex_unlock(&g_progs_mu);
    }
    if (!pr || !pr->has[0] || pr->has[5]) return 0;
    uint64_t revision = vk_treg_revision();
    for (int st = 0; st < 2; st++) if (pr->has[st]) {
        uint64_t sig = stage_sig_for(state, st);
        if (pr->cur_ok[st] && pr->cur_sig[st] == sig) out[st] = pr->cur[st];
        else for (int i = 0; i < pr->nalt[st]; i++) if (pr->alt[st][i].sig == sig) { out[st] = pr->alt[st][i].sh; break; }
        if (!out[st]) return 0;
    }
    return revision == vk_treg_revision();
}
/* Recorder memo compares the active program's semantic signatures, not invalidation flags or unrelated stages.
   Recomputing a dirty signature can prove an NVN rebind did not change the variant selected by the ordinary resolver.
   Compiler workers publish pipeline handles, not shader/variant maps; no pipeline handle is cached here.
   The service's publication epoch also keys this memo, including its negative results. */
static uint64_t g_record_prog_hits, g_record_prog_misses;
int vk_prog_resolve_record(const prog_t *p, vk_shader *out[6]) {
    /* Owned by the program, so unrelated serials cannot evict a hot result. Programs are never freed. */
    if (!p || !p->serial || !vk_shaders_ready()) return vk_prog_resolve(p, out);
    vk_prog *program = prog_of(p); if (!program) return vk_prog_resolve(p, out);
    uint64_t registry = vk_treg_revision(), epoch = vk_shader_xlate_epoch(), signatures[6];
    int refreshed = 0, same = 1;
    __typeof__(program->record) *m = &program->record;
    /* A program's stage set is immutable. Visit only those entries, rather than
       clearing/comparing six signatures and probing all four absent graphics stages.
       The pre-scan registry revision still invalidates the next draw on publication. */
    for (unsigned stages = program->stages; stages; stages &= stages - 1) {
        int st = __builtin_ctz(stages);
        refreshed |= !(g_stage_sig_valid & (1u << st)) || g_stage_sig_revision[st] != registry;
        signatures[st] = stage_sig_at(st, registry);
        same &= m->signature[st] == signatures[st];
    }
    if (m->ready && m->xlate_epoch == epoch && same) {
        if (refreshed || m->registry != registry) vk_record_memo_saved(VK_REC_M_SIGNATURE);
        m->registry = registry;
        memcpy(out, m->shader, sizeof m->shader); g_record_prog_hits++; return m->result;
    }
    unsigned causes = !m->ready || m->xlate_epoch != epoch ? (1u << VK_REC_M_COLD) | (1u << VK_REC_M_PROGRAM) : 0;
    if (m->ready && !same) causes |= 1u << VK_REC_M_SIGNATURE;
    if (m->ready && m->registry != registry) causes |= 1u << VK_REC_M_REGISTRY;
    vk_record_memo_miss(1, causes);
    g_record_prog_misses++; int result = vk_prog_resolve(p, out);
    m->ready = 1; m->registry = registry; m->xlate_epoch = epoch; m->result = result;
    for (unsigned stages = program->stages; stages; stages &= stages - 1) {
        int st = __builtin_ctz(stages); m->signature[st] = signatures[st];
    }
    memcpy(m->shader, out, sizeof m->shader);
    return result;
}
void vk_prog_record_cache_take(uint64_t *hits, uint64_t *misses) {
    *hits = g_record_prog_hits; *misses = g_record_prog_misses; g_record_prog_hits = g_record_prog_misses = 0;
}
#ifdef TOTK_VK_TESTS
uint64_t vk_prog_record_test_hits(void) { return g_record_prog_hits; }
#endif

/* ---------- pipeline layouts: set 0 = vertex (or compute) stage, set 1 = fragment stage ---------- */
static VkDescriptorSetLayout empty_dsl(void) {
    static VkDescriptorSetLayout e;
    if (!e) { VkDescriptorSetLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; VKCHECK(vkCreateDescriptorSetLayout(V.dev, &ci, NULL, &e)); }
    return e;
}
VkPipelineLayout vk_layout_of(vk_shader *vs, vk_shader *fs) {
    static nvn_hmap layouts = {.klen = 16};
    void *k[2] = {vs, fs};
    VkPipelineLayout l = (VkPipelineLayout)nvn_hm_get(&layouts, k);
    if (l) return l;
    VkDescriptorSetLayout sets[2] = {vs ? vs->dsl : empty_dsl(), fs ? fs->dsl : empty_dsl()};
    VkPipelineLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = fs ? 2 : 1, .pSetLayouts = sets};
    VKCHECK(vkCreatePipelineLayout(V.dev, &ci, NULL, &l));
    nvn_hm_put(&layouts, k, (void *)l);
    return l;
}
