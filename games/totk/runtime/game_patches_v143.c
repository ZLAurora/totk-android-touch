/* 1.4.3 has a new compiler and frame-controller layout. Every adapted game-code
   site below is checked before any write; older versions use game_patches.c. */
#include "aspect_patch.h"
#include "code_guard.h"
#include "game_patches.h"
#include "game_version.h"
#include "android_thermal.h"
#include "world_survey.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <math.h>
#include <time.h>
#include <float.h>
#include "platform.h"
#if TARGET_OS_OSX
#include <mach/mach.h>
#endif
#if !TARGET_OS_IPHONE
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#endif

/* Sites and their replacement words. The game's own words at each group of sites are checked by their SHA-256
 * (code_guard.h); SITES passes a table's offsets to code_guard. */
typedef struct { uint64_t off; uint32_t replacement; } Patch;
#define SITES(t) &(t)[0].off,sizeof (t)/sizeof *(t),sizeof *(t)
static const Patch fps60[] = {
    {0x2db204, 0x7100055f},
    {0x2db208, 0x1a9fa52a},
    {0x2db22c, 0xd503201f},
    {0x2db360, 0x710005bf},
    {0x2db36c, 0x1a9fc5ac},
    {0x2db340, 0x52800028},
    {0x2db34c, 0x1a9fa555},
};

/* Optional policy experiment: keep the measured-work calculation, candidate
   clamps, callbacks/veto and window setter; change only the native counter
   limits. Fixed experiment limits intentionally replace configuration overrides. */
static void patch_fps60_hysteresis(Module *m) {
    const char *e = getenv("TOTK_FPS60_HYSTERESIS");
    int on = e && !strcmp(e, "1");
    fprintf(stderr, "[totk] TOTK_FPS60_HYSTERESIS=%d: %s\n", on,
            on ? "over limit 6 (raise on seventh consecutive over-budget call), cooldown reload 2" :
                 "native over-limit/cooldown configuration");
    if (!on) return;
#if TARGET_OS_IPHONE
    runtime_fail("TOTK_FPS60_HYSTERESIS requires a runtime-patched main");
#else
    static const Patch sites[] = {
        {0x2db2b8, 0x528000cc}, /* ldr w12,[x20,#0x10] -> mov w12,#6 */
        {0x2db354, 0x5280004c}, /* ldr w12,[x20,#0x14] -> mov w12,#2 */
        {0x2db3f0, 0x52800049}, /* ldr w9,[x20,#0x14] -> mov w9,#2 */
    };
    uint32_t original[3];
    for (unsigned i = 0; i < 3; i++)
        if (sites[i].off + 4 > m->text_end)
            runtime_fail("1.4.3 FPS60 hysteresis site outside text");
    if (!code_guard(m, SITES(sites), original,
                    "ea577c9101d653362c01f832a336c10798bc2e2845bf77fed4b26fafb51197cc"))
        runtime_fail("1.4.3 FPS60 hysteresis original instruction mismatch");
    for (unsigned i = 0; i < 3; i++)
        memcpy(m->base + sites[i].off, &sites[i].replacement, 4);
#endif
}


static _Atomic uint64_t *frame_step_data;
static _Atomic uint32_t *frame_interval_data;
/* The ARM64 producer writes one aligned 32-bit STR after its half-step
 * selector. The host atomically takes that machine-code publication once;
 * the page is coherent shared memory, not a C producer doing non-atomic I/O. */
static _Atomic uint32_t *frame_consumed_ticks;
static _Atomic uint32_t pinned_step_us;
static uint64_t last_present_ns;
static double frame_samples[5];
static unsigned frame_sample_count,frame_sample_next;
static void frame_step_store(float seconds){
    float values[3]={seconds*30.0f,seconds,seconds*60.0f};uint64_t bits;uint32_t interval;
    memcpy(&bits,values,8);memcpy(&interval,values+2,4);
    atomic_store_explicit(frame_step_data,bits,memory_order_release);
    atomic_store_explicit(frame_interval_data,interval,memory_order_release);
}
static void frame_step_capture_clear(void){
    if(frame_consumed_ticks)atomic_store_explicit(frame_consumed_ticks,0,memory_order_relaxed);
}
/* Called before publishing the next step at guest present. Only the adaptive
 * veneer records the actual post-selector s8 passed to FrameRateMgr, in 60 Hz
 * ticks. Taking the word prevents a second present from reusing that step. */
float runtime_frame_step_seconds(void){
    if(!frame_consumed_ticks)return 0;
    uint32_t bits=atomic_exchange_explicit(frame_consumed_ticks,0,memory_order_relaxed);
    if(atomic_load_explicit(&pinned_step_us,memory_order_relaxed))return 0;
    float ticks;memcpy(&ticks,&bits,sizeof ticks);
    return isfinite(ticks)&&ticks>0?ticks/60.0f:0;
}
void runtime_frame_step_pin(double ms){
    uint32_t us=ms>0?(uint32_t)(ms*1000.0+0.5):0;atomic_store(&pinned_step_us,us);
    frame_step_capture_clear();
    if(frame_step_data&&us)frame_step_store((float)(us/1e6));
    fprintf(stderr,"[totk] 1.4.3 frame step %s (pin %.3f ms)\n",us?"pinned":"real time",us/1000.0);
}
void runtime_frame_step_update(uint64_t now_ns){
    if(!frame_step_data||!now_ns){frame_step_capture_clear();return;}
    if(atomic_load_explicit(&pinned_step_us,memory_order_relaxed)){frame_step_capture_clear();last_present_ns=now_ns;return;}
    if(!last_present_ns){frame_step_capture_clear();last_present_ns=now_ns;return;}
    double dt=(double)(now_ns-last_present_ns)*1e-9;last_present_ns=now_ns;
    if(dt>0.25||dt<0.001){frame_step_capture_clear();frame_sample_count=frame_sample_next=0;return;}
    /* Do not raise short presents to 1/120 s: that speeds up high-refresh and
       jittery delivery. Retain the per-interval hitch cap and five-sample mean. */
    if(dt>1.0/15.0)dt=1.0/15.0;
    frame_samples[frame_sample_next++%5]=dt;if(frame_sample_count<5)frame_sample_count++;
    double sum=0;for(unsigned i=0;i<frame_sample_count;i++)sum+=frame_samples[i];
    frame_step_store((float)(sum/frame_sample_count));
}
static uint32_t adrp(unsigned reg,uint64_t pc,uint64_t target){
    int64_t pages=(int64_t)(target>>12)-(int64_t)(pc>>12);
    if(pages<-(1<<20)||pages>=(1<<20))runtime_fail("1.4.3 data outside ADRP range");
    uint32_t imm=(uint32_t)pages&0x1fffff;
    return 0x90000000|((imm&3)<<29)|((imm>>2)<<5)|reg;
}
static uint32_t branch(uint64_t pc,uint64_t target){
    int64_t delta=(int64_t)target-(int64_t)pc;
    if((delta&3)||delta<-(1<<27)||delta>=(1<<27))runtime_fail("1.4.3 patch branch outside range");
    return 0x14000000|(((uint32_t)(delta>>2))&0x3ffffff);
}

/* Benchmark-only native fixed-time policy. The loader authenticates the pinned
 * rewritten main before relocation and this launch-time patch, then seals RX.
 * No request-bank writes or host calls occur while the game is running. */
static int bench_time_parse(const char *e,unsigned *seconds){
    if(!e||!strcmp(e,"off"))return 0;
    if(strlen(e)!=5||e[2]!=':'||e[0]<'0'||e[0]>'9'||e[1]<'0'||e[1]>'9'||
       e[3]<'0'||e[3]>'9'||e[4]<'0'||e[4]>'9')return -1;
    unsigned hour=(unsigned)(e[0]-'0')*10+(unsigned)(e[1]-'0');
    unsigned minute=(unsigned)(e[3]-'0')*10+(unsigned)(e[4]-'0');
    if(hour>23||minute>59)return -1;
    *seconds=hour*3600+minute*60;
    return 1;
}
static const uint64_t bench_time_sites[]={
    0x68c0bc,0x68c0c0,0x68c0c4,0x68c0c8,0x68c0cc,0x68c0d0,0x68c0d4,
    0x68c0d8,0x68c0dc,0x68c0e0,0x68c0e4,0x68c0e8,0x68c0ec,0x68c0f0,
    0x68c0f4,0x68c0f8,0x68c0fc,0x68c100,0x68c104,0x68c108,0x68c10c,
    0x68c110,0x68c114,0x68c118,0x68c11c,0x68c120,0x68c124,0x68c128,
    0x68c540,0x68c544,0x68c760,0x68c764,0x68c768,0x68c76c,0x68c770,
    0x68c774,0x68c778,0x68c290,0x68c294,0x68c298,0x68c29c,0x68c2a0,
    0x68c2a4,0x68c2a8,0x68c2ac,
};
static void patch_bench_time(Module *m){
    unsigned seconds=0;
    int on=bench_time_parse(getenv("TOTK_BENCH_TIME"),&seconds);
    if(on<0){
        fprintf(stderr,"[totk-bench-time] rejected TOTK_BENCH_TIME: expected HH:MM (00:00..23:59) or off; effective off\n");
        return;
    }
    if(!on){
        fprintf(stderr,"[totk-bench-time] TOTK_BENCH_TIME=off (native clock; default)\n");
        return;
    }
#if TARGET_OS_IPHONE
    (void)m;runtime_fail("TOTK_BENCH_TIME requires writable 1.4.3 main");
#else
    /* The text/rodata gap is occupied. Reuse the loader's zero-filled final
     * rewritten-code RX page, leaving +0..127 for grass observation veneers. */
    if(strcmp(m->name,"main")||!m->digest||strlen(m->digest)!=64||
       m->file_size>UINT64_MAX-0x4000||m->text_end>m->file_size||
       m->patch_off<m->key_off||m->patch_off>=m->file_size)
        runtime_fail("1.4.3 benchmark time module identity");
    uint64_t stub=((m->file_size+15)&~15ULL)+128;
    if(stub>m->size||12>m->size-stub||
       stub+12>((m->file_size+0x3fff)&~0x3fffULL))
        runtime_fail("1.4.3 benchmark time veneer layout");
    enum { COUNT=sizeof bench_time_sites/sizeof *bench_time_sites };
    for(unsigned i=0;i<COUNT;i++)
        if(bench_time_sites[i]>m->text_end||m->text_end-bench_time_sites[i]<4||
           bench_time_sites[i]>m->size||m->size-bench_time_sites[i]<4)
            runtime_fail("1.4.3 benchmark time site bounds");
    uint32_t original[COUNT];
    if(!code_guard(m,bench_time_sites,COUNT,sizeof *bench_time_sites,original,
                   "c509b61832d5d59862e3c167499e4aab31b4cbddac5923469ad8aff880b7ca7c"))
        runtime_fail("1.4.3 benchmark time original instruction mismatch");
    for(unsigned i=0;i<12;i++)if(m->base[stub+i])
        runtime_fail("1.4.3 benchmark time occupied veneer padding");
    float value=(float)seconds;
    uint32_t bits;memcpy(&bits,&value,4);
    /* ldr s0,literal; b resume; float literal. Like the replaced scalar load,
     * this writes s0 and zeros its upper vector bits. Every GPR (including
     * x8/base day and x19/clock), other SIMD register, SP/LR, NZCV, FPCR and
     * FPSR is unchanged. No FP arithmetic or scratch register is needed. */
    uint32_t code[]={0x1c000040,branch(stub+4,0x68c770),bits};
    uint32_t redirect=branch(0x68c124,0x68c760),load=branch(0x68c76c,stub);
    /* All guards, space and both branch directions pass before any write. */
    memcpy(m->base+stub,code,sizeof code);
    memcpy(m->base+0x68c76c,&load,4);
    memcpy(m->base+0x68c124,&redirect,4);
    fprintf(stderr,"[totk-bench-time] TOTK_BENCH_TIME=%02u:%02u effective_seconds=%u (native fixed-time path; benchmark/restored saves only)\n",
            seconds/3600,(seconds/60)%60,seconds);
#endif
}

/* Fade/cutscene sites and the 200 ms marker policy are adapted from
 * mamcjr/TOTKRecompAndroid cc4f134 (MIT, Copyright (c) 2026 Peiikai;
 * SavageWindRecomp lineage). Full notice: third_party/licenses/TOTKRecompAndroid.txt.
 *
 * No host calls from guest code: both the veneers and the presenter read the
 * host's virtual counter, NOT the rewritten/deterministic guest clock. Markers
 * publish aligned timestamps with STLR, consumed with LDAR / acquire. The
 * threshold is CNTFRQ / 5, initialized on this host (including embedded iOS).
 * Slots +80 onward follow DRS framegen's +64 snapshot and aspect's +56 float.
 *
 * Generic entry hits are diagnostic ONLY. The admitted subset is the active
 * ExecuteCameraMoveCameraAnimationInstructions AI action: vtable +0x37508e8,
 * name accessor +0x224a028 -> string +0x2c79f06 in the original 1.4.3 main.
 * Its update returns immediately for null +0x20; +0xa5a660..674 marks completion
 * in +0x18 bit31. This is scripted camera animation, not a universal cinematic
 * predicate. A guarded completion-store hook clears both floors immediately. */
enum { CUTSCENE_STAMP_SLOT=80, CUTSCENE_WINDOW_SLOT=88,
       CUTSCENE_MARKERS_SLOT=96, CUTSCENE_REASON_SLOT=128,
       CUTSCENE_GUEST_SLOT=136, CUTSCENE_DATA_END=144 };
enum { CUTSCENE_NONE, CUTSCENE_CAMERA_ACTIVE, CUTSCENE_CAMERA_EMPTY, CUTSCENE_CAMERA_DONE };
static _Atomic uint64_t *cutscene_stamp,*cutscene_markers,*cutscene_reason,*cutscene_guest;
static _Atomic uint64_t cutscene_diag_decision,cutscene_diag_guest;
static _Atomic unsigned cutscene_diag_state=~0u;
static uint64_t cutscene_window;
static uint64_t cutscene_counter(void){
#if defined(__aarch64__)
    uint64_t value;__asm__ volatile("mrs %0, cntvct_el0":"=r"(value));return value;
#else
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000ull+(uint64_t)t.tv_nsec;
#endif
}
unsigned runtime_cutscene_present_ticks(unsigned ticks,uint64_t refresh_ns){
    if(!cutscene_stamp)return ticks;
    uint64_t stamp=atomic_load_explicit(cutscene_stamp,memory_order_acquire);
    uint64_t now=cutscene_counter();
    int active=stamp&&now-stamp<cutscene_window;
    unsigned result=ticks;
    if(active){
        /* Guest intervals are 60-Hz units; the presenter uses physical ticks.
         * At 120 Hz the same 30-fps floor therefore requires FOUR ticks. */
        unsigned floor=refresh_ns?(unsigned)((1000000000ull/30+refresh_ns/2)/refresh_ns):2;
        if(floor<1)floor=1;
        if(result<floor)result=floor;
    }
    uint64_t markers[4];unsigned recent=0;
    for(unsigned i=0;i<4;i++){
        markers[i]=atomic_load_explicit(cutscene_markers+i,memory_order_acquire);
        if(markers[i]&&now-markers[i]<cutscene_window)recent|=1u<<i;
    }
    uint64_t reason=atomic_load_explicit(cutscene_reason,memory_order_acquire);
    uint64_t guest=atomic_load_explicit(cutscene_guest,memory_order_acquire);
    uint64_t decision=((uint64_t)ticks<<32)|result;
    unsigned state=recent|((unsigned)reason<<4)|((unsigned)active<<8);
    uint64_t old_decision=atomic_exchange_explicit(&cutscene_diag_decision,decision,memory_order_relaxed);
    uint64_t old_guest=atomic_exchange_explicit(&cutscene_diag_guest,guest,memory_order_relaxed);
    unsigned old_state=atomic_exchange_explicit(&cutscene_diag_state,state,memory_order_relaxed);
    if(old_decision!=decision||old_guest!=guest||old_state!=state){
        const char *admission=reason==CUTSCENE_CAMERA_ACTIVE?"camera-active":
            reason==CUTSCENE_CAMERA_EMPTY?"camera-empty":reason==CUTSCENE_CAMERA_DONE?"camera-completed":"none";
        fprintf(stderr,"[fps60-cutscene] admission=%s marker=3 decision=%s stamp=%llu age_ticks=%llu window_ticks=%llu guest=%u->%u present=%u->%u recent=%#x markers(stamp/age_ticks) 0@0x45c700(reset)=%llu/%llu 1@0x2b23c7c(type-query)=%llu/%llu 2@0x42c3bc(unclassified-update)=%llu/%llu 3@0xa5a188(scripted-camera)=%llu/%llu\n",
            admission,active?(result!=ticks?"floor":"retain"):stamp?"expired":"no-authority",
            (unsigned long long)stamp,(unsigned long long)(stamp?now-stamp:0),(unsigned long long)cutscene_window,
            (unsigned)guest,(unsigned)(guest>>32),ticks,result,recent,
            (unsigned long long)markers[0],(unsigned long long)(markers[0]?now-markers[0]:0),
            (unsigned long long)markers[1],(unsigned long long)(markers[1]?now-markers[1]:0),
            (unsigned long long)markers[2],(unsigned long long)(markers[2]?now-markers[2]:0),
            (unsigned long long)markers[3],(unsigned long long)(markers[3]?now-markers[3]:0));
    }
    return result;
}
static void patch_fps60_hardening(Module *m){
    const char *fps=getenv("TOTK_FPS60"),*fg=getenv("TOTK_FRAMEGEN");
    const char *enabled=getenv("TOTK_FPS60_HARDENING"),*clamp=getenv("TOTK_FPS60_FADE_CLAMP");
    if(fps&&!strcmp(fps,"0"))return;
    /* TOTK_FPS60_FADE_CLAMP=0 is the matched A/B control: same image and
     * veneers, the four fade sites keep their original fcvtzs. */
    int fades_enabled=!(clamp&&!strcmp(clamp,"0"));
    int floor_enabled=!(fg&&*fg&&strcmp(fg,"0"))&&
                      !(enabled&&!strcmp(enabled,"0"));
    const Patch sites[]={
        {0x0a7ac4,0},{0x95dd24,0},
        {0x6155e4,0},{0x615660,0},
        {0x45c700,0},{0x2b23c7c,0},
        {0x42c3bc,0},{0xa5a188,0},
        {0x2db238,0},{0xa5a670,0},
    };
    static const char sites_sha256[]="41d0c2905dd9466e7ea7dcc0885beedc9ea0c48d5cce30ea543a0b1fdfb2ab43";
    const Patch predicate_guards[]={
        {0xa5a18c,0}, /* null instructions skip the entire action */
        {0xa5a660,0},{0xa5a664,0},
        {0xa5a668,0},{0xa5a66c,0},
        {0xa5a674,0}, /* completion flag and continuation */
    };
    static const char predicates_sha256[]="edba8d506fed22b2f18d5ec5cf90dbbc673c041efaa7e1ccb0f3e0ee488d6300";
    enum { NSITE=sizeof sites/sizeof *sites, NPRED=sizeof predicate_guards/sizeof *predicate_guards };
    /* The veneer word that replays each site's original instruction (fades, markers, camera, interval, exit). */
    static const unsigned replay[NSITE]={0,10,20,30,46,54,62,88,123,98};
    uint64_t data=(m->file_size+0x3fff)&~0x3fffULL;
    uint64_t stub=((m->text_end+15)&~15ULL)+416;
    enum { FADE_WORDS=10, MARK_WORDS=8, CAMERA_WORDS=26, EXIT_WORDS=10, INTERVAL_WORDS=25,
           WORDS=4*FADE_WORDS+3*MARK_WORDS+CAMERA_WORDS+EXIT_WORDS+INTERVAL_WORDS };
    if(data>m->size||CUTSCENE_DATA_END>m->size-data||
       stub>m->size||WORDS*4>m->size-stub||
       stub+WORDS*4>((m->text_end+0xfff)&~0xfffULL))
        runtime_fail("1.4.3 FPS60 hardening patch layout");
    for(unsigned i=0;i<NSITE;i++)
        if(sites[i].off>m->text_end||m->text_end-sites[i].off<4)
            runtime_fail("1.4.3 FPS60 hardening site outside text");
    for(unsigned i=0;i<NPRED;i++)
        if(predicate_guards[i].off>m->text_end||m->text_end-predicate_guards[i].off<4)
            runtime_fail("1.4.3 FPS60 hardening predicate outside text");
    /* Check ALL sites and the complete veneer/data space before writing
     * any member of this group. A different dump fails closed, not half-on. */
    uint32_t orig[NSITE],pred[NPRED];
#if TARGET_OS_IPHONE
    /* Patched at export: each site's original word is the one its embedded veneer replays. */
    for(unsigned i=0;i<NSITE;i++)memcpy(orig+i,m->base+stub+4*replay[i],4);
    if(!code_guard_words(orig,NSITE,sites_sha256))
#else
    if(!code_guard(m,SITES(sites),orig,sites_sha256))
#endif
        runtime_fail("1.4.3 FPS60 hardening original instruction mismatch");
    if(!code_guard(m,SITES(predicate_guards),pred,predicates_sha256))
        runtime_fail("1.4.3 FPS60 hardening predicate original instruction mismatch");
    uint32_t code[WORDS],jumps[NSITE];unsigned n=0;
    for(unsigned i=0;i<4;i++){
        uint64_t at=stub+4*n;unsigned rd=orig[i]&31;
        jumps[i]=branch(sites[i].off,at);
        /* Run ARM FCVTZS itself: keep its saturation, NaN and FPSR behavior.
         * Only a zero result needs examination. Positive finite values <1
         * have sign=0 and bit30=0; NaNs have bit30=1. Integer/bit branches
         * preserve NZCV, FPCR, all SIMD inputs and every other register. */
        uint32_t fade[]={
            orig[i],0x35000100u|rd, /* cbnz wD, return */
            0x1e260000u|rd,0x340000c0u|rd,   /* fmov wD,s0; cbz wD,return (+0) */
            0x37f80080u|rd,0x37f00060u|rd,   /* tbnz sign/bit30,zero */
            0x52800020u|rd,0x14000002,       /* mov wD,1; b return */
            0x52800000u|rd,branch(at+36,sites[i].off+4),
        };
        memcpy(code+n,fade,sizeof fade);n+=FADE_WORDS;
    }
    for(unsigned i=4;i<7;i++){
        uint64_t at=stub+4*n;jumps[i]=branch(sites[i].off,at);
        uint32_t mark[]={
            0xa9bf27e8, /* stp x8,x9,[sp,#-16]!; preserve entry arguments/LR/NZCV */
            adrp(8,(uintptr_t)m->base+at+4,(uintptr_t)m->base+data),
            0x91000108u|((CUTSCENE_MARKERS_SLOT+8*(i-4))<<10), /* diagnostic entry only */
            0xd53be049,0xc89ffd09, /* mrs x9,cntvct_el0; stlr x9,[x8] */
            0xa8c127e8,orig[i],branch(at+28,sites[i].off+4),
        };
        memcpy(code+n,mark,sizeof mark);n+=MARK_WORDS;
    }
    uint64_t at=stub+4*n;jumps[7]=branch(sites[7].off,at);
    /* Preserve x8/x9 and NZCV; replay the original load after recording the
     * marker. Only a nonempty, unfinished scripted camera action admits a
     * floor. Null/completed entries clear it rather than refreshing a lease. */
    uint32_t camera[]={
        0xa9bf27e8,adrp(8,(uintptr_t)m->base+at+4,(uintptr_t)m->base+data),
        0x91000108u|((CUTSCENE_MARKERS_SLOT+24)<<10),0xd53be049,0xc89ffd09,
        0xf9401009,0xb4000149, /* ldr x9,[x0,#0x20]; cbz x9,empty */
        0xb9401809,0x37f80149, /* ldr w9,[x0,#0x18]; tbnz w9,31,completed */
        0x52800029,0x91002108,0xc89ffd09, /* reason=active */
        0xd100c108,0xd53be049,0xc89ffd09,0x14000008, /* publish admitted stamp */
        0x52800049,0x14000002,0x52800069, /* empty / completed */
        0x91002108,0xc89ffd09,0xd100c108,0xc89ffd1f, /* reason; clear stamp */
        0xa8c127e8,orig[7],branch(at+100,sites[7].off+4),
    };
    memcpy(code+n,camera,sizeof camera);n+=CAMERA_WORDS;
    at=stub+4*n;jumps[9]=branch(sites[9].off,at);
    /* The original completion store runs on the same call which finishes the
     * camera action; do not leave a 200ms floor after this demonstrated exit. */
    uint32_t done[]={
        0xa9bf27e8,adrp(8,(uintptr_t)m->base+at+4,(uintptr_t)m->base+data),
        0x91000108u|(CUTSCENE_REASON_SLOT<<10),0x52800069,0xc89ffd09,
        0xd100c108,0xc89ffd1f,0xa8c127e8,orig[9],branch(at+36,sites[9].off+4),
    };
    memcpy(code+n,done,sizeof done);n+=EXIT_WORDS;
    at=stub+4*n;jumps[8]=branch(sites[8].off,at);
    /* All interval-setting paths join here BEFORE the guest state and window
     * are updated. Only w21 and the original w8 destination may change; keep
     * the incoming comparison flags and all other live registers intact. */
    uint32_t interval[]={
        0xa9be2be9,0xd53b4209,0xf9000be9, /* save x9,x10,NZCV (32-byte stack) */
        0xb9001bf5, /* preserve the incoming candidate in the unused stack slot */
        adrp(9,(uintptr_t)m->base+at+16,(uintptr_t)m->base+data),
        0x91000129u|(CUTSCENE_STAMP_SLOT<<10),0xc8dffd2a, /* add; ldar x10,[x9] */
        0xb400012a,0xd53be048,0xcb0a0108, /* cbz stamp,done; now; unsigned age */
        0xf940052a,0xeb0a011f,0x54000082, /* window; cmp age,window; b.hs done */
        0x71000abf,0x54000042,0x52800055, /* cmp w21,2; b.hs done; mov w21,2 */
        0xb9401bea,0xaa15814a, /* pack original/applied guest intervals in x10 */
        0x91000129u|((CUTSCENE_GUEST_SLOT-CUTSCENE_STAMP_SLOT)<<10),0xc89ffd2a,
        0xf9400be9,0xd51b4209,0xa8c22be9, /* restore NZCV,x9,x10,sp */
        orig[8],branch(at+96,sites[8].off+4),
    };
    memcpy(code+n,interval,sizeof interval);
    for(unsigned i=0;i<NSITE;i++)if(code[replay[i]]!=orig[i])runtime_fail("1.4.3 FPS60 hardening veneer layout");
#if TARGET_OS_IPHONE
    for(unsigned i=0;i<NSITE;i++){
        uint32_t got;memcpy(&got,m->base+sites[i].off,4);
        uint32_t expected=(i<4?fades_enabled:floor_enabled)?jumps[i]:orig[i];
        if(got!=expected){
            fprintf(stderr,"[totk] FPS60 hardening main+%#llx: found %08x, expected %08x\n",
                (unsigned long long)sites[i].off,got,expected);
            runtime_fail("1.4.3 FPS60 hardening original instruction mismatch");
        }
    }
#endif
#if TARGET_OS_IPHONE
    if(memcmp(m->base+stub,code,sizeof code))
        runtime_fail("1.4.3 embedded FPS60 hardening veneer mismatch");
#else
    for(unsigned i=0;i<sizeof code;i++)if(m->base[stub+i])
        runtime_fail("1.4.3 FPS60 hardening veneer padding in use");
    for(unsigned i=CUTSCENE_STAMP_SLOT;i<CUTSCENE_DATA_END;i++)if(m->base[data+i])
        runtime_fail("1.4.3 FPS60 hardening data padding in use");
    memcpy(m->base+stub,code,sizeof code);
    for(unsigned i=0;i<sizeof sites/sizeof *sites;i++)
        if(i<4?fades_enabled:floor_enabled)memcpy(m->base+sites[i].off,jumps+i,4);
#endif
    if(floor_enabled){
        uint64_t frequency=1000000000ull;
#if defined(__aarch64__)
        __asm__ volatile("mrs %0, cntfrq_el0":"=r"(frequency));
#endif
        cutscene_window=frequency/5;
        memcpy(m->base+data+CUTSCENE_WINDOW_SLOT,&cutscene_window,8);
        cutscene_stamp=(_Atomic uint64_t *)(void *)(m->base+data+CUTSCENE_STAMP_SLOT);
        cutscene_markers=(_Atomic uint64_t *)(void *)(m->base+data+CUTSCENE_MARKERS_SLOT);
        cutscene_reason=(_Atomic uint64_t *)(void *)(m->base+data+CUTSCENE_REASON_SLOT);
        cutscene_guest=(_Atomic uint64_t *)(void *)(m->base+data+CUTSCENE_GUEST_SLOT);
        atomic_store_explicit(cutscene_stamp,0,memory_order_release);
    }
    fprintf(stderr,"[totk] 1.4.3 FPS60: %s; %s\n",
        fades_enabled?"4 positive fade clamps":"fade clamps OFF, original fcvtzs (TOTK_FPS60_FADE_CLAMP=0)",
        floor_enabled?"4 diagnostic markers; active scripted-camera subset only, exit clear/200ms expiry, guest/present 30fps floor (TOTK_FPS60_HARDENING=0 opts out)":
                      "cutscene floor off (framegen or TOTK_FPS60_HARDENING=0)");
}

/* The FPS60 selects override stored min=2. Clamp their RESULTS, not the stored
 * bounds, so the game's own candidate/current update and NVN interval setter
 * stay authoritative. Installed only with startup TOTK_THERMAL_STEP=1; off
 * retains the original seven-word policy and allocates no thermal veneer.
 * Each veneer preserves incoming NZCV and x16/x17, replaying its select first.
 * The aligned literal is a single machine-word publication, with no side data. */
#if (defined(__ANDROID__) && defined(TOTK_ANDROID_APP)) || defined(TOTK_THERMAL_PATCH_TEST)
static void patch_thermal_step(Module *m) {
    const char *enabled = getenv("TOTK_THERMAL_STEP");
    if (!enabled || strcmp(enabled, "1")) return;
    const unsigned indices[] = {1, 6, 4};
    const unsigned destinations[] = {10, 21, 12};
    enum { WORDS = 9, COUNT = 3, FLOOR_SLOT = 144 };
    uint64_t stub = ((m->text_end + 15) & ~15ULL) + 916;
    uint64_t data = ((m->file_size + 0x3fff) & ~0x3fffULL) + FLOOR_SLOT;
    if (stub > m->size || COUNT * WORDS * 4 > m->size - stub ||
        stub + COUNT * WORDS * 4 > ((m->text_end + 0xfff) & ~0xfffULL) ||
        data > m->size || 4 > m->size - data)
        runtime_fail("1.4.3 thermal veneer layout");
    for (unsigned i = 0; i < COUNT; i++) {
        uint32_t word;
        memcpy(&word, m->base + fps60[indices[i]].off, 4);
        if (word != fps60[indices[i]].replacement)
            runtime_fail("1.4.3 thermal FPS60 select mismatch");
    }
    for (unsigned i = 0; i < COUNT * WORDS * 4; i++)
        if (m->base[stub + i]) runtime_fail("1.4.3 thermal veneer padding in use");
    for (unsigned i = 0; i < 4; i++)
        if (m->base[data + i]) runtime_fail("1.4.3 thermal data padding in use");
    uint32_t code[COUNT * WORDS], jumps[COUNT];
    for (unsigned i = 0; i < COUNT; i++) {
        uint64_t at = stub + i * WORDS * 4;
        int64_t delta = (int64_t)data - (int64_t)(at + 12);
        if ((delta & 3) || delta < -(1 << 20) || delta >= (1 << 20))
            runtime_fail("1.4.3 thermal literal outside range");
        unsigned d = destinations[i], index = indices[i];
        uint32_t veneer[] = {
            fps60[index].replacement,
            0xa9bf47f0, 0xd53b4210, /* save x16/x17; mrs x16,nzcv */
            0x18000011u | (((uint32_t)(delta >> 2) & 0x7ffff) << 5),
            0x6b11001fu | (d << 5), /* cmp wD,w17 (floor) */
            0x1a912000u | (d << 5) | d, /* csel wD,wD,w17,hs */
            0xd51b4210, 0xa8c147f0, /* msr nzcv,x16; restore x16/x17 */
            branch(at + 32, fps60[index].off + 4),
        };
        memcpy(code + i * WORDS, veneer, sizeof veneer);
        jumps[i] = branch(fps60[index].off, at);
    }
    uint32_t floor = 1;
    memcpy(m->base + data, &floor, 4);
    memcpy(m->base + stub, code, sizeof code);
    for (unsigned i = 0; i < COUNT; i++)
        memcpy(m->base + fps60[indices[i]].off, jumps + i, 4);
    runtime_thermal_bind((_Atomic uint32_t *)(void *)(m->base + data));
}
#endif
/* The actor animation update has a second, unscaled clock for cloth/hair
 * (ordinary animation already receives FrameRateMgr's real-time step). Its
 * explicit time-factor path still constructs {factor / 30, factor} every
 * rendered frame. At 120 FPS that advances four seconds per real second.
 * Correct only that clock, retaining pauses, slow motion and the fallback to
 * the ordinary clock. 1.4.3 inlines the update in two actor paths; two other
 * callers share the out-of-line update at +0x2a0e40. Its third caller already
 * passes measured time and MUST NOT be scaled a second time.
 *
 * The inline veneers use only the original destination s register and x8,
 * which the next original load overwrites. At the two call boundaries x8/s4
 * are caller-saved scratch; the veneer tail-calls the original function with
 * its original LR. No stack, flags, ordinary-clock arguments or callee-saved
 * registers are changed. Stubs follow the four 32-byte aspect veneers. */
static void patch_secondary_animation(Module *m,uint64_t data,uint64_t stub){
    const char *enabled=getenv("TOTK_CLOTH_REALTIME");
    if(enabled&&!strcmp(enabled,"0"))return;   /* diagnostic comparison */
    const Patch sites[]={
        {0x246eac,0}, /* fmul s0,s10,s0: inline clock */
        {0x345904,0}, /* fmul s1,s0,s1: inline clock */
        {0x24944c,0}, /* bl 0x2a0e40: actor with transform override */
        {0x28bc98,0}, /* bl 0x2a0e40: includes both transform branches */
    };
    enum { WORDS=19 };
    if(stub+WORDS*4>((m->text_end+0x3fff)&~0x3fffULL)||data+8>m->size)
        runtime_fail("1.4.3 secondary animation patch layout");
    for(unsigned i=0;i<sizeof sites/sizeof *sites;i++)
        if(sites[i].off+4>m->text_end)runtime_fail("1.4.3 secondary animation site outside text");
    /* The veneers run the two inline fmuls themselves (words 2/5 and 9/12). */
    uint32_t orig[4]={0};
#if TARGET_OS_IPHONE
    memcpy(orig,m->base+stub+8,4);memcpy(orig+1,m->base+stub+36,4);   /* patched at export: the embedded replays */
#else
    if(!code_guard(m,SITES(sites),orig,"4fdf9536c07de6dfae5226265e5da2643486e16b932fe8d8dd838b09b66ba304"))
        runtime_fail("1.4.3 secondary animation instruction mismatch");
#endif
    uint32_t code[WORDS]={
        adrp(8,(uintptr_t)m->base+stub,(uintptr_t)m->base+data),
        0xbd400100,orig[0],0xbd0367e0, /* ratio * factor -> sp+0x364 */
        0xbd400500,orig[0],branch(stub+24,0x246eb0), /* seconds * factor */
        adrp(8,(uintptr_t)m->base+stub+28,(uintptr_t)m->base+data),
        0xbd400101,orig[1],0xbd0117e1, /* ratio * factor -> sp+0x114 */
        0xbd400501,orig[1],branch(stub+52,0x345908),
        adrp(8,(uintptr_t)m->base+stub+56,(uintptr_t)m->base+data),
        0xbd400104,0x1e240842,0x1e240863, /* s2/s3 *= measured 30 Hz ratio */
        branch(stub+72,0x2a0e40),
    };
    uint32_t jumps[]={branch(sites[0].off,stub),branch(sites[1].off,stub+28),
        branch(sites[2].off,stub+56)|0x80000000u,branch(sites[3].off,stub+56)|0x80000000u};
#if TARGET_OS_IPHONE
    for(unsigned i=0;i<sizeof sites/sizeof *sites;i++){
        uint32_t word;memcpy(&word,m->base+sites[i].off,4);
        if(word!=jumps[i])runtime_fail("1.4.3 secondary animation instruction mismatch");
    }
#endif
#if TARGET_OS_IPHONE
    if(memcmp(m->base+stub,code,sizeof code))runtime_fail("1.4.3 embedded secondary animation veneer mismatch");
#else
    for(unsigned i=0;i<sizeof code;i++)if(m->base[stub+i])runtime_fail("1.4.3 secondary animation padding in use");
    memcpy(m->base+stub,code,sizeof code);
    for(unsigned i=0;i<sizeof sites/sizeof *sites;i++)memcpy(m->base+sites[i].off,jumps+i,4);
#endif
    fprintf(stderr,"[totk] 1.4.3 cloth/hair clock: real-time seconds and ratio at 4 guarded actor sites; ordinary animation and time factors retained\n");
}
static void patch_realtime(Module *m){
    const char *enabled=getenv("TOTK_REALTIME");if(enabled&&!strcmp(enabled,"0"))return;
    uint64_t data=(m->file_size+0x3fff)&~0x3fffULL;
    /* Keep executable veneers inside the text segment. iOS embeds x0 as a
     * signed RX segment, so these bytes must be emitted by export-ios rather
     * than written after the app starts. */
    uint64_t stub=(m->text_end+15)&~15ULL;
    int capture=0;
#if !TARGET_OS_IPHONE
    const char *fg=getenv("TOTK_FRAMEGEN");
    capture=fg&&!strcmp(fg,"adaptive")&&!getenv("TOTK_EXPORT_IOS");
#endif
    size_t veneer_bytes=capture?20:16;
    if(data+0x4000!=m->size||stub+veneer_bytes>((m->text_end+0x3fff)&~0x3fffULL)||
       (capture&&stub+veneer_bytes>((m->text_end+0xfff)&~0xfffULL)))runtime_fail("1.4.3 frame-step patch layout");
    const Patch guards[]={
        {0x3e5658,0},{0x2daa54,0},{0x2daa58,0},
        {0x2daa6c,0},{0x2daa70,0},{0x2daa74,0},{0x2daa78,0}};
    uint32_t orig[sizeof guards/sizeof *guards]={0};
#if TARGET_OS_IPHONE
    memcpy(orig,m->base+stub+8,4);   /* patched at export: the fmul the embedded veneer replays */
#else
    if(!code_guard(m,SITES(guards),orig,"f74853ffb1f80f8dc0a7ebbb175eed01aade446f57eeb0f7e91d54c28b157ade"))
        runtime_fail("1.4.3 real-time original instruction mismatch");
    for(size_t i=0;i<veneer_bytes;i++)if(m->base[stub+i])runtime_fail("1.4.3 frame-step veneer padding not empty");
#endif
    /* Replace the nominal 60/FPS interval BEFORE the original producer applies
     * its 0.5/1.0 selector (sys+0x1da bit 0). Both FrameRateMgr call sites use
     * this s8 value. Hooking the callee entry instead would discard that scale
     * and affect any other intentional interval setter. Keep the original fmul
     * and all FrameRateMgr ratio/inverse/seconds/FPS arithmetic untouched.
     * x9 contains live task flags here: use x8, which the next ldxr overwrites.
     * s0 is dead until the call sites copy s8 into it again. */
    uint32_t veneer[]={adrp(8,(uintptr_t)m->base+stub,(uintptr_t)m->base+data),0xbd400900,orig[0],branch(stub+12,0x3e565c),0};
    /* Keep every legacy/iOS byte unchanged. Exact adaptive alone captures the
     * selected interval, without changing the selector or callee arithmetic.
     * Twenty bytes end exactly where the output-size veneer (+20..+48) begins. */
    if(capture){veneer[3]=0xbd000d08;veneer[4]=branch(stub+16,0x3e565c);} /* str s8,[x8,#12] */
 #if TARGET_OS_IPHONE
    if(memcmp(m->base+stub,veneer,veneer_bytes))runtime_fail("1.4.3 embedded frame-step veneer mismatch");
 #else
    memcpy(m->base+stub,veneer,veneer_bytes);
 #endif
    uint32_t words[]={branch(0x3e5658,stub),adrp(9,(uintptr_t)m->base+0x2daa54,(uintptr_t)m->base+data),0xbd400120,0xd503201f,0xd503201f,0xd503201f,0xd503201f};
 #if TARGET_OS_IPHONE
    for(unsigned i=0;i<sizeof guards/sizeof *guards;i++){uint32_t got;memcpy(&got,m->base+guards[i].off,4);if(got!=words[i])runtime_fail("1.4.3 embedded real-time patch mismatch");}
 #else
    for(unsigned i=0;i<sizeof guards/sizeof *guards;i++)memcpy(m->base+guards[i].off,words+i,4);
 #endif
    frame_step_data=(_Atomic uint64_t *)(void *)(m->base+data);
    frame_interval_data=(_Atomic uint32_t *)(void *)(m->base+data+8);frame_step_store(1.0f/60.0f);
    atomic_store_explicit((_Atomic uint32_t *)(void *)(m->base+data+12),0,memory_order_relaxed);
    frame_consumed_ticks=capture?(_Atomic uint32_t *)(void *)(m->base+data+12):NULL;
    patch_secondary_animation(m,data,stub+176);
    fprintf(stderr,"[totk] 1.4.3 real-time frame step: 7 guarded sites, producer veneer +%#llx; original half-step selector and ratio/inverse/seconds/FPS calculations retained\n",(unsigned long long)stub);
}
/* High frame rates (#61, sky-island trampolines): the contact code at main+0x562a64 scales the bounce vector by 0.1
   when X+0x28 > 100, X being the time object 0x3e58a4 feeds (X+0x28 = 30 / (30 Hz ratio * time scale), X+0x38 the
   ratio). The shipping game steps at 30 Hz (ratio 1), so the check means "time slowed below 0.3"; above 100 FPS it also
   fires with time running normally. The veneer compares X+0x28 * X+0x38 = 30 / time scale instead: the same value at
   30 Hz, independent of the frame rate. TOTK_HIGHFPS_FIX=0 keeps the original check. */
static void patch_slowmo_bounce(Module *m){
    const char *e=getenv("TOTK_HIGHFPS_FIX");if(e&&!strcmp(e,"0"))return;
    /* after the ultrawide veneers (+48, 4 x 32 bytes) and the cloth/hair clock (+176, 19 words); read-only data starts
       at the next 4 KiB page */
    uint64_t stub=((m->text_end+15)&~15ULL)+256;
    if(stub+20>((m->text_end+0xfff)&~0xfffULL))runtime_fail("1.4.3 bounce veneer layout");
    uint32_t orig[3];
    static const char guards_sha256[]="4143eb6755121e8dd299b29cf8ab552068a1038a8807f37bc3ac72e556e77143";
#if TARGET_OS_IPHONE
    /* patched at export: the first original word is the one the embedded veneer replays */
    memcpy(orig,m->base+stub,4);memcpy(orig+1,m->base+0x562a68,8);
    if(!code_guard_words(orig,3,guards_sha256))runtime_fail("1.4.3 embedded bounce patch mismatch");
#else
    const Patch guards[]={{0x562a64,0},{0x562a68,0},{0x562a6c,0}};
    if(!code_guard(m,SITES(guards),orig,guards_sha256))runtime_fail("1.4.3 bounce check: original instruction mismatch");
#endif
    uint32_t veneer[]={orig[0],      /* the original ldr s0, [x9, #0x28]: X+0x28 */
        0xbd403921,                  /* ldr s1, [x9, #0x38]: the 30 Hz ratio (s1 is set again by the next original instruction) */
        0x1e210800,                  /* fmul s0, s0, s1 */
        orig[1],                     /* the original mov w9, #0x42c80000 (100.0) */
        branch(stub+16,0x562a6c)};
    uint32_t word=branch(0x562a64,stub);
#if TARGET_OS_IPHONE
    uint32_t got;memcpy(&got,m->base+0x562a64,4);
    if(got!=word||memcmp(m->base+stub,veneer,sizeof veneer))runtime_fail("1.4.3 embedded bounce patch mismatch");
#else
    for(unsigned i=0;i<sizeof veneer;i++)if(m->base[stub+i])runtime_fail("1.4.3 bounce veneer padding not empty");
    memcpy(m->base+stub,veneer,sizeof veneer);memcpy(m->base+0x562a64,&word,4);
#endif
    fprintf(stderr,"[totk] 1.4.3 high-FPS fix: trampoline slow-motion check uses the time scale, veneer +%#llx\n",(unsigned long long)stub);
}

/* Optional renderer-only reductions. Each group verifies every site (including
 * unchanged dependent instructions) before touching code. No simulation,
 * streaming, object activation or FOV policy is patched; the optional
 * directional-cascade cap changes shadow coverage only. */
static void verify_render_sites(Module *m,const Patch *sites,size_t count,uint32_t *words,const char *sha256){
    for(size_t i=0;i<count;i++)
        if(sites[i].off>m->text_end||m->text_end-sites[i].off<4||
           sites[i].off>m->size||m->size-sites[i].off<4)
            runtime_fail("1.4.3 renderer patch outside text");
    if(!code_guard(m,&sites[0].off,count,sizeof *sites,words,sha256)){
        fprintf(stderr,"[totk] renderer patch main+%#llx (%zu sites): the instructions are not the expected ones\n",
                (unsigned long long)sites[0].off,count);
        runtime_fail("1.4.3 renderer patch original instruction mismatch");
    }
#if TARGET_OS_IPHONE
    runtime_fail("optional renderer patches require writable code (Android/Mac)");
#endif
}
static void patch_shadow_res(Module *m){
    const char *e=getenv("TOTK_SHADOW_RES");
    if(!e||!*e||!strcmp(e,"0"))return;
    unsigned n;
    if(!strcmp(e,"288"))n=288;
    else if(!strcmp(e,"256"))n=256;
    else runtime_fail("TOTK_SHADOW_RES must be 0, 288 or 256");
    const Patch sites[]={
        {0x6835a4,0x5280000b|(n<<5)}, /* mov w11,#N */
        {0x6835a8,0x14000017},        /* skip width modulation */
        {0x683604,0},                /* original round-up to 8 */
        {0x683608,0},
        {0x68360c,0},
        {0x683610,0},                /* original packed size / 8 */
        {0x683614,0},                /* original matching size */
        {0x683618,0},
        {0x68361c,0},                /* pack height from size / 8 */
        {0x683620,0},                /* packed width+height */
        {0x683624,0},                /* dependent width */
        {0x683628,0},                /* height from packed state */
        {0x68362c,0},
        {0x683630,0},
        {0x683634,0},
        {0x683638,0},                /* dependent aligned height */
    };
    uint32_t orig[sizeof sites/sizeof *sites];
    verify_render_sites(m,sites,sizeof sites/sizeof *sites,orig,"1d9a11cd182e68bf029ca220faf8ca1e446b8bbb3324b88f6a3da6c8a64df603");
    for(unsigned i=0;i<2;i++)memcpy(m->base+sites[i].off,&sites[i].replacement,4);
    fprintf(stderr,"[totk] 1.4.3 TOTK_SHADOW_RES=%u applied: main+0x6835a4 %08x -> %08x, +0x6835a8 %08x -> %08x; original dependent width/height packing retained; check cascade coverage visually\n",
            n,orig[0],sites[0].replacement,orig[1],sites[1].replacement);
}
static void patch_fxaa(Module *m){
    const char *e=getenv("TOTK_FXAA");
    if(!e||!*e||!strcmp(e,"1"))return;
    if(strcmp(e,"0"))runtime_fail("TOTK_FXAA must be 0 or 1");
    const Patch site={0x586944,0x3906301f};
    uint32_t orig;
    verify_render_sites(m,&site,1,&orig,"67a0d425317efa36a5b590557e57a87ac540de0eeef04c1076b974aae86a42cb");
    memcpy(m->base+site.off,&site.replacement,4);
    fprintf(stderr,"[totk] 1.4.3 TOTK_FXAA=0 applied: main+0x586944 %08x -> %08x (effective flag store, all inheritance paths)\n",
            orig,site.replacement);
}
static void patch_far_clamp(Module *m){
    const char *e=getenv("TOTK_FAR_CLAMP");
    if(!e||!*e||!strcmp(e,"0"))return;
    char *end;float cap=strtof(e,&end);
    if(end==e||*end||!isfinite(cap)||cap<=0)
        runtime_fail("TOTK_FAR_CLAMP must be 0 or finite positive metres");
    /* The graphics camera controller, not actor/physics/streaming distances.
     * q1 is live for subsequent FOV math: save all 128 bits, not only s1.
     * No GPR, LR or NZCV is touched. Keep the controller's render-camera far
     * value consistent with the projection it publishes at +0x628c38. */
    uint64_t stub=((m->text_end+15)&~15ULL)+288;
    const Patch sites[]={
        {0x628c20,0}, {0x628c28,0},
        {0x628c30,0}, {0x628c34,0},
        {0x628c38,0}, {0x628c3c,0},
    };
    enum { CODE_WORDS=8 };
    if(stub>m->size||CODE_WORDS*4>m->size-stub||
       stub+CODE_WORDS*4>((m->text_end+0xfff)&~0xfffULL))
        runtime_fail("1.4.3 far-clamp veneer layout");
    uint32_t orig[sizeof sites/sizeof *sites];
    verify_render_sites(m,sites,sizeof sites/sizeof *sites,orig,"4f7170fb8a16575f6283b66b73e7edcd78826054070c68b29fa22ca2c5195202");
    uint32_t bits;memcpy(&bits,&cap,4);
    uint32_t code[CODE_WORDS]={
        0x3c9f0fe1,             /* str q1,[sp,#-16]! */
        orig[2],                /* the original ldr s0,[x19,#0x138] */
        0x1c0000a1,             /* ldr s1,stub+28 */
        0x1e215800,             /* fmin s0,s0,s1 */
        0xbd013a60,             /* str s0,[x19,#0x138] */
        0x3cc107e1,             /* ldr q1,[sp],#16 */
        branch(stub+24,0x628c34),bits,
    };
    uint32_t jump=branch(0x628c30,stub);
    for(size_t i=0;i<sizeof code;i++)if(m->base[stub+i])
        runtime_fail("1.4.3 far-clamp veneer padding not empty");
    memcpy(m->base+stub,code,sizeof code);
    memcpy(m->base+0x628c30,&jump,4);
    fprintf(stderr,"[totk] 1.4.3 TOTK_FAR_CLAMP=%.9g applied: render far=min(original,cap), main+0x628c30 %08x -> %08x, veneer +%#llx; gameplay/FOV unchanged\n",
            cap,orig[2],jump,(unsigned long long)stub);
}
/* Directional shadow cascades (default four, configured at shadow object +0xfc) are walked once per cascade
 * by the GPU translator, so each one removed takes its caster draws off the CPU-bound recording thread.
 * The update at main+0x68318c holds the configured count in w15 and the active per-context count in w16;
 * every path (including the flag-forced 2/3 paths at +0x683acc/+0x683ad8/+0x683af8/+0x683798) rejoins at
 * +0x6834fc and passes +0x683500 before either is published (+0x683504.. metadata, +0x68362c descriptor
 * count, then the split-distance loop up to count-1). Cap both coherently there: w6 is dead until the replayed
 * original mov, and NZCV is redefined by the cmp at +0x683518 before its csel reads it. */
static void patch_shadow_cascades(Module *m){
    const char *e=getenv("TOTK_SHADOW_CASCADES");
    if(!e||!*e||!strcmp(e,"0"))return;
    if(strlen(e)!=1||e[0]<'1'||e[0]>'3')runtime_fail("TOTK_SHADOW_CASCADES must be 0 (unchanged), 1, 2 or 3");
    unsigned n=(unsigned)(e[0]-'0');
    uint64_t stub=((m->text_end+15)&~15ULL)+352;
    const Patch sites[]={
        {0x683418,0}, /* ldr w15,[x0,#0xfc]: configured count */
        {0x6834f8,0}, /* mov w16,w15: ordinary active count */
        {0x6834fc,0}, /* rejoin point of every forced-count path */
        {0x683500,0}, /* mov w6,#0xbb0: replaced, replayed in the veneer */
        {0x683504,0}, /* strb w16,[x17,#0xc]!: first publication */
        {0x683518,0}, /* cmp w4,w1: flags redefined before the csel */
        {0x68351c,0}, /* csel w4,w1,wzr,hi */
        {0x68362c,0}, /* str w15,[x16,#0x4d8]: descriptor count */
    };
    enum { CODE_WORDS=7 };
    if(stub>m->size||CODE_WORDS*4>m->size-stub||
       stub+CODE_WORDS*4>((m->text_end+0xfff)&~0xfffULL))
        runtime_fail("1.4.3 shadow-cascade veneer layout");
    uint32_t orig[sizeof sites/sizeof *sites];
    verify_render_sites(m,sites,sizeof sites/sizeof *sites,orig,"b62bb58073d00daa93e9cc57d25aa6ec65cfa3abd023a815bcd951f3e9fa6541");
    uint32_t code[CODE_WORDS]={
        0x52800006|(n<<5),       /* mov w6,#N */
        0x6b0601ff,              /* cmp w15,w6 */
        0x1a8fc0cf,              /* csel w15,w6,w15,gt */
        0x6b06021f,              /* cmp w16,w6 */
        0x1a90c0d0,              /* csel w16,w6,w16,gt */
        orig[3],                 /* the original mov w6,#0xbb0 */
        branch(stub+24,0x683504),
    };
    uint32_t jump=branch(0x683500,stub);
    for(size_t i=0;i<sizeof code;i++)if(m->base[stub+i])
        runtime_fail("1.4.3 shadow-cascade veneer padding not empty");
    memcpy(m->base+stub,code,sizeof code);
    memcpy(m->base+0x683500,&jump,4);
    fprintf(stderr,"[totk] 1.4.3 TOTK_SHADOW_CASCADES=%u applied: configured and active directional cascades capped at %u, main+0x683500 %08x -> %08x, veneer +%#llx; check far-shadow coverage visually\n",
            n,n,orig[3],jump,(unsigned long long)stub);
}
/* Point-light depth shadows: Light_DepthShadowPointNum (accessor main+0xb87550 returns the selected setting's
 * +0x4c) sizes the light system's point-shadow pool at main+0xb866c4 (result kept at owner +0xb0). A pool
 * initialized with a count below one creates no elements (+0xb899f8). Cap the budget, never raise it: each
 * shadowed point light is a separate set of caster passes for the recording thread. 0 is a diagnostic only:
 * not every later zero-capacity acquisition path has been proven harmless. w9 is dead here (caller-saved,
 * unused before the call at +0xb866c4); nothing reads NZCV before that call. */
static void patch_point_shadows(Module *m){
    const char *e=getenv("TOTK_POINT_SHADOWS");
    if(!e||!*e)return;
    char *end;long n=strtol(e,&end,10);
    if(end==e||*end||n<0||n>64)runtime_fail("TOTK_POINT_SHADOWS must be unset or 0..64 (budget cap; 0 is a diagnostic)");
    uint64_t stub=((m->text_end+15)&~15ULL)+384;
    const Patch sites[]={
        {0xb866b0,0}, /* bl accessor 0xb87550 */
        {0xb866b4,0}, /* ldr w1,[x0]: budget, replaced and replayed */
        {0xb866b8,0}, /* mov x0,x23 */
        {0xb866bc,0}, /* mov x2,x20 */
        {0xb866c0,0}, /* mov x3,x19 */
        {0xb866c4,0}, /* bl pool init 0xb89878 */
        {0xb87574,0}, /* accessor: add x0,x19,#0x4c */
    };
    enum { CODE_WORDS=5 };
    if(stub>m->size||CODE_WORDS*4>m->size-stub||
       stub+CODE_WORDS*4>((m->text_end+0xfff)&~0xfffULL))
        runtime_fail("1.4.3 point-shadow veneer layout");
    uint32_t orig[sizeof sites/sizeof *sites];
    verify_render_sites(m,sites,sizeof sites/sizeof *sites,orig,"bcc839fc979a4cc351a043f9050e56816c9364bb871d50953d740d14b2bcc78a");
    uint32_t code[CODE_WORDS]={
        orig[1],                 /* the original ldr w1,[x0] */
        0x52800009|((uint32_t)n<<5), /* mov w9,#N */
        0x6b09003f,              /* cmp w1,w9 */
        0x1a81c121,              /* csel w1,w9,w1,gt: min(original,N) */
        branch(stub+16,0xb866b8),
    };
    uint32_t jump=branch(0xb866b4,stub);
    for(size_t i=0;i<sizeof code;i++)if(m->base[stub+i])
        runtime_fail("1.4.3 point-shadow veneer padding not empty");
    memcpy(m->base+stub,code,sizeof code);
    memcpy(m->base+0xb866b4,&jump,4);
    fprintf(stderr,"[totk] 1.4.3 TOTK_POINT_SHADOWS=%ld applied: point-shadow pool budget=min(original,%ld), main+0xb866b4 %08x -> %08x, veneer +%#llx%s\n",
            n,n,orig[1],jump,(unsigned long long)stub,n?"":"; 0 is a diagnostic (zero-capacity paths unproven)");
}

/* The predictor's GPU stress time unit (16666.666 us, a float in main's data). */
static const Patch DRS_TIME_UNIT[]={{0x2d02094,0}};
static const char DRS_TIME_UNIT_SHA256[]="2ba64480db226174427cab3b3d43e54926077f4c9ef770207277488fe4386089";
/* DRS consumes GPU queue-span stress in percent of one 60-Hz tick, NOT ms:
 * main+a1b90..a1bb8 converts timestamp ns to us, then *100/16666.666.
 * The native predictor at +584d70 compares its projected cost with this+0xc.
 * Cap that comparison operand to 100 (16.667 ms); keep an already tighter
 * budget, actual measurements, cost mirror, scale palette and hysteresis.
 * Both ordinary and reset decisions call this one predictor. */
static void patch_drs60(Module *m){
    const char *e=getenv("TOTK_DRS60");
    if(!e||!*e||!strcmp(e,"0"))return;
    if(strcmp(e,"1"))runtime_fail("TOTK_DRS60 must be 0 or 1");
    const char *fps=getenv("TOTK_FPS60");
    if(fps&&!strcmp(fps,"0")){
        fprintf(stderr,"[totk] TOTK_DRS60=1 ignored: shipping 30 FPS controller (TOTK_FPS60=0)\n");
        return;
    }
    uint64_t stub=((m->text_end+15)&~15ULL)+320;
    const Patch sites[]={
        {0x584e84,0}, /* ldr s17,[x8,#0xc]: native budget */
        {0x584e88,0}, /* fcmp s21,s17: predicted GPU stress */
        {0x584e8c,0}, /* str s21,[x8,#0x10]: actual cost mirror */
        {0x584e90,0}, /* below-budget full-quality path */
        {0x584790,0}, /* ordinary update calls predictor */
        {0x584b68,0}, /* reset update calls same predictor */
        {0x0a1bb0,0}, /* GPU us * 100 */
        {0x0a1bb4,0}, /* / 16666.666 us */
    };
    enum { CODE_WORDS=5 };
    if(stub>m->size||CODE_WORDS*4>m->size-stub||
       stub+CODE_WORDS*4>((m->text_end+0xfff)&~0xfffULL))
        runtime_fail("1.4.3 DRS60 veneer layout");
    uint32_t orig[sizeof sites/sizeof *sites];
    verify_render_sites(m,sites,sizeof sites/sizeof *sites,orig,"b33749330db7c306c70881258fc4bbb70b0913391d6694840f31f60677e93c68");
    if(DRS_TIME_UNIT[0].off>m->file_size||m->file_size-DRS_TIME_UNIT[0].off<4||DRS_TIME_UNIT[0].off>m->size||m->size-DRS_TIME_UNIT[0].off<4)
        runtime_fail("1.4.3 DRS60 time-unit constant outside module");
    uint32_t unit;
    if(!code_guard(m,SITES(DRS_TIME_UNIT),&unit,DRS_TIME_UNIT_SHA256))runtime_fail("1.4.3 DRS60 GPU stress time-unit mismatch");
    /* s22 is predictor-local caller-saved scratch: each candidate loop writes
     * it before reading it. s17 was the original destination; no GPR, LR,
     * stack or flags is changed before the original comparison resumes. */
    uint32_t code[CODE_WORDS]={
        orig[0],                /* the original ldr s17,[x8,#0xc] */
        0x1c000076,             /* ldr s22,stub+16: 100.0 percent */
        0x1e365a31,             /* fmin s17,s17,s22 */
        branch(stub+12,0x584e88),0x42c80000,
    };
    uint32_t jump=branch(0x584e84,stub);
    for(size_t i=0;i<sizeof code;i++)if(m->base[stub+i])
        runtime_fail("1.4.3 DRS60 veneer padding not empty");
    memcpy(m->base+stub,code,sizeof code);
    memcpy(m->base+sites[0].off,&jump,4);
    fprintf(stderr,"[totk] 1.4.3 TOTK_DRS60=1 applied: native predicted GPU queue-span budget=min(original,100%% of 16.667 ms); main+0x584e84 %08x -> %08x, veneer +%#llx; native palette/hysteresis retained\n",
            orig[0],jump,(unsigned long long)stub);
}

/* Separate from DRS60: a mutable operand, never mutable executable text.
 * Data +48 is the cap in native stress units; +64 is a DIAG-only atomic
 * {scale,priority} snapshot. +56 belongs to the live camera aspect override. */
enum { DRS_FRAMEGEN_CAP_SLOT=48, DRS_FRAMEGEN_SAMPLE_SLOT=64 };
_Static_assert(DRS_FRAMEGEN_CAP_SLOT+4<=ASPECT_DATA_SLOT &&
               ASPECT_DATA_SLOT+4<=DRS_FRAMEGEN_SAMPLE_SLOT,
               "DRS framegen data must not overlap the camera aspect slot");
static _Atomic uint32_t *drs_framegen_cap;
static _Atomic uint64_t *drs_framegen_sample;
void runtime_drs_framegen_budget(double ms){
    if(!drs_framegen_cap)return;
    float stress=isfinite(ms)&&ms>=24.0&&ms<=30.0?(float)(ms*6.0):FLT_MAX;
    uint32_t bits;memcpy(&bits,&stress,4);
    atomic_store_explicit(drs_framegen_cap,bits,memory_order_release);
}
uint64_t runtime_drs_framegen_level(void){
    return drs_framegen_sample?atomic_load_explicit(drs_framegen_sample,memory_order_acquire):0;
}
static void patch_drs_framegen(Module *m){
    const char *e=getenv("TOTK_DRS_FRAMEGEN"),*diag=getenv("TOTK_FRAMEGEN_DIAG");
    int on=e&&!strcmp(e,"1"),sample=diag&&!strcmp(diag,"1");
    if(e&&strcmp(e,"0")&&strcmp(e,"1"))runtime_fail("TOTK_DRS_FRAMEGEN must be 0 or 1");
    const char *fg=getenv("TOTK_FRAMEGEN"),*fps=getenv("TOTK_FPS60");
    if(!fg||strcmp(fg,"locked")||!fps||strcmp(fps,"0")){
        if(on)fprintf(stderr,"[totk] TOTK_DRS_FRAMEGEN=1 ignored: requires locked framegen and TOTK_FPS60=0\n");
        return;
    }
    if(!on&&!sample)return;
    uint64_t data=(m->file_size+0x3fff)&~0x3fffULL;
    uint64_t stub=((m->text_end+15)&~15ULL)+352,level_stub=stub+32;
    const Patch sites[]={
        {0x584e84,0},{0x584e88,0},
        {0x584e8c,0},{0x584e90,0},
        {0x584790,0},{0x584b68,0},
        {0x0a1bb0,0},{0x0a1bb4,0},
        {0x5848a8,0}, /* mov w9,w23: selected scale; x19 still manager */
        {0x5848ac,0}, /* then restore manager/callee-saved registers */
        {0x5848b0,0}, /* packed return flags | scale */
    };
    if(data>m->size||DRS_FRAMEGEN_SAMPLE_SLOT+8>m->size-data||level_stub+32>((m->text_end+0xfff)&~0xfffULL)||
       level_stub>m->size||32>m->size-level_stub)
        runtime_fail("1.4.3 DRS framegen patch layout");
    uint32_t orig[sizeof sites/sizeof *sites];
    verify_render_sites(m,sites,sizeof sites/sizeof *sites,orig,"be02b6cf05e6445af876956d5d10a831818e69c5c97d1b1a884bf304ac4d0c3f");
    uint32_t unit;
    if(m->file_size<DRS_TIME_UNIT[0].off+4||m->size<DRS_TIME_UNIT[0].off+4)
        runtime_fail("1.4.3 DRS framegen time-unit constant outside module");
    if(!code_guard(m,SITES(DRS_TIME_UNIT),&unit,DRS_TIME_UNIT_SHA256))runtime_fail("1.4.3 DRS framegen GPU stress time-unit mismatch");
    for(unsigned i=0;i<64;i++)if(m->base[stub+i])
        runtime_fail("1.4.3 DRS framegen veneer padding not empty");
    for(unsigned i=DRS_FRAMEGEN_CAP_SLOT;i<DRS_FRAMEGEN_SAMPLE_SLOT+8;i++)
        if((i<DRS_FRAMEGEN_CAP_SLOT+8||i>=DRS_FRAMEGEN_SAMPLE_SLOT)&&m->base[data+i])
            runtime_fail("1.4.3 DRS framegen data padding not empty");
    uint32_t code[]={
        orig[0],0xf81f0fe9, /* the original load; save x9, aligned stack */
        adrp(9,(uintptr_t)m->base+stub+8,(uintptr_t)m->base+data),
        0xbd400000u|((DRS_FRAMEGEN_CAP_SLOT/4)<<10)|(9<<5)|22,0xf84107e9, /* ldr s22,[x9,#cap]; restore x9 */
        0x1e365a31,branch(stub+24,0x584e88),
    };
    uint32_t level[]={
        orig[8],0xa9bf2fea, /* the original mov; save x10/x11 */
        adrp(10,(uintptr_t)m->base+level_stub+8,(uintptr_t)m->base+data),
        0xb941366b,0xaa0b812b, /* native priority +134; pack with scale */
        0xf9000000u|((DRS_FRAMEGEN_SAMPLE_SLOT/8)<<10)|(10<<5)|11, /* str x11,[x10,#sample] */
        0xa8c12fea,branch(level_stub+28,0x5848ac),
    };
    if(on){
        uint32_t native=0x7f7fffff,jump=branch(0x584e84,stub);
        memcpy(m->base+data+DRS_FRAMEGEN_CAP_SLOT,&native,4);
        memcpy(m->base+stub,code,sizeof code);memcpy(m->base+0x584e84,&jump,4);
        drs_framegen_cap=(_Atomic uint32_t *)(void *)(m->base+data+DRS_FRAMEGEN_CAP_SLOT);
        fprintf(stderr,"[totk] 1.4.3 TOTK_DRS_FRAMEGEN=1 applied: mutable min(native,published) predicted GPU budget; starts native, backend clamps active target to 24..30 ms; palette/floor/hysteresis unchanged\n");
    }
    if(sample){
        uint32_t jump=branch(0x5848a8,level_stub);
        memcpy(m->base+level_stub,level,sizeof level);memcpy(m->base+0x5848a8,&jump,4);
        drs_framegen_sample=(_Atomic uint64_t *)(void *)(m->base+data+DRS_FRAMEGEN_SAMPLE_SLOT);
        fprintf(stderr,"[totk] 1.4.3 DRS framegen DIAG: selected native priority/scale snapshot at data+%u, disjoint from camera aspect +%u (no host reads of guest manager)\n",DRS_FRAMEGEN_SAMPLE_SLOT,ASPECT_DATA_SLOT);
    }
}
#if TARGET_OS_OSX   /* Mac diagnostics (Mach thread debug state) */
/* High-frame-rate diagnostics (Mac, TOTK_INPUT_FILE commands). Such bugs sit in game code that compares the game's time
   values with constants tuned for 30 Hz (#61: X+0x28 > 100 above). The time values: the FrameRateMgr singleton
   (main+0x3ada848; +0x10 30 Hz ratio, +0x18 seconds, +0x1c integer FPS) and the time object X = [[main+0x3ad27c8]+0xc8]
   (+0x24 seconds, +0x28 FPS, +0x38 ratio), which 0x3e58a4 updates only when the ratio changes.
   - "frmwatch <seconds> [hex offset]" (FrameRateMgr, default +0x10) / "frmwatchx <seconds> <hex offset>" (X): a hardware
     read watchpoint in every thread, re-armed every 10 ms. A hit (loader.c crash() hands SIGTRAP to
     runtime_diag_watch_trap) records the reading instruction and its return address and disarms that thread, so the
     load runs again; the readers are printed at the end. A frequent reader hides the later ones on its thread until the
     next re-arm, and threshold code often runs only at the moment of contact: watch while the case happens.
   - "xpoke <hex offset> <value>": writes a float into X. With the frame step pinned (framestep) the value stays until
     the ratio changes, which tells which field a behaviour depends on. */
static Module *diag_main;
static uint64_t time_object(void){uint64_t x;memcpy(&x,diag_main->base+0x3ad27c8,8);memcpy(&x,(const void *)(uintptr_t)(x+0xc8),8);return x;}
static uint64_t watch_addr;
enum{WATCH_SLOTS=512};
static struct{_Atomic uint64_t pc;_Atomic uint64_t lr;_Atomic uint32_t n;} watch_hits[WATCH_SLOTS];
static void watch_set(thread_act_t t,int on){
    arm_debug_state64_t d;mach_msg_type_number_t c=ARM_DEBUG_STATE64_COUNT;
    if(thread_get_state(t,ARM_DEBUG_STATE64,(thread_state_t)&d,&c)!=KERN_SUCCESS)return;
    /* WCR: enable, EL0, load, byte address select for the 4 bytes at watch_addr */
    d.__wvr[0]=on?(watch_addr&~7ULL):0;d.__wcr[0]=on?((0xfULL<<((watch_addr&7)+5))|(1u<<3)|(2u<<1)|1u):0;
    thread_set_state(t,ARM_DEBUG_STATE64,(thread_state_t)&d,ARM_DEBUG_STATE64_COUNT);
}
static void watch_all(int on){
    thread_act_array_t list;mach_msg_type_number_t n;thread_act_t self=mach_thread_self();
    if(task_threads(mach_task_self(),&list,&n)!=KERN_SUCCESS)return;
    for(unsigned i=0;i<n;i++){if(list[i]!=self){thread_suspend(list[i]);watch_set(list[i],on);thread_resume(list[i]);}mach_port_deallocate(mach_task_self(),list[i]);}
    vm_deallocate(mach_task_self(),(vm_address_t)list,n*sizeof *list);mach_port_deallocate(mach_task_self(),self);
}
int runtime_diag_watch_trap(void *context){
    ucontext_t *u=context;uint64_t pc=u->uc_mcontext->__ss.__pc,lr=u->uc_mcontext->__ss.__lr;
    unsigned ec=(unsigned)(u->uc_mcontext->__es.__esr>>26);if(!watch_addr||(ec!=0x34&&ec!=0x35))return 0;   /* watchpoint */
    for(unsigned i=(unsigned)(pc>>2)%WATCH_SLOTS,k=0;k<WATCH_SLOTS;k++,i=(i+1)%WATCH_SLOTS){
        uint64_t z=0;if(atomic_compare_exchange_strong(&watch_hits[i].pc,&z,pc)||z==pc){atomic_store(&watch_hits[i].lr,lr);atomic_fetch_add(&watch_hits[i].n,1);break;}
    }
    thread_act_t self=mach_thread_self();watch_set(self,0);mach_port_deallocate(mach_task_self(),self);
    return 1;
}
static void *watch_run(void *arg){
    double secs=((double *)arg)[0];unsigned off=(unsigned)((double *)arg)[1];int in_x=((double *)arg)[2]!=0;free(arg);
    uint64_t obj;if(in_x)obj=time_object();else memcpy(&obj,diag_main->base+0x3ada848,8);
    for(unsigned i=0;i<WATCH_SLOTS;i++){atomic_store(&watch_hits[i].pc,0);atomic_store(&watch_hits[i].n,0);}
    watch_addr=obj+off;
    fprintf(stderr,"[totk] frmwatch: %s %#llx, watching +%#x for %.0f s\n",in_x?"X":"FrameRateMgr",(unsigned long long)obj,off,secs);
    for(double t=0;t<secs;t+=0.01){watch_all(1);usleep(10000);}
    watch_all(0);uint64_t base=(uintptr_t)diag_main->base;unsigned total=0;
    for(unsigned i=0;i<WATCH_SLOTS;i++){uint64_t pc=atomic_load(&watch_hits[i].pc);if(!pc)continue;uint32_t n=atomic_load(&watch_hits[i].n);total+=n;
        fprintf(stderr,"[totk] frmwatch hit main%+lld x%u lr main%+lld\n",(long long)(pc-base),n,(long long)(atomic_load(&watch_hits[i].lr)-base));}
    fprintf(stderr,"[totk] frmwatch done: %u hits\n",total);watch_addr=0;return NULL;
}
void runtime_diag_frm_watch(double secs,unsigned off,int in_x){
    if(watch_addr)runtime_fail("frmwatch: a watch is already running");
    double *a=malloc(3*sizeof *a);a[0]=secs;a[1]=off;a[2]=in_x;pthread_t t;pthread_create(&t,NULL,watch_run,a);pthread_detach(t);
}
void runtime_diag_xpoke(unsigned off,float value){
    uint64_t x=time_object();float old;
    memcpy(&old,(const void *)(uintptr_t)(x+off),4);memcpy((void *)(uintptr_t)(x+off),&value,4);
    fprintf(stderr,"[totk] xpoke X+%#x: %g -> %g\n",off,old,value);
}
#endif
static void patch_output_res(Module *m);
static void patch_render_res(Module *m);
void runtime_game_patches(Module *m) {
    runtime_survey_init(m);
    if (strcmp(m->name, "main")) runtime_fail("1.4.3 game patches expect main");
    patch_bench_time(m);
    patch_realtime(m);
    patch_output_res(m);
    patch_render_res(m);
    patch_slowmo_bounce(m);
    patch_shadow_res(m);
    patch_far_clamp(m);
    patch_shadow_cascades(m);
    patch_point_shadows(m);
    patch_fxaa(m);
    patch_drs60(m);
    patch_drs_framegen(m);
#if TARGET_OS_OSX   /* Mac diagnostics (Mach thread debug state) */
    diag_main=m;   /* frmwatch / xpoke */
#endif
    { static const uint64_t sites[] = {0x628c74, 0xb43460, 0x1ff04f4, 0x253fbb0};   /* camera update and setup, viewport x2 */
      aspect_install(m, "1.4.3", sites, 4, ((m->text_end + 15) & ~15ULL) + 48,   /* after the two veneers above */
                     (m->file_size + 0x3fff) & ~0x3fffULL); }
    const char *enabled = getenv("TOTK_FPS60");
    if (enabled && !strcmp(enabled, "0")) {
        fprintf(stderr, "[totk] 1.4.3 shipping frame controller (TOTK_FPS60=0)\n");
        fprintf(stderr, "[totk] TOTK_FPS60_HYSTERESIS=0: inactive in shipping-30 mode\n");
        return;
    }
#if !TARGET_OS_IPHONE
    for (unsigned i = 0; i < sizeof fps60 / sizeof *fps60; i++)
        if (fps60[i].off + 4 > m->text_end) runtime_fail("1.4.3 FPS patch outside text");
    uint32_t orig[sizeof fps60 / sizeof *fps60];
    if (!code_guard(m, SITES(fps60), orig, "667ee7929bfeae700b8d42b8f786abfe3fafa44bbec61ead3930d1479217ef27"))
        runtime_fail("1.4.3 FPS patch original instruction mismatch");
#endif
 #if TARGET_OS_IPHONE
    for (unsigned i = 0; i < sizeof fps60 / sizeof *fps60; i++) {
        uint32_t word; memcpy(&word,m->base+fps60[i].off,4);
        if(word!=fps60[i].replacement)runtime_fail("1.4.3 embedded FPS patch mismatch");
    }
 #else
    for (unsigned i = 0; i < sizeof fps60 / sizeof *fps60; i++)
        memcpy(m->base + fps60[i].off, &fps60[i].replacement, 4);
 #endif
    fprintf(stderr, "[totk] 1.4.3 60 FPS: seven verified controller instructions; original game frame-time calculation\n");
    patch_fps60_hysteresis(m);
    patch_fps60_hardening(m);
    #if defined(__ANDROID__) && defined(TOTK_ANDROID_APP)
    patch_thermal_step(m);
    #endif
}

/* New display configuration object: +0x10 is display 0's canvas size, +0x508
 * its active crop, and +0x528 the floating display scale. The allocation path
 * multiplies the canvas by that scale (8d860..8d894). Publishing only +0x508
 * changes the final crop but leaves every UI/composition target at 1080p.
 * Pending output changes are bit 21 of +8. Keep the original rebuild path. */
static int output_full_at_start,output_native_ui;
static unsigned output_w,output_h;
static _Atomic(uint8_t *) output_page;
static uint64_t pack_size(unsigned w,unsigned h){return (uint64_t)h<<32|w;}
uint64_t runtime_output_size_current(void){
    uint8_t *page=atomic_load(&output_page);
    return page?atomic_load((_Atomic uint64_t *)(void *)(page+32)):pack_size(1920,1080);
}
int runtime_output_res(unsigned *w,unsigned *h){
    static int decided,on;static unsigned cw,ch;
    if(!decided){
        const char *e=getenv("TOTK_OUTPUT_RES");
        const char *ui=getenv("TOTK_NATIVE_UI");
        if(ui&&*ui&&strcmp(ui,"0")&&strcmp(ui,"1"))runtime_fail("TOTK_NATIVE_UI must be 0 or 1");
        output_native_ui=ui&&!strcmp(ui,"1");
        /* iOS shows 720p only (the user, 2026-10-18): no larger swapchain, heaps or output there, whatever
           TOTK_OUTPUT_RES says. The iOS export runs these patches on the Mac to make the phone's embedded code
           (TOTK_EXPORT_IOS), so it follows the same rule: before, auto put the 2560x1440 swapchain into the phone's code,
           and the phone, which carries the heap overlay in its bundle, reserved 88 MiB more for a picture it never shows. */
        if(TARGET_OS_IPHONE||getenv("TOTK_EXPORT_IOS")){
            on=0;
            if(e&&*e&&strcmp(e,"0")&&strcmp(e,"auto"))fprintf(stderr,"[totk] TOTK_OUTPUT_RES=%s ignored: iOS shows 720p only\n",e);
        }else if(output_native_ui){
            /* Preserve the shipping display-config/handheld scale exactly:
             * its 1920 allocation contains a 1280x720 canvas/crop. */
            on=0;
            fprintf(stderr,"[totk] TOTK_NATIVE_UI=1 v2: shipping guest canvas/DRS retained; host-only native UI; TOTK_OUTPUT_RES=%s ignored\n",e?e:"unset");
        }else if(e&&!strcmp(e,"0"))on=0;
        else if(!e||!*e||!strcmp(e,"auto")){
            const char *dir=getenv("TOTK_ROMFS_PATCHES_OUT");char path[4096];
            snprintf(path,sizeof path,"%s/2560x1440/manifest.tsv",dir?dir:"extracted/v143/romfs-patches-out");
            FILE *f=fopen(path,"r");if(f){fclose(f);on=1;cw=2560;ch=1440;}
            output_full_at_start=0;
        }else{
            char tail;
            if(sscanf(e,"%ux%u%c",&cw,&ch,&tail)!=2||cw<640||ch<360||cw>7680||ch>4320)runtime_fail("TOTK_OUTPUT_RES must be auto, 0 or widthxheight");
            on=1;output_full_at_start=1;
        }
        decided=1;
    }
    if(on){*w=cw;*h=ch;}return on;
}
/* The canvas-sized and the scene-sized render targets share DynamicTextureAllocatorMem (113664 KiB at a 1080p canvas
 * and the 1600x900 scene). Each grows the heap by its own pixel ratio above the shipped size, a bigger scene with a 1.6
 * margin: measured 2026-10-19, a 3440x1440 canvas with a 2580x1080 scene crashed in the allocator (main+0x1c2d20) at
 * +258 MiB and loaded at +400 MiB. That was before render_res_publish divided out the game's own canvas scaling (the
 * scene was then about 4620x1935), so for the scene asked for the heaps are now larger than needed: safe, not tight.
 * The HeapDef overlay (scripts/make_output_res_overlay.py --scene) uses the same formula on writable-code hosts;
 * iOS keeps the game's own heaps. 1920x1080 plus a <=1600x900 scene fits the shipped allocation on Android too. */
static int render_res(unsigned *w,unsigned *h);
int runtime_render_res(unsigned *w,unsigned *h){
#if TARGET_OS_IPHONE
    (void)w;(void)h;return 0;
#else
    return !getenv("TOTK_EXPORT_IOS")&&render_res(w,h);
#endif
}
uint64_t runtime_output_res_extra_bytes(void){
    unsigned w,h,sw,sh;double grow=0;
    if(runtime_output_res(&w,&h)&&(double)w*h>1920.0*1080.0)grow+=(double)w*h/(1920.0*1080.0)-1;
    int scene=runtime_render_res(&sw,&sh)&&(double)sw*sh>1600.0*900.0;
    if(scene)grow+=((double)sw*sh/(1600.0*900.0)-1);
    if(grow<=0)return 0;
    if(scene)grow*=1.6;
    uint64_t kib=(uint64_t)ceil(113664.0*(1+grow)/1024.0)*1024-113664;return kib<<10;
}
static void patch_output_res(Module *m){
    unsigned w,h;if(!runtime_output_res(&w,&h))return;
    /* +20..+48: after the frame-step veneer, which is 16 bytes, or 20 with the
     * adaptive capture store, and before the aspect veneers at +48. */
    uint64_t data=(m->file_size+0x3fff)&~0x3fffULL,stub=((m->text_end+15)&~15ULL)+20;
    if(data+0x4000!=m->size||stub+28>((m->text_end+0x3fff)&~0x3fffULL))runtime_fail("1.4.3 output patch layout");
    const Patch guards[]={
        {0xbdb578,0},{0xbdb584,0},
        {0x3dcb28,0},{0x3dcb2c,0},{0x3dcb30,0}};
    uint32_t orig[sizeof guards/sizeof *guards]={0};
#if TARGET_OS_IPHONE
    memcpy(orig+2,m->base+stub+16,4);   /* patched at export: the lsr the embedded veneer replays */
    memcpy(orig+3,m->base+guards[3].off,4);
#else
    if(!code_guard(m,SITES(guards),orig,"a97331f94bd9682248467ba7e71d0a53617c96da02f1625fc0d657f0282bc5d9"))
        runtime_fail("1.4.3 output original instruction mismatch");
    for(unsigned i=0;i<28;i++)if(m->base[stub+i])runtime_fail("1.4.3 output veneer padding not empty");
#endif
    /* x9 is dead at this publication point; the next original use initializes
     * it from the active display selector. x19 remains the display config. */
    uint32_t veneer[]={adrp(9,(uintptr_t)m->base+stub,(uintptr_t)m->base+data),
        0xf9001533, /* str x19,[x9,#40]: live display config */
        0xf9401123, /* ldr x3,[x9,#32]: requested output */
        0xf9000a63, /* str x3,[x19,#0x10]: display 0 canvas */
        orig[2],    /* the original lsr x4,x3,#32: crop height */
        orig[3],    /* the original str x3,[x19,#0x508]: active output write */
        branch(stub+24,0x3dcb30)};
 #if TARGET_OS_IPHONE
    if(memcmp(m->base+stub,veneer,sizeof veneer))runtime_fail("1.4.3 embedded output veneer mismatch");
 #else
    memcpy(m->base+stub,veneer,sizeof veneer);
 #endif
    uint32_t words[]={0xd2800008|(w<<5),0xf2c00008|(h<<5),branch(0x3dcb28,stub)};
 #if TARGET_OS_IPHONE
    for(unsigned i=0;i<3;i++){uint32_t got;memcpy(&got,m->base+guards[i].off,4);if(got!=words[i])runtime_fail("1.4.3 embedded output patch mismatch");}
 #else
    for(unsigned i=0;i<3;i++)memcpy(m->base+guards[i].off,words+i,4);
 #endif
    uint8_t *page=m->base+data;output_w=w;output_h=h;
    atomic_store((_Atomic uint64_t *)(void *)(page+32),output_full_at_start?pack_size(w,h):pack_size(1920,1080));
    atomic_store((_Atomic uint64_t *)(void *)(page+40),0);
    atomic_store(&output_page,page);   /* Publish initialized state to the HUD on the main thread. */
    fprintf(stderr,"[totk] 1.4.3 output: %ux%u swapchain; starts %s, key 3 cycles MetalFX / native / off; display-config veneer +%#llx\n",w,h,output_full_at_start?"at requested resolution":"at 1920x1080",(unsigned long long)stub);
}
/* Render resolution (TOTK_RENDER_RES=<w>x<h>, e.g. 960x540) - the 1.4.3 port of game_patches.c's patch.
   1.4.3 keeps the same graphics-parameter entry (+0x30 of the mode entry: handheld 1280x720, docked 1600x900) and the
   same renderer fields (+0xcc4 width, +0xcc8 height), but converts both through s0 and keeps the output size in w9:
     main+0x585e48  scvtf s0, w11             -> adrp x3, <data page>
     main+0x585e50  str   s0, [x19, #0xcc4]   -> ldp  s0, s1, [x3, #16]
     main+0x585e54  scvtf s0, w10             -> str  s0, [x19, #0xcc4]
     main+0x585e60  str   s0, [x19, #0xcc8]   -> str  s1, [x19, #0xcc8]
   Every path that picks the render size converges through main+0x585e48 (the branch at 0x585f50 only reloads w11/w10
   and jumps back), so the four sites cover all of them. x3 is scratch and untouched by this function; x9 must NOT be
   used here because w9 still holds the output size for main+0x585e64. The size comes from the loader's data page +16
   (the page patch_realtime reserves); TOTK_RENDER_RES unset = the game's own size and nothing is patched.
   iOS cannot write code at run time: the iOS export (TOTK_EXPORT_IOS, runtime/loader.c) runs this patch on the Mac, so
   the phone's embedded code carries it only when TOTK_RENDER_RES was set for the export. The phone checks the embedded
   words and writes the size on the data page itself: its own TOTK_RENDER_RES, or the game's handheld 1280x720 when unset
   (the phone always reports handheld mode). Embedded code without the patch keeps the game's own size. */
/* The game scales its scene with the canvas: the size it renders is the mode entry's (or the patched) size times the
   output width over 1920, on both axes (measured: 2560x1440 asked on a 2560x1440 canvas rendered 3408x1920, 3440x1440 on
   the 3440x1440 canvas 6160x2576). The data page therefore holds the asked size divided by that factor, rewritten when
   key 3 changes the canvas, so the scene is the size asked for on every canvas. */
static uint8_t *render_page;static unsigned render_w,render_h;
static void render_res_publish(unsigned canvas_w){
    if(!render_page)return;
    float k=canvas_w>1920?canvas_w/1920.0f:1.0f,v[2]={render_w/k,render_h/k};
    memcpy(render_page+16,v,sizeof v);
    fprintf(stderr,"[totk] 1.4.3 render resolution: %ux%u on a %u-wide canvas (game size %.0fx%.0f, scaled by %.4f)\n",render_w,render_h,canvas_w,v[0],v[1],k);
}
static int render_res(unsigned *w,unsigned *h){
    const char *e=getenv("TOTK_RENDER_RES");
    if(!e||!*e||!strcmp(e,"0"))return 0;
    char rest;
    if(sscanf(e,"%ux%u%c",w,h,&rest)!=2||*w<64||*h<64||*w>4096||*h>4096)runtime_fail("TOTK_RENDER_RES must be <width>x<height>");
    return 1;
}
static void patch_render_res(Module *m){
    unsigned w=1280,h=720;int set=render_res(&w,&h);
#if !TARGET_OS_IPHONE
    if(!set)return;   /* Writable-code hosts (including Android): game's own scene unless asked. */
#endif
    uint64_t data=(m->file_size+0x3fff)&~0x3fffULL;
    if(data+24>m->size)runtime_fail("1.4.3 render-resolution patch layout");
    const Patch sites[]={
        {0x585e48,0},
        {0x585e50,0x2d420460},
        {0x585e54,0},          /* the original store from +0x585e50, one instruction later */
        {0x585e60,0xbd0cca61},
    };
    static const char sites_sha256[]="1cf3b3a9738a2fefcfdb27badff1ac43b43722394f7e82dd9a2bd6af19eddaa2";
    uint32_t orig[sizeof sites/sizeof *sites];
    int original=code_guard(m,SITES(sites),orig,sites_sha256);
#if TARGET_OS_IPHONE
    if(original){fprintf(stderr,"[totk] 1.4.3 render resolution: the game's own (not patched at build)\n");return;}
    orig[1]=orig[2];   /* patched at export: the moved store is already in place */
#endif
    uint32_t want[4]={adrp(3,(uintptr_t)m->base+sites[0].off,(uintptr_t)m->base+data),
                      sites[1].replacement,orig[1],sites[3].replacement};
#if TARGET_OS_IPHONE
    for(unsigned i=0;i<sizeof sites/sizeof *sites;i++){
        uint32_t got;memcpy(&got,m->base+sites[i].off,4);
        if(got!=want[i])runtime_fail("1.4.3 embedded render-resolution patch mismatch");
    }
#else
    if(!original){
        fprintf(stderr,"[totk] render resolution patch main+%#llx: the instructions are not the expected ones\n",
                (unsigned long long)sites[0].off);
        runtime_fail("render resolution patch: main does not match 1.4.3");
    }
    for(unsigned i=0;i<sizeof sites/sizeof *sites;i++)memcpy(m->base+sites[i].off,&want[i],4);
#endif
    render_page=m->base+data;render_w=w;render_h=h;
    render_res_publish(output_w?(unsigned)runtime_output_size_current():1920);
    fprintf(stderr,"[totk] 1.4.3 render resolution patch: scene render size %ux%u%s (4 instructions at main+0x585e48, value on the data page)\n",
            w,h,set?"":" (the game's handheld size; TOTK_RENDER_RES not set)");
}
int runtime_output_res_toggle(void){
    if(!output_page||output_native_ui)return 0;
    _Atomic uint64_t *size=(_Atomic uint64_t *)(void *)(output_page+32);
    int full=atomic_load(size)!=pack_size(output_w,output_h);
    atomic_store(size,full?pack_size(output_w,output_h):pack_size(1920,1080));
    uint64_t now=atomic_load(size);render_res_publish((unsigned)now);   /* the scene size first: the game rebuilds when told */
    uint64_t config=atomic_load((_Atomic uint64_t *)(void *)(output_page+40));
    if(config){__atomic_fetch_or((uint32_t *)(uintptr_t)(config+8),1u<<21,__ATOMIC_SEQ_CST);runtime_oe_push_message(30);}
    fprintf(stderr,"[totk] 1.4.3 output requested %ux%u (display config %s)\n",(unsigned)now,(unsigned)(now>>32),config?"ready":"not published yet");
    return 1;
}
int runtime_output_mode_cycle(void){
    if(!output_page||output_native_ui)return 0;
    extern int mfxup_output_enabled(void);extern void dm_gfx_set_flag(const char *,int);
    int full=atomic_load((_Atomic uint64_t *)(void *)(output_page+32))==pack_size(output_w,output_h);
    int native=0,mfx=0;const char *name;
    if(full)name="关闭 2K";
    else if(mfxup_output_enabled()){native=1;name="原生 2K";}
    else{mfx=1;name="MetalFX 2K";}
    if(full!=native)runtime_output_res_toggle();
    dm_gfx_set_flag("mfx_output",mfx);dm_gfx_set_flag("mfx_upscale",mfx);
    fprintf(stderr,"[totk] key 3: %s\n",name);return 1;
}
