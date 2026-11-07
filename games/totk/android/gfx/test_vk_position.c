/* Pure host policy/rewrite checks. Optional input/output arguments let the Python
   driver validate real seed modules with spirv-val, without a Vulkan device. */
#include <assert.h>
#include "vk_position.h"
#include "vk_seed_pack.h"
#define OP(n, op) ((uint32_t)(n) << 16 | (op))

typedef struct { uint32_t *words; size_t count; } module;
static module clone(const uint32_t *words, size_t count) {
    module m = {malloc(count * 4), count}; assert(m.words);
    memcpy(m.words, words, count * 4); return m;
}
static unsigned decoration(module m, uint32_t id, uint32_t member, uint32_t kind) {
    unsigned count = 0;
    for (size_t i = 5; i < m.count; i += m.words[i] >> 16) {
        uint32_t n = m.words[i] >> 16, op = m.words[i] & 0xffff;
        if (member == UINT32_MAX) count += op == 71 && n == 3 && m.words[i+1] == id && m.words[i+2] == kind;
        else count += op == 72 && n == 4 && m.words[i+1] == id && m.words[i+2] == member && m.words[i+3] == kind;
    }
    return count;
}
/* Numeric IDs pin the slice across a local store/load, phi and a function call.
   The unrelated varying/PointSize product must remain contractible. */
static module fixture(int block) {
    static const uint32_t pre[] = {
        0x07230203, 0x00010300, 0, 64, 0,
        OP(2,17), 1, OP(6,11), 14, 0x4c534c47, 0x6474732e, 0x3035342e, 0,
        OP(3,14), 0, 1,
        OP(7,15), 0, 30, 0x6e69616d, 0, 10, 11,
    };
    static const uint32_t direct[] = {OP(4,71), 10, 11, 0};
    static const uint32_t member[] = {OP(3,71), 17, 2, OP(5,72), 17, 0, 11, 0, OP(5,72), 17, 1, 11, 1};
    static const uint32_t types[] = {
        OP(4,71), 11, 30, 0,
        OP(2,19), 1, OP(3,22), 2, 32, OP(4,23), 3, 2, 4,
        OP(4,32), 4, 3, 3, OP(4,32), 5, 7, 2, OP(3,33), 6, 1,
        OP(4,43), 2, 7, 0x3f800000, OP(4,33), 13, 2, 2,
        OP(2,20), 15, OP(3,41), 15, 16,
        OP(4,30), 17, 3, 2, OP(4,32), 18, 3, 17,
        OP(4,21), 19, 32, 1, OP(4,43), 19, 20, 0, OP(4,43), 19, 21, 1,
        OP(4,32), 23, 3, 2,
    };
    static const uint32_t vars[] = {OP(4,59), 4, 10, 3, OP(4,59), 4, 11, 3};
    static const uint32_t functions[] = {
        OP(5,54), 2, 40, 0, 13, OP(3,55), 2, 41, OP(2,248), 42,
        OP(5,129), 2, 43, 41, 7, OP(2,254), 43, OP(1,56),
        OP(5,54), 1, 30, 0, 6, OP(2,248), 31, OP(4,59), 5, 32, 7,
        OP(5,133), 2, 33, 7, 7, OP(3,62), 32, 33, OP(4,61), 2, 34, 32,
        OP(3,247), 48, 0, OP(4,250), 16, 45, 46,
        OP(2,248), 45, OP(5,131), 2, 47, 34, 7, OP(2,249), 48,
        OP(2,248), 46, OP(5,136), 2, 49, 34, 7, OP(2,249), 48,
        OP(2,248), 48, OP(7,245), 2, 50, 47, 45, 49, 46,
        OP(5,57), 2, 35, 40, 50, OP(5,133), 2, 36, 7, 7,
        OP(8,12), 2, 53, 14, 50, 35, 7, 7, /* explicit Fma must stay fused */
        OP(7,80), 3, 37, 53, 7, 7, 7, OP(7,80), 3, 38, 36, 7, 7, 7,
    };
    uint32_t words[512]; size_t count = 0;
#define APPEND(a) do { memcpy(words + count, (a), sizeof(a)); count += sizeof(a) / 4; } while (0)
    APPEND(pre);
    if (block) APPEND(member); else APPEND(direct);
    APPEND(types);
    size_t variable = count; APPEND(vars);
    if (block) words[variable + 1] = 18;
    APPEND(functions);
    if (block) {
        const uint32_t stores[] = {OP(5,65), 4, 51, 10, 20, OP(5,65), 23, 52, 10, 21,
            OP(3,62), 51, 37, OP(3,62), 52, 36};
        APPEND(stores);
    } else { const uint32_t store[] = {OP(3,62), 10, 37}; APPEND(store); }
    const uint32_t end[] = {OP(3,62), 11, 38, OP(1,253), OP(1,56)}; APPEND(end);
#undef APPEND
    return clone(words, count);
}
static void check_fixture(int block, const char *output) {
    module original = fixture(block);
    for (unsigned policy = 0; policy < 4; policy++) {
        module m = clone(original.words, original.count); uint32_t *before = m.words;
        int changed = vk_spirv_position(&m.words, &m.count, policy);
        assert(changed == (policy ? 1 : 0));
        assert(m.words[3] == original.words[3]);
        assert(decoration(m, block ? 17 : 10, block ? 0 : UINT32_MAX, 18) == !!(policy & 1));
        for (unsigned id = 0; id < 64; id++)
            assert(decoration(m, id, UINT32_MAX, 42) == ((policy & 2) && (id == 33 || id == 43 || id == 47 || id == 49)));
        if (!policy) assert(m.words == before && m.count == original.count && !memcmp(m.words, original.words, m.count * 4));
        before = m.words; size_t count = m.count;
        assert(vk_spirv_position(&m.words, &m.count, policy) == 0);
        assert(m.words == before && m.count == count); /* no duplicate decorations */
        if (output) {
            char path[4096]; assert(snprintf(path, sizeof path, "%s/%s-%u.spv", output, block ? "member" : "direct", policy) < (int)sizeof path);
            FILE *f = fopen(path, "wb"); assert(f); assert(fwrite(m.words, 4, m.count, f) == m.count); assert(!fclose(f));
        }
        free(m.words);
    }
    /* Non-vertex stages and an absent position output do not change. */
    module other = clone(original.words, original.count);
    for (size_t i = 5; i < other.count; i += other.words[i] >> 16)
        if ((other.words[i] & 0xffff) == 15) other.words[i+1] = 4;
    assert(vk_spirv_position(&other.words, &other.count, 3) == 0); free(other.words);
    other = clone(original.words, original.count);
    for (size_t i = 5; i < other.count; i += other.words[i] >> 16) {
        if ((other.words[i] & 0xffff) == 71 && (other.words[i] >> 16) == 4 && other.words[i+2] == 11) other.words[i+3] = 1;
        if ((other.words[i] & 0xffff) == 72 && (other.words[i] >> 16) == 5 && other.words[i+3] == 11) other.words[i+4] = 1;
    }
    assert(vk_spirv_position(&other.words, &other.count, 3) == 0); free(other.words);
    /* Unsupported dependencies fail atomically, but an unrelated one is fine. */
    for (unsigned id = 33; id <= 36; id += 3) {
        other = clone(original.words, original.count);
        for (size_t i = 5; i < other.count; i += other.words[i] >> 16)
            if ((other.words[i] & 0xffff) == 133 && other.words[i+2] == id) other.words[i] = OP(5,60000);
        uint32_t *before = other.words; size_t count = other.count;
        assert(vk_spirv_position(&other.words, &other.count, 2) == (id == 33 ? -1 : 1));
        if (id == 33) assert(other.words == before && other.count == count);
        free(other.words);
    }
    other = clone(original.words, original.count); other.words[5] = 0;
    uint32_t *before = other.words; size_t count = other.count;
    assert(vk_spirv_position(&other.words, &other.count, 3) == -1);
    assert(other.words == before && other.count == count); free(other.words);
    other = clone(original.words, original.count); count = other.count; before = other.words;
    other.words[other.count-1] = OP(2,56); /* instruction overruns the module */
    assert(vk_spirv_position(&other.words, &other.count, 3) == -1);
    assert(other.words == before && other.count == count); free(other.words);
    free(original.words);
}
static int sample_pack(const char *path, const char *output, unsigned limit) {
    vk_seed_pack pack; assert(limit && !vk_seed_pack_open(&pack, path));
    unsigned samples = 0;
    for (uint64_t i = 0; i < pack.count && samples < limit; i++) {
        const char *name; const void *bytes; size_t size;
        assert(vk_seed_pack_entry(&pack, i, &name, &bytes, &size));
        size_t length = strlen(name);
        if (length < 4 || strcmp(name + length - 4, ".spv") || size < 20 || size % 4) continue;
        const uint32_t *w = bytes; int vertex = 0;
        for (size_t j = 5; j < size / 4; j += w[j] >> 16) {
            unsigned n = w[j] >> 16; assert(n && n <= size / 4 - j);
            if ((w[j] & 0xffff) == 15 && n >= 4 && w[j+1] == 0) vertex = 1;
        }
        if (!vertex) continue;
        char target[4096]; assert(snprintf(target, sizeof target, "%s/%s", output, name) < (int)sizeof target);
        FILE *f = fopen(target, "wb"); assert(f); assert(fwrite(bytes, 1, size, f) == size); assert(!fclose(f));
        puts(name); samples++;
    }
    vk_seed_pack_close(&pack); assert(samples); return 0;
}
int main(int argc, char **argv) {
    if (argc == 5 && !strcmp(argv[1], "--sample-pack"))
        return sample_pack(argv[2], argv[3], (unsigned)strtoul(argv[4], NULL, 10));
    if (argc == 5) {
        FILE *f = fopen(argv[1], "rb"); assert(f); assert(!fseek(f, 0, SEEK_END));
        long bytes = ftell(f); assert(bytes >= 20 && bytes % 4 == 0); rewind(f);
        module m = {malloc((size_t)bytes), (size_t)bytes / 4}; assert(m.words);
        assert(fread(m.words, 1, (size_t)bytes, f) == (size_t)bytes); assert(!fclose(f));
        unsigned policy = vk_position_policy(argv[3], argv[4]);
        int result = vk_spirv_position(&m.words, &m.count, policy);
        if (result < 0) { free(m.words); fprintf(stderr, "position dependency unsupported: %s\n", argv[1]); return 2; }
        f = fopen(argv[2], "wb"); assert(f); assert(fwrite(m.words, 4, m.count, f) == m.count); assert(!fclose(f));
        free(m.words); return 0;
    }
    assert(argc == 1 || argc == 2);
    assert(vk_position_policy(NULL, NULL) == 0);
    const char *off[] = {"", "0", "2", "true", "01", "1x", "-1"};
    for (unsigned i = 0; i < sizeof off / sizeof *off; i++) assert(vk_position_policy(off[i], off[i]) == 0);
    assert(vk_position_policy("1", "0") == 1 && vk_position_policy("0", "1") == 2 && vk_position_policy("1", "1") == 3);
    assert(!*vk_position_cache_suffix(0));
    for (unsigned a = 0; a < 4; a++) for (unsigned b = a + 1; b < 4; b++)
        assert(strcmp(vk_position_cache_suffix(a), vk_position_cache_suffix(b)));
    check_fixture(0, argc == 2 ? argv[1] : NULL); check_fixture(1, argc == 2 ? argv[1] : NULL);
    puts("vertex position policy, decorations, slices, idempotence and failure atomicity passed");
    return 0;
}
