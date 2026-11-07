/* Fragment alpha-test epilogue. The original entry remains an ordinary function;
 * a new entry calls it, then rejects from colour zero's alpha. No seed changes.
 * Each comparison gets an immutable module; only the reference is draw data. */
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint32_t op, a, b, location, component, index, storage; } vk_alpha_id;
static uint32_t *vk_spirv_alpha_test(const uint32_t *w, size_t nw, unsigned func,
        unsigned binding, size_t *out_nw) {
    if (nw < 5 || w[0] != 0x07230203 || !w[3] || func < 1 || func > 7) return NULL;
    uint32_t bound=w[3], entry=0, output=0, scalar=0, vector=0, function_type=0, void_type=0;
    uint32_t tfloat=0, tbool=0, tint=0;
    size_t annotations=0, functions=0;
    vk_alpha_id *ids=calloc(bound,sizeof *ids);
    if (!ids) return NULL;
    for (uint32_t i=0;i<bound;i++) ids[i].location=UINT32_MAX;
    for (size_t i=5;i<nw;) {
        uint32_t n=w[i]>>16, op=w[i]&65535;
        if (!n || i+n>nw) goto bad;
        if (op==15 && n>=4 && w[i+1]==4) { if (entry) goto bad; entry=w[i+2]; }
        if (op==71 && n>=4 && w[i+1]<bound) {
            vk_alpha_id *d=&ids[w[i+1]];
            if (w[i+2]==30) d->location=w[i+3];
            if (w[i+2]==31) d->component=w[i+3];
            if (w[i+2]==32) d->index=w[i+3];
        }
        if (!annotations && (op==19 || op==20 || op==21 || op==22 || op==23 || op==28 || op==30 || op==32 || op==33)) annotations=i;
        if (op==19 && n==2) void_type=w[i+1];
        if (op==20 && n==2) tbool=w[i+1];
        if (op==21 && n==4 && w[i+2]==32 && !w[i+3]) tint=w[i+1];
        if (op==22 && n==3 && w[i+2]==32) tfloat=w[i+1];
        if ((op==21 || op==22 || op==23 || op==28 || op==32 || op==33) && n>=3 && w[i+1]<bound) {
            vk_alpha_id *d=&ids[w[i+1]]; d->op=op; d->a=w[i+2]; d->b=n>3?w[i+3]:0;
        }
        if (op==59 && n>=4 && w[i+2]<bound) { ids[w[i+2]].op=op; ids[w[i+2]].a=w[i+1]; ids[w[i+2]].storage=w[i+3]; }
        if (op==54) { if (!functions) functions=i; if (n>=5 && w[i+2]==entry) function_type=w[i+4]; }
        i+=n;
    }
    if (!entry || !annotations || !functions || !void_type || !function_type) goto bad;
    uint32_t indices=0;
    for (uint32_t i=1;i<bound;i++) if (ids[i].op==59 && ids[i].storage==3 && !ids[i].location && !ids[i].index) {
        uint32_t type=ids[i].a;
        if (type>=bound || ids[type].op!=32) goto bad;
        type=ids[type].b; indices=0;
        while (type<bound && ids[type].op==28 && indices<8) { type=ids[type].a; indices++; }
        if (type>=bound) goto bad;
        if (ids[type].op==23 && ids[type].b==4) { vector=type; scalar=ids[type].a; }
        else if (ids[i].component==3) scalar=type;
        else continue;
        if (scalar>=bound || (ids[scalar].op!=22 && ids[scalar].op!=21) || ids[scalar].a!=32) goto bad;
        output=i; break;
    }
    /* No colour-zero export leaves alpha undefined. Do not invent a passing
     * output: a requested test with no comparable alpha cannot be translated. */
    if (!output) goto bad;
    uint32_t wrapper=bound++, zero=bound++, three=bound++;
    int add_float=!tfloat, add_bool=!tbool, add_int=!tint;
    if (add_float) tfloat=bound++;
    if (add_bool) tbool=bound++;
    if (add_int) tint=bound++;
    uint32_t pfloat=0,pout=0;
    for (uint32_t i=1;i<w[3];i++) if (ids[i].op==32) {
        if (ids[i].a==2 && ids[i].b==tfloat) pfloat=i;
        if (ids[i].a==3 && ids[i].b==scalar) pout=i;
    }
    int add_pfloat=!pfloat,add_pout=!pout;
    if (add_pfloat) pfloat=bound++;
    if (add_pout) pout=bound++;
    uint32_t block=bound++, pblock=bound++, uniform=bound++;
    uint32_t start=bound++, called=bound++, ptr=bound++, alpha=bound++, refptr=bound++, ref=bound++;
    uint32_t compare=bound++, kill=bound++, merge=bound++, converted=bound++;
    uint32_t *o=malloc((nw+256)*sizeof *o); if (!o) goto bad;
    size_t k=0;
#define A(...) do { const uint32_t x[]={__VA_ARGS__}; memcpy(o+k,x,sizeof x); k+=sizeof x/sizeof *x; } while (0)
    memcpy(o,w,20); k=5;
    for (size_t i=5;i<nw;) {
        uint32_t n=w[i]>>16, op=w[i]&65535;
        if (i==annotations) {
            A(3u<<16|71,block,2); /* Block */
            A(5u<<16|72,block,0,35,0); /* member Offset */
            A(4u<<16|71,uniform,34,1); A(4u<<16|71,uniform,33,binding);
        }
        if (i==functions) {
            if (add_float) A(3u<<16|22,tfloat,32);
            if (add_bool) A(2u<<16|20,tbool);
            if (add_int) A(4u<<16|21,tint,32,0);
            A(4u<<16|43,tint,zero,0); A(4u<<16|43,tint,three,3);
            A(3u<<16|30,block,tfloat); A(4u<<16|32,pblock,2,block);
            A(4u<<16|59,pblock,uniform,2);
            if (add_pfloat) A(4u<<16|32,pfloat,2,tfloat);
            if (add_pout) A(4u<<16|32,pout,3,scalar);
        }
        /* Alpha rejection must also suppress depth/stencil writes. */
        if (op==16 && n>=3 && w[i+1]==entry && w[i+2]==9) { i+=n; continue; }
        if (op==15 && n>=4 && w[i+2]==entry) {
            o[k++]=((n+(w[1]>=0x00010400))<<16)|15; o[k++]=w[i+1]; o[k++]=wrapper;
            memcpy(o+k,w+i+3,(n-3)*4); k+=n-3;
            if (w[1]>=0x00010400) o[k++]=uniform;
        } else {
            memcpy(o+k,w+i,n*4);
            if ((op==16 || op==331) && n>=3 && w[i+1]==entry) o[k+1]=wrapper;
            k+=n;
        }
        i+=n;
    }
    A(5u<<16|54,void_type,wrapper,0,function_type); A(2u<<16|248,start);
    A(4u<<16|57,void_type,called,entry);
    if (indices || vector) {
        o[k++]=((4+indices+(vector!=0))<<16)|65; o[k++]=pout; o[k++]=ptr; o[k++]=output;
        for (uint32_t j=0;j<indices;j++) o[k++]=zero;
        if (vector) o[k++]=three;
    } else ptr=output;
    A(4u<<16|61,scalar,alpha,ptr);
    if (ids[scalar].op==21) { A(4u<<16|(ids[scalar].b?111:112),tfloat,converted,alpha); alpha=converted; }
    A(5u<<16|65,pfloat,refptr,uniform,zero); A(4u<<16|61,tfloat,ref,refptr);
    /* Ordered comparisons, except NOT_EQUAL: unordered NaNs are not equal. */
    static const uint32_t cmp[8]={0,0,184,180,188,186,183,190};
    if (func==1) A(1u<<16|252);
    else {
        A(5u<<16|cmp[func],tbool,compare,alpha,ref);
        A(3u<<16|247,merge,0); A(4u<<16|250,compare,merge,kill);
        A(2u<<16|248,kill); A(1u<<16|252); A(2u<<16|248,merge); A(1u<<16|253);
    }
    A(1u<<16|56); o[3]=bound; *out_nw=k;
#undef A
    free(ids); return o;
bad:
    free(ids); return NULL;
}
