/* Standalone proof tests; optional file mode exercises the production pass on
 * a SPIR-V corpus without Vulkan. No game shaders are embedded in this test. */
#include <stdio.h>
#include <assert.h>
#include "vk_int_fold.h"

static uint32_t rng = 0x654321u;
static uint32_t random32(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static void algebra(void) {
    const unsigned ops[] = {128, 132, 194, 196, 197, 199};
    for (unsigned i = 0; i < 200000; i++) {
        uint32_t x = random32(), y = random32(), k = random32(), l = random32();
        if (i & 1) { y &= 31; l = UINT32_MAX; }
        if (i % 3 == 0) { k = vk_int_mask(i % 33); l = vk_int_mask((i / 33) % 33); }
        for (unsigned j = 0; j < sizeof ops / sizeof *ops; j++) {
            unsigned op = ops[j];
            if ((op == 194 || op == 196) && y >= 32) continue;
            uint32_t actual = op == 128 ? x+y : op == 132 ? x*y : op == 194 ? x>>y :
                              op == 196 ? x<<y : op == 197 ? x|y : x&y;
            vk_int_bits b = vk_int_binary(op, (vk_int_bits){k, x&k}, (vk_int_bits){l, y&l});
            assert((actual & b.known) == b.value);
        }
    }
    vk_int_bits b = vk_int_binary(132, (vk_int_bits){0}, (vk_int_bits){UINT32_MAX, 48});
    assert((b.known & 15) == 15 && !(b.value & 15));
}
/* Synthetic scalar address: ((unknown * 48) >> 2) & 3 is always zero.
 * The Function local models the translator's register temporaries. */
static size_t fixture(uint32_t *w, unsigned boundary, unsigned escape, unsigned vol) {
    uint32_t a[] = {0x07230203,0x10300,0,64,0,
        4u<<16|21,1,32,0, 4u<<16|32,2,7,1,
        4u<<16|43,1,3,0, 4u<<16|43,1,4,48,
        4u<<16|43,1,5,2, 4u<<16|43,1,6,3,
        5u<<16|54,1,7,0,8, 2u<<16|248,9,
        4u<<16|59,2,10,7, 3u<<16|45,1,11,
        5u<<16|132,1,12,11,4, 3u<<16|62,10,12};
    memcpy(w,a,sizeof a);size_t n=sizeof a/4;
    if (boundary) { w[n++]=2u<<16|248;w[n++]=20; }
    if (escape) { w[n++]=4u<<16|83;w[n++]=2;w[n++]=21;w[n++]=10; }
    w[n++]=(vol?5u:4u)<<16|61;w[n++]=1;w[n++]=13;w[n++]=10;if(vol)w[n++]=1;
    uint32_t b[]={5u<<16|194,1,14,13,5, 5u<<16|199,1,15,14,6, 1u<<16|253, 1u<<16|56};
    memcpy(w+n,b,sizeof b);return n+sizeof b/4;
}
static void modules(void) {
    uint32_t w[128], before[128];size_t n=fixture(w,0,0,0);
    assert(vk_spirv_int_fold(w,n)==1);
    for(unsigned j=1;j<=3;j++) {n=fixture(w,j==1,j==2,j==3);assert(vk_spirv_int_fold(w,n)==0);}
    n=fixture(w,0,0,0);w[n++]=0;memcpy(before,w,n*4);
    assert(vk_spirv_int_fold(w,n)==-1 && !memcmp(w,before,n*4));
    assert(vk_spirv_int_fold(w,4)==-1);
}
/* High-bit correlations are not valid facts across RelaxedPrecision. The
 * validator-backed Python cases provide complete shaders; these raw streams
 * keep the decoration boundary covered even without SPIRV-Tools installed. */
static size_t decorated_fixture(uint32_t *w, unsigned decoration, unsigned group,
        unsigned target, unsigned floating, unsigned sum) {
    uint32_t header[] = {0x07230203, 0x10300, 0, 64, 0};
    memcpy(w, header, sizeof header);
    size_t n = sizeof header / sizeof *header;
    if (decoration != UINT32_MAX) {
        if (group) {
            w[n++] = 2u << 16 | 73; w[n++] = 30; /* OpDecorationGroup */
        }
        w[n++] = 3u << 16 | 71; w[n++] = group ? 30 : target; w[n++] = decoration;
        if (group) {
            /* Put the affected ID after an unrelated result to exercise all
             * OpGroupDecorate targets, not just its first operand. */
            w[n++] = 4u << 16 | 74; w[n++] = 30; w[n++] = 31; w[n++] = target;
        }
    }
    uint32_t declarations[] = {
        4u << 16 | 21, 1, 32, 1, 4u << 16 | 32, 2, 7, 1,
        3u << 16 | 22, 16, 32, 4u << 16 | 32, 17, 7, 16,
        4u << 16 | 32, 19, 3, 1,
        4u << 16 | 59, 19, 20, 3, 4u << 16 | 59, 19, 21, 3,
        4u << 16 | 43, 1, 3, 0, 4u << 16 | 43, 1, 4, 65536,
        5u << 16 | 54, 1, 7, 0, 8, 2u << 16 | 248, 9,
        4u << 16 | 59, floating ? 17 : 2, 10, 7,
        3u << 16 | 45, 1, 11, 5u << 16 | 128, 1, 31, 11, 3
    };
    memcpy(w + n, declarations, sizeof declarations);
    n += sizeof declarations / sizeof *declarations;
    w[n++] = 5u << 16 | (sum ? 128 : 197); w[n++] = 1; w[n++] = 12;
    w[n++] = sum ? 3 : 11; w[n++] = sum ? 3 : 4;
    unsigned value = 12;
    if (!sum && target != 12) {
        if (floating) {
            w[n++] = 4u << 16 | 124; w[n++] = 16; w[n++] = 18; w[n++] = 12;
        }
        w[n++] = 3u << 16 | 62; w[n++] = 10; w[n++] = floating ? 18 : 12;
        w[n++] = 4u << 16 | 61; w[n++] = floating ? 16 : 1; w[n++] = 13; w[n++] = 10;
        value = 13;
        if (floating) {
            w[n++] = 4u << 16 | 124; w[n++] = 1; w[n++] = 14; w[n++] = 13;
            value = 14;
        }
    }
    if (!sum) {
        w[n++] = 5u << 16 | 199; w[n++] = 1; w[n++] = 15; w[n++] = value; w[n++] = 4;
    }
    uint32_t outputs[] = {
        3u << 16 | 62, 20, value, 3u << 16 | 62, 21, sum ? 12 : 15,
        1u << 16 | 253, 1u << 16 | 56
    };
    memcpy(w + n, outputs, sizeof outputs);
    return n + sizeof outputs / sizeof *outputs;
}
static void decorations(void) {
    uint32_t w[160], before[160];
    for (unsigned floating = 0; floating < 2; floating++) {
        const unsigned targets[] = {floating ? 18 : 12, 10, 13};
        for (unsigned i = 0; i < sizeof targets / sizeof *targets; i++) {
            size_t n = decorated_fixture(w, UINT32_MAX, 0, targets[i], floating, 0);
            assert(vk_spirv_int_fold(w, n) == 1);
            for (unsigned group = 0; group < 2; group++) {
                n = decorated_fixture(w, 0, group, targets[i], floating, 0);
                memcpy(before, w, n * sizeof *w);
                assert(vk_spirv_int_fold(w, n) == 0);
                assert(!memcmp(before, w, n * sizeof *w));
            }
        }
    }
    /* RP as well as opcode-specific wrap decorations must retain the IAdd.
     * NoSignedWrap=4469 and NoUnsignedWrap=4470 are illegal on CopyObject. */
    const unsigned arithmetic_decorations[] = {0, 4469, 4470};
    size_t n = decorated_fixture(w, UINT32_MAX, 0, 12, 0, 1);
    assert(vk_spirv_int_fold(w, n) == 1);
    for (unsigned i = 0; i < sizeof arithmetic_decorations / sizeof *arithmetic_decorations; i++) {
        for (unsigned group = 0; group < 2; group++) {
            n = decorated_fixture(w, arithmetic_decorations[i], group, 12, 0, 1);
            memcpy(before, w, n * sizeof *w);
            assert(vk_spirv_int_fold(w, n) == 0);
            assert(!memcmp(before, w, n * sizeof *w));
        }
    }
}
int main(int argc, char **argv) {
    if(argc==1) {algebra();modules();decorations();puts("integer fold: algebra, boundaries, escape, volatile, malformed PASS; decorations PASS");return 0;}
    if(argc!=3)return 2;
    FILE *in=fopen(argv[1],"rb");if(!in)return 2;
    fseek(in,0,SEEK_END);long bytes=ftell(in);rewind(in);
    if(bytes<20 || bytes%4)return 2;
    uint32_t *words=malloc((size_t)bytes);if(!words)return 2;
    if(fread(words,1,(size_t)bytes,in)!=(size_t)bytes)return 2;
    fclose(in);
    int folded=vk_spirv_int_fold(words,(size_t)bytes/4);if(folded<0)return 3;
    FILE *out=fopen(argv[2],"wb");if(!out)return 2;
    if(fwrite(words,1,(size_t)bytes,out)!=(size_t)bytes)return 2;
    fclose(out);free(words);printf("%d\n",folded);return 0;
}
