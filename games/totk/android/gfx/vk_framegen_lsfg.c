/* MIT. User DLL and translated shaders stay private/in memory. */
#include "vk_framegen_lsfg.h"
#include "framegen_qcom_policy.h"
#include <math.h>
#include "vk_overlay.h"
static _Atomic int requested=-1,refused;
static _Atomic int probe_requested=-1;
int vk_fg_lsfg_probe_requested(void) {
    int on=atomic_load_explicit(&probe_requested,memory_order_relaxed);
    if (on>=0) return on;
    const char *value=getenv("TOTK_LSFG_PROBE");
    on=value && !strcmp(value,"1");
    int expected=-1;
    if (!atomic_compare_exchange_strong(&probe_requested,&expected,on)) return expected;
    return on;
}
static _Atomic uint64_t refusals;
static _Atomic(const char *) refusal_reason;
int vk_fg_lsfg_requested(void) {
    int on=atomic_load_explicit(&requested,memory_order_relaxed);
    if (on>=0) return on;
    const char *engine=getenv("TOTK_FRAMEGEN_ENGINE"),*mode=getenv("TOTK_FRAMEGEN");
    on=engine && !strcmp(engine,"lsfg") && mode && !strcmp(mode,"locked");
    int expected=-1;
    if (!atomic_compare_exchange_strong(&requested,&expected,on)) return expected;
    if (engine && !strcmp(engine,"lsfg") && !on)
        LOGV("framegen lsfg refused: %s",fg_qcom_adaptive(mode) ?
            "adaptive activation disabled pending validated doubling repair (real-only; no camera fallback)" :
            "requires TOTK_FRAMEGEN=locked (using existing generator)");
    return on;
}
void vk_fg_lsfg_refuse(const char *reason) {
    atomic_fetch_add_explicit(&refusals,1,memory_order_relaxed);
    if (!atomic_exchange_explicit(&refused,1,memory_order_acq_rel)) {
        atomic_store_explicit(&refusal_reason,reason,memory_order_release);
        LOGV("framegen lsfg refused: %s (permanent fallback to locked camera interpolation)",reason);
    }
}
#if defined(__ANDROID__) && defined(TOTK_ANDROID_APP)
#include "vk_framegen.h"
#include "../third_party/lsfg/api.h"
#include <dlfcn.h>
#include <unistd.h>
extern const char *runtime_android_private_files(void);
enum { LSFG_SOURCES=4 };
#define LSFG_TIMEOUT_NS UINT64_C(100000000)
typedef struct {
    vk_fg_lsfg_provenance source;
    totk_lsfg_image captured[2],bound[2];
    VkCommandBuffer record_cb,seal_cb;
    uint64_t frame,predicted,actual,observed_seal,reader_ticket[2];
    uint64_t record_serial,seal_serial;
    uint32_t w,h;
    int32_t context;
    unsigned previous,current;
    int recorded,awaiting_submit;
    char error[256];
} lsfg_probe;
typedef struct {
    totk_lsfg_image scene,output;
    VkDeviceMemory memory;
    VkQueryPool query;
    uint32_t w,h;
    int32_t context;
    int context_valid,pending,bad;
    VkImage previous,current;
    uint64_t started,ticket;
    totk_lsfg_image bound[2]; /* PROBE1 observations; image-only rebind policy is unchanged. */
    lsfg_probe probe;
} lsfg_source;
static struct {
    pthread_mutex_t mu;
    _Atomic int live,suspended,cooling;
    _Atomic unsigned pending,quarantined;
    PFN_vkGetInstanceProcAddr ip;
    __typeof__(&totk_lsfg_initialize) initialize;
    __typeof__(&totk_lsfg_context) context;
    __typeof__(&totk_lsfg_rebind) rebind;
    __typeof__(&totk_lsfg_retire) retire;
    __typeof__(&totk_lsfg_record) record;
    __typeof__(&totk_lsfg_probe_prepare) probe_prepare;
    __typeof__(&totk_lsfg_record_probe) record_probe;
    __typeof__(&totk_lsfg_probe_write) probe_write;
    void *library;
    int device,diag,performance;
    int probe_done;
    VkCommandBuffer seal_cb;
    uint64_t seal_serial;
    float flow;
    char dll[1200],error[256];
    lsfg_source source[LSFG_SOURCES];
    fg_qcom_policy policy;
    uint64_t submitted,generated,post_refused,timed,missing,timeout,contexts,rebinds;
    double sum_ms,max_ms,pass_sum[TOTK_LSFG_PASSES],pass_max[TOTK_LSFG_PASSES];
} L={.mu=PTHREAD_MUTEX_INITIALIZER};
void vk_fg_lsfg_device(PFN_vkGetInstanceProcAddr ip,PFN_vkGetDeviceProcAddr dp,int interop) {
    (void)dp; (void)interop;
    if (!vk_fg_lsfg_requested()) return;
    const char *diag=getenv("TOTK_FRAMEGEN_DIAG"); L.diag=diag && !strcmp(diag,"1");
    const char *files=runtime_android_private_files();
    if (!files || snprintf(L.dll,sizeof L.dll,"%s/lsfg/Lossless.dll",files)>=(int)sizeof L.dll || access(L.dll,R_OK)) {
        vk_fg_lsfg_refuse("user DLL missing: <private files>/lsfg/Lossless.dll"); return;
    }
    const char *perf=getenv("TOTK_LSFG_PERF"),*flow=getenv("TOTK_LSFG_FLOW");
    L.performance=perf && !strcmp(perf,"1"); L.flow=0.5f;
    if (perf && strcmp(perf,"0") && strcmp(perf,"1")) {
        vk_fg_lsfg_refuse("TOTK_LSFG_PERF must be 0 or 1"); return;
    }
    if (flow) {
        if (!strcmp(flow,"1") || !strcmp(flow,"1.0")) L.flow=1.0f;
        else if (!strcmp(flow,"0.75")) L.flow=0.75f;
        else if (!strcmp(flow,"0.5")) L.flow=0.5f;
        else { vk_fg_lsfg_refuse("TOTK_LSFG_FLOW must be 1.0, 0.75 or 0.5"); return; }
    }
    if (!V.timestamp_bits || !isfinite(V.props.limits.timestampPeriod) || V.props.limits.timestampPeriod<=0) {
        vk_fg_lsfg_refuse("graphics/compute queue GPU timestamps unavailable"); return;
    }
    L.ip=ip; L.device=1;
}
static int init(void) {
    if (atomic_load(&L.live)) return 1;
    if (!L.device) return 0;
    L.library=dlopen("libtotk_lsfg.so",RTLD_NOW|RTLD_LOCAL);
    if (!L.library) { vk_fg_lsfg_refuse("packaged MIT LSFG backend unavailable"); return 0; }
#define LOAD(field) L.field=(__typeof__(L.field))dlsym(L.library,"totk_lsfg_" #field); if (!L.field) { vk_fg_lsfg_refuse("LSFG backend ABI unavailable"); return 0; }
    LOAD(initialize) LOAD(context) LOAD(rebind) LOAD(retire) LOAD(record)
    if (vk_fg_lsfg_probe_requested()) { LOAD(probe_prepare) LOAD(record_probe) LOAD(probe_write) }
#undef LOAD
    if (!L.initialize(L.ip,V.inst,V.pd,V.dev,V.queue,V.qfam,V.has_null_descriptor,
            L.dll,L.performance,L.flow,L.error)) { vk_fg_lsfg_refuse(L.error); return 0; }
    atomic_store_explicit(&L.live,1,memory_order_release);
    const char *wide=getenv("TOTK_LSFG_WIDE_BARRIERS"),*full=getenv("TOTK_LSFG_FULL_BARRIERS");
    LOGV("framegen lsfg: LSFG 3.1 x2 midpoint; model=%s flow=%.2f wide_barriers=%d full_barriers=%d, game VkDevice/queue, direct pre-UI sampled scenes and HDR storage target; no AHB transport/copy/conversion or cross-device semaphore; older affine UI; per-pass GPU timestamps; no derived disk cache",
        L.performance ? "performance" : "normal",L.flow,
        wide && !strcmp(wide,"1"),full && !strcmp(full,"1"));
    return 1;
}
int vk_fg_lsfg_active(void) {
    if (!vk_fg_lsfg_requested() || !vk_fg_locked() || !vk_fg_enabled() || atomic_load(&refused)) return 0;
    if (!atomic_load_explicit(&L.live,memory_order_acquire)) {
        pthread_mutex_lock(&L.mu); int on=init(); pthread_mutex_unlock(&L.mu); if (!on) return 0;
    }
    return !atomic_load(&refused) && !atomic_load(&L.suspended) && !atomic_load(&L.cooling) && !atomic_load(&L.quarantined);
}
/* L.mu serializes policy mutation with capture/harvest. */
/* L.mu is already held; never query or synchronize just for the panel. */
static void overlay_policy(void) {
    uint64_t left=L.policy.cooldown_until>L.policy.tick ? L.policy.cooldown_until-L.policy.tick : 0;
    int fallback=left || atomic_load_explicit(&L.suspended,memory_order_relaxed) ||
        atomic_load_explicit(&L.quarantined,memory_order_relaxed);
    vk_overlay_set_framegen(VK_OVERLAY_FG_LSFG,VK_OVERLAY_FG_INTERP,
        atomic_load_explicit(&refused,memory_order_relaxed) ? VK_OVERLAY_FG_REFUSED :
        left ? VK_OVERLAY_FG_COOLDOWN : fallback ? VK_OVERLAY_FG_WARMUP : VK_OVERLAY_FG_ACTIVE,
        left>UINT32_MAX ? UINT32_MAX : (unsigned)left,fallback);
}
static void failure(unsigned kind) {
    atomic_store(&L.suspended,1);
    int permanent=fg_qcom_failure(&L.policy,kind);
    atomic_store_explicit(&L.cooling,!fg_qcom_can_generate(&L.policy),memory_order_release);
    overlay_policy();
    if (permanent) vk_fg_lsfg_refuse("sustained post-warmup deadline/timing failures (5 in 120 real ticks)");
}
void vk_fg_lsfg_late(void) {
    if (!atomic_load(&L.live) || atomic_load(&refused)) return;
    pthread_mutex_lock(&L.mu); failure(FG_QCOM_DEADLINE); pthread_mutex_unlock(&L.mu);
}
void vk_fg_lsfg_tick(int scene) {
    if (!atomic_load(&L.live)) {
        vk_overlay_set_framegen(VK_OVERLAY_FG_LSFG,VK_OVERLAY_FG_INTERP,
            atomic_load_explicit(&refused,memory_order_relaxed) ? VK_OVERLAY_FG_REFUSED : VK_OVERLAY_FG_WARMUP,0,1);
        return;
    }
    /* Frame end follows presenter/overlay initialization; early model admission
       can precede the first present, when all overlay setters are still inert. */
    static int model_published;
    if (!model_published) { vk_overlay_set_lsfg_model(3,1,0); model_published=1; }
    pthread_mutex_lock(&L.mu);
    fg_qcom_tick(&L.policy,scene);
    atomic_store_explicit(&L.cooling,!fg_qcom_can_generate(&L.policy),memory_order_release);
    overlay_policy();
    pthread_mutex_unlock(&L.mu);
}
int vk_fg_lsfg_can_generate(void) { return vk_fg_lsfg_active(); }
unsigned vk_fg_lsfg_pending(void) { return atomic_load_explicit(&L.pending,memory_order_acquire); }
void vk_fg_lsfg_rearm(void) {
    if (!atomic_load(&refused) && !atomic_load(&L.cooling) && !atomic_load(&L.quarantined) && !vk_fg_lsfg_pending())
        atomic_store(&L.suspended,0);
}
static void output_free(lsfg_source *s) {
    if (s->context_valid) L.retire(s->context);
    s->context_valid=0;
    if (s->output.view) vkDestroyImageView(V.dev,s->output.view,NULL);
    if (s->output.image) vkDestroyImage(V.dev,s->output.image,NULL);
    vk_memory_release(s->memory,VK_MEMORY_SNAPSHOT);
    s->output=(totk_lsfg_image){0}; s->memory=VK_NULL_HANDLE;
}
static int prepare(lsfg_source *s,uint32_t w,uint32_t h) {
    if (s->output.image && s->w==w && s->h==h) return 1;
    output_free(s);
    s->w=w; s->h=h; s->output.format=VK_FORMAT_R16G16B16A16_SFLOAT;
    VkImageCreateInfo ci={VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,
        .format=s->output.format,.extent={w,h,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,
        .tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT};
    if (vk_fg_dump_requested()) ci.usage|=VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateImage(V.dev,&ci,NULL,&s->output.image)!=VK_SUCCESS) goto fail;
    VkMemoryRequirements req; vkGetImageMemoryRequirements(V.dev,s->output.image,&req);
    int type=vk_pick_type(req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type<0) goto fail;
    VkMemoryAllocateInfo ai={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=(uint32_t)type};
    if (vk_memory_allocate(&ai,&s->memory,VK_MEMORY_SNAPSHOT)!=VK_SUCCESS ||
        vkBindImageMemory(V.dev,s->output.image,s->memory,0)!=VK_SUCCESS) goto fail;
    VkImageViewCreateInfo vi={VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=s->output.image,
        .viewType=VK_IMAGE_VIEW_TYPE_2D,.format=s->output.format,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    if (vkCreateImageView(V.dev,&vi,NULL,&s->output.view)!=VK_SUCCESS) goto fail;
    if (!s->query) {
        VkQueryPoolCreateInfo qi={VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,.queryType=VK_QUERY_TYPE_TIMESTAMP,
            .queryCount=TOTK_LSFG_TIMESTAMPS};
        if (vkCreateQueryPool(V.dev,&qi,NULL,&s->query)!=VK_SUCCESS) goto fail;
    }
    return 1;
fail:
    output_free(s); vk_fg_lsfg_refuse("same-device LSFG HDR storage/timestamp allocation unavailable"); return 0;
}
int vk_fg_lsfg_capture(VkCommandBuffer cb,unsigned index,VkImage scene,VkImageView view,VkFormat format,uint32_t w,uint32_t h) {
    (void)cb;
    if (index>=LSFG_SOURCES || !vk_fg_lsfg_active()) return 0;
    vk_fg_lsfg_harvest();
    pthread_mutex_lock(&L.mu);
    lsfg_source *s=&L.source[index];
    int ok=0;
    if (s->pending) { vk_fg_lsfg_refuse("source reused before LSFG timestamp retirement"); goto done; }
    if (w*L.flow<64 || h*L.flow<64) { vk_fg_lsfg_refuse("scene too small for seven flow levels"); goto done; }
    if (!prepare(s,w,h)) goto done;
    s->scene=(totk_lsfg_image){scene,view,format}; ok=1;
done:
    pthread_mutex_unlock(&L.mu); return ok;
}
int vk_fg_lsfg_interpolate(unsigned previous,unsigned index,uint64_t frame,float phase,
        const vk_fg_lsfg_provenance *provenance,VkImage *image,VkImageView *view) {
    if (previous>=LSFG_SOURCES || index>=LSFG_SOURCES || !image || !view ||
        !fg_qcom_phase_valid(phase) || !vk_fg_lsfg_active()) return 0;
    pthread_mutex_lock(&L.mu);
    lsfg_source *s=&L.source[index],*old=&L.source[previous]; int ok=0;
    if (!s->scene.image || !old->scene.image || s->w!=old->w || s->h!=old->h) goto done;
    if (!s->context_valid) {
        if (!L.context(old->scene,s->scene,s->output,s->w,s->h,&s->context,L.error)) {
            vk_fg_lsfg_refuse(L.error); goto done;
        }
        s->context_valid=1; s->previous=old->scene.image; s->current=s->scene.image; L.contexts++;
        if (vk_fg_lsfg_probe_requested()) { s->bound[0]=old->scene; s->bound[1]=s->scene; }
    } else if (s->previous!=old->scene.image || s->current!=s->scene.image) {
        if (!L.rebind(s->context,old->scene,s->scene,L.error)) { vk_fg_lsfg_refuse(L.error); goto done; }
        s->previous=old->scene.image; s->current=s->scene.image; L.rebinds++;
        if (vk_fg_lsfg_probe_requested()) { s->bound[0]=old->scene; s->bound[1]=s->scene; }
    }
    VkCommandBuffer cb=vk_cb();
    int probe=phase==.5f && provenance && vk_fg_lsfg_probe_requested() && !L.probe_done;
    int prepared=0;
    if (probe) {
        s->probe=(lsfg_probe){.source=*provenance,.record_cb=cb,.frame=frame,
            .record_serial=vk_submit_serial(),.reader_ticket={old->ticket,s->ticket},
            .w=s->w,.h=s->h,.context=s->context,.previous=previous,.current=index};
        s->probe.captured[0]=old->scene; s->probe.captured[1]=s->scene;
        s->probe.bound[0]=s->bound[0]; s->probe.bound[1]=s->bound[1];
        prepared=L.probe_prepare(s->context,s->probe.error);
        if (!prepared && !s->probe.error[0])
            snprintf(s->probe.error,sizeof s->probe.error,"probe preparation failed without backend reason");
    }
    vkCmdResetQueryPool(cb,s->query,0,TOTK_LSFG_TIMESTAMPS);
    /* The isolated library owns its Vulkan dispatch table, so hand it the
     * actual ordered secondary rather than the stable walking token. Keep cb
     * itself for the existing seal-membership/provenance bookkeeping. */
    VkCommandBuffer commands = VK_SECTION_CB(cb);
    int recorded=prepared ? L.record_probe(s->context,commands,s->query,L.error) :
        L.record(s->context,commands,phase,s->query,L.error);
    if (!recorded) {
        if (probe) snprintf(s->probe.error,sizeof s->probe.error,"%s",L.error);
        vk_fg_lsfg_refuse(L.error); goto done;
    }
    if (probe) { s->probe.recorded=prepared; s->probe.awaiting_submit=1; }
    s->pending=1; s->bad=0; s->started=host_now_ns(); L.submitted++;
    s->ticket=mtl_last_fence()+1;
    if (probe) s->probe.predicted=s->ticket;
    atomic_fetch_add_explicit(&L.pending,1,memory_order_release);
    *image=s->output.image; *view=s->output.view; ok=1;
done:
    pthread_mutex_unlock(&L.mu); return ok;
}
void vk_fg_lsfg_submitting(VkCommandBuffer commands) {
    if (!vk_fg_lsfg_probe_requested() || !atomic_load(&L.live)) return;
    pthread_mutex_lock(&L.mu);
    L.seal_cb=commands;
    L.seal_serial=vk_submit_serial();
    pthread_mutex_unlock(&L.mu);
}
void vk_fg_lsfg_submitted(uint64_t actual_fence) {
    if (!vk_fg_lsfg_probe_requested() || !atomic_load(&L.live)) return;
    pthread_mutex_lock(&L.mu);
    for (unsigned i=0;i<LSFG_SOURCES;i++) {
        lsfg_probe *p=&L.source[i].probe;
        if (!p->awaiting_submit) continue;
        p->seal_cb=L.seal_cb; p->seal_serial=L.seal_serial; p->observed_seal=actual_fence;
        /* Generic membership observation normally assigned this already. A
           fallback needs the same incarnation and exactly one intervening seal. */
        if (vk_submit_serial()==L.seal_serial+1)
            (void)vk_fg_lsfg_probe_assign(&p->actual,p->record_cb,p->record_serial,
                &L.seal_cb,1,L.seal_serial,actual_fence);
        p->awaiting_submit=0;
    }
    L.seal_cb=VK_NULL_HANDLE;
    pthread_mutex_unlock(&L.mu);
}
void vk_fg_lsfg_sealed(const VkCommandBuffer *commands,unsigned count,uint64_t actual_fence) {
    if (!vk_fg_lsfg_probe_requested() || !atomic_load(&L.live)) return;
    uint64_t serial=vk_submit_serial();
    /* Called only after the submitter released g_mu, before queue publication.
       No allocation, logging, GPU command or ticket/lifetime mutation occurs. */
    pthread_mutex_lock(&L.mu);
    for (unsigned i=0;i<LSFG_SOURCES;i++) {
        lsfg_probe *p=&L.source[i].probe;
        if (!p->awaiting_submit ||
            !vk_fg_lsfg_probe_assign(&p->actual,p->record_cb,p->record_serial,
                commands,count,serial,actual_fence)) continue;
        p->seal_cb=p->record_cb; p->seal_serial=serial; p->observed_seal=actual_fence;
        p->awaiting_submit=0;
    }
    pthread_mutex_unlock(&L.mu);
}
void vk_fg_lsfg_release(VkCommandBuffer cb,unsigned index) { (void)cb; (void)index; }
void vk_fg_lsfg_harvest(void) {
    if (!atomic_load(&L.live) || !atomic_load(&L.pending)) return;
    pthread_mutex_lock(&L.mu);
    uint64_t mask=V.timestamp_bits>=64 ? UINT64_MAX : (UINT64_C(1)<<V.timestamp_bits)-1;
    for (unsigned i=0;i<LSFG_SOURCES;i++) {
        lsfg_source *s=&L.source[i]; if (!s->pending) continue;
        if (!mtl_wait(s->ticket,0)) continue;
        uint64_t time[TOTK_LSFG_TIMESTAMPS];
        VkResult result=vkGetQueryPoolResults(V.dev,s->query,0,TOTK_LSFG_TIMESTAMPS,sizeof time,time,
            sizeof(uint64_t),VK_QUERY_RESULT_64_BIT);
        if (result!=VK_SUCCESS) {
            if (!s->bad && host_now_ns()-s->started>LSFG_TIMEOUT_NS) {
                s->bad=1; L.missing++; L.timeout++; failure(FG_QCOM_TIMING);
                atomic_fetch_add(&L.quarantined,1);
            }
            continue; /* A missing timestamp never authorizes resource reuse. */
        }
        double ms=(double)((time[TOTK_LSFG_TIMESTAMPS-1]-time[0])&mask)*V.props.limits.timestampPeriod/1e6;
        if (!s->bad && isfinite(ms)) {
            L.timed++; L.sum_ms+=ms; if (ms>L.max_ms) L.max_ms=ms;
            for (unsigned pass=0;pass<TOTK_LSFG_PASSES;pass++) {
                double t=(double)((time[pass+1]-time[pass])&mask)*V.props.limits.timestampPeriod/1e6;
                L.pass_sum[pass]+=t; if (t>L.pass_max[pass]) L.pass_max[pass]=t;
            }
        }
        if (s->bad) atomic_fetch_sub(&L.quarantined,1);
        s->pending=0; atomic_fetch_sub_explicit(&L.pending,1,memory_order_release);
    }
    pthread_mutex_unlock(&L.mu);
}
void vk_fg_lsfg_result(int accepted) {
    pthread_mutex_lock(&L.mu);
    if (accepted) L.generated++; else L.post_refused++;
    pthread_mutex_unlock(&L.mu);
}
int vk_fg_lsfg_dump_image(unsigned index,VkImage *image,VkFormat *format,uint32_t *width,uint32_t *height) {
    if (index>=LSFG_SOURCES || !vk_fg_dump_requested()) return 0;
    pthread_mutex_lock(&L.mu);
    const lsfg_source *s=&L.source[index];
    int valid=s->context_valid && s->output.image;
    if (valid) { *image=s->output.image; *format=s->output.format; *width=s->w; *height=s->h; }
    pthread_mutex_unlock(&L.mu); return valid;
}
static int probe_export(unsigned index,uint64_t frame,const char *directory,FILE *receipt) {
    pthread_mutex_lock(&L.mu);
    L.probe_done=1;
    if (index>=LSFG_SOURCES) {
        fprintf(receipt,"status=error error=source index out of range\n");
        pthread_mutex_unlock(&L.mu); return 0;
    }
    const lsfg_source *s=&L.source[index];
    const lsfg_probe *p=&s->probe;
    int match=p->frame && p->frame==frame;
    int cb_match=p->record_cb && p->record_cb==p->seal_cb;
    int serial_match=p->record_serial && p->record_serial==p->seal_serial;
    int ticket_mismatch=p->actual ? p->predicted!=p->actual : -1;
    fprintf(receipt,"probe-generation=%llu frame-match=%d context=%d source-slots=%u,%u "
        "history-at-record=%d pair-source-at-record=%d extent=%ux%u recorded=%d\n"
        "record-cb=0x%llx seal-cb=0x%llx cb-match=%d predicted-ticket=%llu "
        "actual-ticket=%llu observed-seal=%llu predicted-actual-mismatch=%d awaiting-seal=%d\n"
        "record-incarnation=%llu seal-incarnation=%llu incarnation-match=%d\n"
        "actual-ticket=0 or predicted-actual-mismatch=-1 means UNKNOWN; exact attribution requires "
        "CB membership and recording incarnation, not a handle alone. Retirement policy is unchanged.\n",
        (unsigned long long)p->frame,match,p->context,p->previous,p->current,
        p->source.history,p->source.pair_source,p->w,p->h,p->recorded,
        (unsigned long long)(uintptr_t)p->record_cb,(unsigned long long)(uintptr_t)p->seal_cb,cb_match,
        (unsigned long long)p->predicted,(unsigned long long)p->actual,
        (unsigned long long)p->observed_seal,ticket_mismatch,p->awaiting_submit,
        (unsigned long long)p->record_serial,(unsigned long long)p->seal_serial,serial_match);
    for (unsigned side=0;side<2;side++) {
        fprintf(receipt,"%s frame=%llu epoch=%llu capture-cb=0x%llx capture-ticket-predicted=%llu "
            "reader-ticket-at-record=%llu retire-at-record=%llu references-at-record=%u "
            "captured-image=0x%llx captured-view=0x%llx captured-format=%u "
            "bound-image=0x%llx bound-view=0x%llx bound-format=%u view-mismatch=%d\n",
            side ? "current" : "previous",(unsigned long long)p->source.frame[side],
            (unsigned long long)p->source.epoch[side],
            (unsigned long long)(uintptr_t)p->source.capture_cb[side],
            (unsigned long long)p->source.capture_ticket[side],
            (unsigned long long)p->reader_ticket[side],(unsigned long long)p->source.retire[side],
            p->source.references[side],(unsigned long long)(uintptr_t)p->captured[side].image,
            (unsigned long long)(uintptr_t)p->captured[side].view,p->captured[side].format,
            (unsigned long long)(uintptr_t)p->bound[side].image,
            (unsigned long long)(uintptr_t)p->bound[side].view,p->bound[side].format,
            p->captured[side].view!=p->bound[side].view);
    }
    LOGV("[lsfg-provenance] dump-frame=%llu probe-frame=%llu frame-match=%d slots=%u,%u "
        "frames=%llu,%llu epochs=%llu,%llu capture-predicted=%llu,%llu readers=%llu,%llu "
        "retire=%llu,%llu refs=%u,%u history=%d pair-source=%d context=%d extent=%ux%u "
        "bound0=0x%llx/0x%llx/%u bound1=0x%llx/0x%llx/%u view-mismatch=%d,%d "
        "record-cb=0x%llx seal-cb=0x%llx cb-match=%d predicted=%llu actual=%llu "
        "observed-seal=%llu incarnation=%llu/%llu incarnation-match=%d "
        "predicted-actual-mismatch=%d (actual0/mismatch-1=UNKNOWN; no lifetime change)",
        (unsigned long long)frame,(unsigned long long)p->frame,match,p->previous,p->current,
        (unsigned long long)p->source.frame[0],(unsigned long long)p->source.frame[1],
        (unsigned long long)p->source.epoch[0],(unsigned long long)p->source.epoch[1],
        (unsigned long long)p->source.capture_ticket[0],(unsigned long long)p->source.capture_ticket[1],
        (unsigned long long)p->reader_ticket[0],(unsigned long long)p->reader_ticket[1],
        (unsigned long long)p->source.retire[0],(unsigned long long)p->source.retire[1],
        p->source.references[0],p->source.references[1],p->source.history,p->source.pair_source,
        p->context,p->w,p->h,(unsigned long long)(uintptr_t)p->bound[0].image,
        (unsigned long long)(uintptr_t)p->bound[0].view,p->bound[0].format,
        (unsigned long long)(uintptr_t)p->bound[1].image,
        (unsigned long long)(uintptr_t)p->bound[1].view,p->bound[1].format,
        p->captured[0].view!=p->bound[0].view,p->captured[1].view!=p->bound[1].view,
        (unsigned long long)(uintptr_t)p->record_cb,(unsigned long long)(uintptr_t)p->seal_cb,cb_match,
        (unsigned long long)p->predicted,(unsigned long long)p->actual,
        (unsigned long long)p->observed_seal,(unsigned long long)p->record_serial,
        (unsigned long long)p->seal_serial,serial_match,ticket_mismatch);
    char error[256]={0};
    int ok=0;
    if (!atomic_load(&L.live) || !L.probe_write)
        snprintf(error,sizeof error,"LSFG probe backend unavailable");
    else if (!match) snprintf(error,sizeof error,"probe generation does not match native dumped frame");
    else if (!s->context_valid || s->context!=p->context)
        snprintf(error,sizeof error,"probe context retired or replaced before export");
    else if (!p->recorded)
        snprintf(error,sizeof error,"%s",p->error[0] ? p->error : "probe was not recorded");
    else ok=L.probe_write(p->context,directory,error);
    if (!ok && !error[0]) snprintf(error,sizeof error,"backend probe export failed without reason");
    fprintf(receipt,"status=%s error=%s\n",ok ? "ok" : "error",ok ? "none" : error);
    if (!ok) LOGV("[lsfg-probe] frame=%llu error=%s (native raw originals retained)",
        (unsigned long long)frame,error);
    pthread_mutex_unlock(&L.mu); return ok;
}
void vk_fg_lsfg_stats(void) {
    if (!vk_fg_lsfg_requested() || !L.diag) return;
    pthread_mutex_lock(&L.mu);
    const char *reason=atomic_load(&refusal_reason);
    LOGV("framegen lsfg last 300 real: submitted=%llu generated=%llu post-budget-or-timing-refused=%llu refused=%llu missing-timing=%llu timeout=%llu contexts=%llu rebinds=%llu LSFG-GPU mean/max=%.3f/%.3f ms samples=%llu late=%llu startup-ignored=%llu cooldown-skipped=%llu cooldown-left=%llu sustained-misses=%u fallback=%d reason=%s (same-device model included ONCE in real ticket; incremental is synthetic presentation; model=%s flow=%.2f)",
        (unsigned long long)L.submitted,(unsigned long long)L.generated,(unsigned long long)L.post_refused,
        (unsigned long long)atomic_exchange(&refusals,0),(unsigned long long)L.missing,
        (unsigned long long)L.timeout,(unsigned long long)L.contexts,(unsigned long long)L.rebinds,
        L.timed ? L.sum_ms/L.timed : NAN,L.timed ? L.max_ms : NAN,(unsigned long long)L.timed,
        (unsigned long long)L.policy.failures[FG_QCOM_DEADLINE],(unsigned long long)L.policy.startup_ignored,
        (unsigned long long)L.policy.cooldown_skips,
        (unsigned long long)(L.policy.cooldown_until>L.policy.tick ? L.policy.cooldown_until-L.policy.tick : 0),
        L.policy.misses,atomic_load(&refused),reason ? reason : "none",L.performance ? "performance" : "normal",L.flow);
    for (unsigned p=0;L.timed && p<TOTK_LSFG_PASSES;p++)
        LOGV("[lsfg-pass] pass=%u mean=%.6f max=%.6f ms samples=%llu",p,L.pass_sum[p]/L.timed,L.pass_max[p],(unsigned long long)L.timed);
    L.submitted=L.generated=L.post_refused=L.timed=L.missing=L.timeout=L.contexts=L.rebinds=0;
    L.sum_ms=L.max_ms=0; memset(L.pass_sum,0,sizeof L.pass_sum); memset(L.pass_max,0,sizeof L.pass_max);
    memset(L.policy.failures,0,sizeof L.policy.failures); L.policy.startup_ignored=L.policy.cooldown_skips=0;
    pthread_mutex_unlock(&L.mu);
}
#else
void vk_fg_lsfg_device(PFN_vkGetInstanceProcAddr ip,PFN_vkGetDeviceProcAddr dp,int interop) {
    (void)ip; (void)dp; (void)interop;
    if (vk_fg_lsfg_requested()) vk_fg_lsfg_refuse("user-DLL backend is only packaged in the Android app");
}
int vk_fg_lsfg_active(void) { return 0; }
void vk_fg_lsfg_late(void) {}
void vk_fg_lsfg_tick(int scene) {
    (void)scene;
    if (vk_fg_lsfg_requested())
        vk_overlay_set_framegen(VK_OVERLAY_FG_LSFG,VK_OVERLAY_FG_INTERP,VK_OVERLAY_FG_REFUSED,0,1);
}
int vk_fg_lsfg_can_generate(void) { return 0; }
unsigned vk_fg_lsfg_pending(void) { return 0; }
void vk_fg_lsfg_rearm(void) {}
int vk_fg_lsfg_capture(VkCommandBuffer cb,unsigned source,VkImage scene,VkImageView view,VkFormat format,uint32_t w,uint32_t h) {
    (void)cb; (void)source; (void)scene; (void)view; (void)format; (void)w; (void)h; return 0;
}
int vk_fg_lsfg_interpolate(unsigned previous,unsigned current,uint64_t frame,float phase,
        const vk_fg_lsfg_provenance *provenance,VkImage *image,VkImageView *view) {
    (void)previous; (void)current; (void)frame; (void)phase; (void)provenance; (void)image; (void)view; return 0;
}
void vk_fg_lsfg_submitting(VkCommandBuffer commands) { (void)commands; }
void vk_fg_lsfg_submitted(uint64_t actual_fence) { (void)actual_fence; }
void vk_fg_lsfg_sealed(const VkCommandBuffer *commands,unsigned count,uint64_t actual_fence) {
    (void)commands; (void)count; (void)actual_fence;
}
void vk_fg_lsfg_release(VkCommandBuffer cb,unsigned source) { (void)cb; (void)source; }
void vk_fg_lsfg_harvest(void) {}
void vk_fg_lsfg_result(int accepted) { (void)accepted; }
void vk_fg_lsfg_stats(void) {}
int vk_fg_lsfg_dump_image(unsigned source,VkImage *image,VkFormat *format,uint32_t *width,uint32_t *height) {
    (void)source; (void)image; (void)format; (void)width; (void)height; return 0;
}
static int probe_export(unsigned source,uint64_t frame,const char *directory,FILE *receipt) {
    (void)source; (void)frame; (void)directory;
    fprintf(receipt,"status=error error=LSFG probe unsupported outside Android app\n");
    return 0;
}
#endif
int vk_fg_lsfg_probe_dump(unsigned source,uint64_t frame,const char *directory) {
    if (!vk_fg_lsfg_probe_requested()) return 0;
    char path[1200];
    int length=directory ? snprintf(path,sizeof path,"%s/lsfg-probe.txt",directory) : -1;
    if (length<0 || length>=(int)sizeof path) {
        LOGV("[lsfg-probe] frame=%llu error=invalid receipt directory",(unsigned long long)frame);
        return 0;
    }
    FILE *receipt=fopen(path,"w");
    if (!receipt) {
        LOGV("[lsfg-probe] frame=%llu error=cannot open %s",(unsigned long long)frame,path);
        return 0;
    }
    fprintf(receipt,"dumped-frame=%llu source=%u fence=existing-native-dump-flush-wait-completed\n",
        (unsigned long long)frame,source);
    int ok=probe_export(source,frame,directory,receipt);
    int failed=ferror(receipt);
    if (fclose(receipt)) failed=1;
    if (failed) {
        LOGV("[lsfg-probe] frame=%llu error=receipt write failed",(unsigned long long)frame);
        return 0;
    }
    return ok;
}
