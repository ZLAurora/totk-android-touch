/* Real Vulkan split/continuation lifetime fixture. No Android/device/game data.
 * argv[1] is the existing CS (salt 3) produced by test_vkdraw_seed.py. */
#include "vk_int.h"
#include "vk_framegen.h"
#include "vk_record.h"
#include <errno.h>
#include <limits.h>
#include <sched.h>
#include <time.h>

static unsigned checks,failures;
#define CHECK(c,label) do { checks++; if (!(c)) { failures++; fprintf(stderr,"FAIL: %s\n",label); } } while (0)
/* A production allocation can block before the test reaches a join. Bound
 * each real phase independently, without opening a GPU gate or clearing a pin
 * to rescue a failure (the same liveness pattern as test_vk.c). */
static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    struct timespec deadline;
    const char *phase;
    unsigned generation;
    int done;
} liveness={.mu=PTHREAD_MUTEX_INITIALIZER,.cv=PTHREAD_COND_INITIALIZER};
static void liveness_phase(const char *phase,unsigned seconds) {
    pthread_mutex_lock(&liveness.mu);
    liveness.phase=phase; liveness.generation++;
    if (clock_gettime(CLOCK_REALTIME,&liveness.deadline)) { perror("liveness clock"); _Exit(1); }
    liveness.deadline.tv_sec+=seconds;
    pthread_cond_signal(&liveness.cv);
    fprintf(stderr,"[replay-pin] phase: %s\n",phase);
    pthread_mutex_unlock(&liveness.mu);
}
static void *liveness_timeout(void *unused) {
    (void)unused;
    pthread_mutex_lock(&liveness.mu);
    while (!liveness.done) {
        struct timespec deadline=liveness.deadline;
        unsigned generation=liveness.generation;
        int error=pthread_cond_timedwait(&liveness.cv,&liveness.mu,&deadline);
        if ((error==ETIMEDOUT && generation==liveness.generation && !liveness.done) ||
                (error && error!=ETIMEDOUT)) {
            fprintf(stderr,"FAIL: replay-pin liveness during %s (condition=%d); no GPU gate/pin was released to rescue it\n",
                liveness.phase,error);
            fflush(stderr); _Exit(1);
        }
    }
    pthread_mutex_unlock(&liveness.mu); return NULL;
}
static void join_bounded(pthread_t thread,const char *phase) {
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME,&deadline)) { perror("join clock"); _Exit(1); }
    deadline.tv_sec+=10;
    int error=pthread_timedjoin_np(thread,NULL,&deadline);
    CHECK(!error,phase);
    if (error) {
        fprintf(stderr,"FAIL: bounded replay-pin join: %s (error=%d)\n",phase,error);
        fflush(stderr); _Exit(1);
    }
}
static void wait_ticket(uint64_t ticket,const char *label) {
    int complete=ticket && mtl_wait(ticket,UINT64_C(2000000000));
    CHECK(complete,label);
    /* A failed completion is not authority to read/destroy its resources or
     * run the next scenario with an unfinished queue frontier. */
    if (!complete) { fflush(stderr); _Exit(1); }
}
static VkDescriptorSetLayout dsl;
static VkPipelineLayout layout;
static VkPipeline pipeline;
static void make_pipeline(const char *path) {
    FILE *file=fopen(path,"rb"); if (!file) { perror(path); exit(2); }
    fseek(file,0,SEEK_END); long bytes=ftell(file); rewind(file);
    if (bytes<=0 || bytes%4) exit(2);
    uint32_t *code=malloc((size_t)bytes);
    if (!code || fread(code,1,(size_t)bytes,file)!=(size_t)bytes) exit(2);
    fclose(file);
    VkShaderModuleCreateInfo mi={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=(size_t)bytes,.pCode=code};
    VkShaderModule module; VKCHECK(vkCreateShaderModule(V.dev,&mi,NULL,&module)); free(code);
    VkDescriptorSetLayoutBinding binding={.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo di={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=1,.pBindings=&binding};
    VKCHECK(vkCreateDescriptorSetLayout(V.dev,&di,NULL,&dsl));
    VkPipelineLayoutCreateInfo li={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&dsl};
    VKCHECK(vkCreatePipelineLayout(V.dev,&li,NULL,&layout));
    VkComputePipelineCreateInfo ci={VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.layout=layout,
        .stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=module,.pName="main"}};
    VKCHECK(vkCreateComputePipelines(V.dev,VK_NULL_HANDLE,1,&ci,NULL,&pipeline));
    vkDestroyShaderModule(V.dev,module,NULL);
}
static VkDescriptorSet descriptor(VkBuffer buffer,VkDeviceSize offset) {
    VkDescriptorSet set=vk_alloc_set(dsl);
    VkDescriptorBufferInfo info={buffer,offset,256};
    VkWriteDescriptorSet write={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=0,
        .descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&info};
    vkUpdateDescriptorSets(V.dev,1,&write,0,NULL); return set;
}
/* Every churn submission also stages a live descriptor-stock change (on/off and its ceiling) for the next slot
   generations: pinned slots must keep their pools while the free ones reset or destroy theirs. */
static _Atomic unsigned churns;
static void churn(void) {
    static const char *const toggles[] = {"set TOTK_VK_DESC_STOCK=1\n", "set TOTK_VK_DESC_STOCK_MAX_SETS=1\n",
        "set TOTK_VK_DESC_STOCK=0\n", "unset TOTK_VK_DESC_STOCK_MAX_SETS\nset TOTK_VK_DESC_STOCK=1\n", "unset TOTK_VK_DESC_STOCK\n"};
    unsigned n = atomic_fetch_add(&churns, 1);
    const char *lines = toggles[n % (sizeof toggles / sizeof *toggles)];
    totk_live_lines l = {{0}}; totk_live_feed(&l, lines, strlen(lines)); totk_live_frame(n);
    VkBuffer buffer; VkDeviceSize offset;
    memset(vk_transient(512,256,&buffer,&offset),0xe7,512);
    (void)descriptor(buffer,offset);
}
typedef struct { uint64_t ticket; _Atomic int started; } wrapped;
static void *wrap_slot(void *arg) {
    wrapped *result=arg;
    vk_rec_lock(); atomic_store(&result->started,1); churn(); result->ticket=vk_submit_now(); vk_rec_unlock(); return NULL;
}
static void scenario(int cancel) {
    liveness_phase(cancel ? "single split cancellation and ring wrap" : "single split replay and ring wrap",15);
    vk_submit_external_enable();
    VkBuffer readback; VkDeviceMemory memory;
    VkBufferCreateInfo bi={VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=512,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VKCHECK(vkCreateBuffer(V.dev,&bi,NULL,&readback));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev,readback,&req);
    VkMemoryAllocateInfo ai={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,
        .memoryTypeIndex=(uint32_t)vk_pick_type(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VKCHECK(vkAllocateMemory(V.dev,&ai,NULL,&memory)); VKCHECK(vkBindBufferMemory(V.dev,readback,memory,0));
    uint32_t *pixels; VKCHECK(vkMapMemory(V.dev,memory,0,512,0,(void **)&pixels)); memset(pixels,0xa5,512);
    VkSemaphore signal; VkSemaphoreCreateInfo si={VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VKCHECK(vkCreateSemaphore(V.dev,&si,NULL,&signal));
    VkEvent gate; VkEventCreateInfo ei={VK_STRUCTURE_TYPE_EVENT_CREATE_INFO}; VKCHECK(vkCreateEvent(V.dev,&ei,NULL,&gate));
    vk_rec_lock();
    VkBuffer source; VkDeviceSize offset;
    uint32_t *bytes=vk_transient(512,256,&source,&offset);
    for (unsigned i=0;i<128;i++) bytes[i]=0x98760000u+i;
    VkDescriptorSet frozen=descriptor(source,offset);
    uint64_t serial=vk_submit_serial();
    CHECK(!vk_submit_signal(VK_NULL_HANDLE) && serial==vk_submit_serial(),"refused split does not seal a primary");
    uint64_t first=vk_submit_signal(signal);
    wait_ticket(first,"input ticket completes independently of resource retention");
    uint64_t until=0,skips=0;
    CHECK(vk_record_test_replay_pin(first,&until,&skips) && until==UINT64_MAX,
        "completed input still owns its transient arena and descriptors before continuation sealing");
    CHECK(!vk_submit_external_disable(),"fallback cannot disable retention while an unsealed continuation owns resources");
    CHECK(!vk_submit_signal(signal),"an unsealed continuation cannot start a nested signal split");
    liveness_phase("completed split input; record gated terminal continuation",15);
    vk_submit_wait(signal);
    VkCommandBuffer cb=vk_cb();
    vkCmdWaitEvents(cb,1,&gate,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,NULL,0,NULL,0,NULL);
    if (!cancel) {
        vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
        vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&frozen,0,NULL);
        vkCmdDispatch(cb,1,1,1); vk_barrier_all(cb);
        VkBufferCopy copy={offset,0,512}; vkCmdCopyBuffer(cb,source,readback,1,&copy);
    }
    uint64_t terminal=vk_submit_now();
    CHECK(vk_record_test_replay_pin(first,&until,&skips) && until==terminal,
        "pin follows the exact terminal continuation ticket, not the earlier signal fence");
    CHECK(!mtl_wait(terminal,0) && !vk_present_ready_ns(terminal),
        "unfinished generated output has no completed ticket/readiness to present");
    liveness_phase("fill six free slots behind pinned input and gated continuation",15);
    for (unsigned i=0;i<6;i++) { churn(); (void)vk_submit_now(); }
    uint64_t before_skips=skips;
    vk_rec_unlock();
    wrapped result={0}; pthread_t worker; int error=pthread_create(&worker,NULL,wrap_slot,&result);
    CHECK(!error,"start wrap producer"); if (error) _Exit(1);
    uint64_t start=host_now_ns();
    while (host_now_ns()-start<UINT64_C(2000000000)) {
        (void)vk_record_test_replay_pin(first,&until,&skips);
        if (skips>before_skips) break;
        sched_yield();
    }
    CHECK(skips>before_skips && until==terminal,
        "ring wrap skips the completed-but-pinned input instead of resetting its arena/pools");
    for (unsigned i=64;i<128;i++) CHECK(bytes[i]==0x98760000u+i,
        "vertex/UBO-like transient bytes survive input completion and attempted ring reuse");
    CHECK(!mtl_wait(terminal,0),"timeout/cancellation does not turn pending GPU work into retirement");
    CHECK(!vk_submit_external_disable(),"cooldown/timeout cannot disengage an in-flight continuation pin");
    liveness_phase("open single-split gate and join wrap producer",15);
    VKCHECK(vkSetEvent(V.dev,gate)); join_bounded(worker,"single-split wrap producer exits after its GPU gate opens");
    wait_ticket(result.ticket,"continuation and wrapped producer finish after the actual GPU gate opens");
    CHECK(vk_record_test_replay_pin(first,&until,&skips) && !until,"input resources unpin only after terminal completion");
    CHECK(vk_submit_external_disable(),"fallback disengages the external sidecar after actual terminal retirement");
    for (unsigned i=0;i<128;i++) {
        uint32_t expected=cancel ? 0xa5a5a5a5u : i<64 ? (i^0x12345678u) : 0x98760000u+i;
        CHECK(pixels[i]==expected,"frozen descriptor and transient bindings produce exact bytes, or canceled output stays untouched");
    }
    liveness_phase("recycle retired single-split ring slots",15);
    vk_rec_lock();
    for (unsigned i=0;i<24;i++) { churn(); uint64_t ticket=vk_submit_now(); wait_ticket(ticket,"retired ring continues recycling after split"); }
    CHECK(!vk_record_test_replay_pin(first,&until,&skips),"old input incarnation is eventually recycled after its pin retires");
    vk_rec_unlock();
    vkDestroyEvent(V.dev,gate,NULL); vkDestroySemaphore(V.dev,signal,NULL);
    vkUnmapMemory(V.dev,memory); vkDestroyBuffer(V.dev,readback,NULL); vkFreeMemory(V.dev,memory,NULL);
}
static void wait_gate(VkEvent gate) {
    vkCmdWaitEvents(vk_cb(),1,&gate,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,NULL,0,NULL,0,NULL);
}
static void replay_descriptor(VkDescriptorSet frozen,VkBuffer source,VkDeviceSize offset,VkBuffer readback) {
    VkCommandBuffer cb=vk_cb();
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
    vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&frozen,0,NULL);
    vkCmdDispatch(cb,1,1,1); vk_barrier_all(cb);
    VkBufferCopy copy={offset,0,512}; vkCmdCopyBuffer(cb,source,readback,1,&copy);
}
/* Two real owners overlap. Four split/continuation pairs wrap the eight-slot
 * ring before the first owner's FINAL phase, not just its first replay. */
static void owner_lease_scenario(void) {
    liveness_phase("original real owner and four adaptive phases",15);
    VkBuffer readback[2],source[2]; VkDeviceMemory memory[2]; VkDeviceSize offset[2];
    uint32_t *pixels[2],*bytes[2]; VkDescriptorSet frozen[2]; VkEvent gate[2];
    uint64_t lease[2],real[2],terminal[2],until=0,skips=0,initial_skips=0;
    vk_submit_external_enable();
    for (unsigned n=0;n<2;n++) {
        VkBufferCreateInfo bi={VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=512,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VKCHECK(vkCreateBuffer(V.dev,&bi,NULL,&readback[n]));
        VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev,readback[n],&req);
        VkMemoryAllocateInfo ai={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,
            .memoryTypeIndex=(uint32_t)vk_pick_type(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        VKCHECK(vkAllocateMemory(V.dev,&ai,NULL,&memory[n]));
        VKCHECK(vkBindBufferMemory(V.dev,readback[n],memory[n],0));
        VKCHECK(vkMapMemory(V.dev,memory[n],0,512,0,(void **)&pixels[n])); memset(pixels[n],0xa5,512);
        VkEventCreateInfo ei={VK_STRUCTURE_TYPE_EVENT_CREATE_INFO}; VKCHECK(vkCreateEvent(V.dev,&ei,NULL,&gate[n]));
    }
    VkSemaphore signal; VkSemaphoreCreateInfo si={VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VKCHECK(vkCreateSemaphore(V.dev,&si,NULL,&signal));
    vk_rec_lock();
    bytes[0]=vk_transient(512,256,&source[0],&offset[0]);
    for (unsigned i=0;i<128;i++) bytes[0][i]=0x98760000u+i;
    frozen[0]=descriptor(source[0],offset[0]); lease[0]=vk_replay_lease_begin();
    CHECK(lease[0] && !vk_replay_lease_begin(),"one lease owns the actual real recording slot; nested begin is refused");
    real[0]=vk_submit_now();
    wait_ticket(real[0],"real owner GPU completion does not end a multi-phase lease");
    CHECK(vk_record_test_replay_pin(real[0],&until,&initial_skips) && until==UINT64_MAX,
        "real owner is pinned before any external split can select a different slot");
    for (unsigned phase=0;phase<4;phase++) {
        liveness_phase(phase==3 ? "final gated adaptive phase" : "early adaptive phase retirement",15);
        uint64_t split=vk_submit_signal(signal);
        CHECK(split!=0,"each adaptive phase admits its own external split");
        vk_submit_wait(signal);
        if (phase==3) wait_gate(gate[0]);
        replay_descriptor(frozen[0],source[0],offset[0],readback[0]);
        terminal[0]=vk_submit_now();
        if (phase<3) wait_ticket(terminal[0],"earlier adaptive phases retire independently");
        CHECK(vk_record_test_replay_pin(real[0],&until,&skips) && until==UINT64_MAX,
            "neither a signal nor an earlier continuation releases the original real owner");
    }
    CHECK(skips>initial_skips,"four phases wrap the ring and skip the original owner's descriptor/transient pools");
    CHECK(!vk_replay_lease_end(lease[0],UINT64_MAX),"future covering tickets are refused without releasing the lease");
    CHECK(vk_record_test_replay_pin(real[0],&until,&skips) && until==UINT64_MAX,"invalid release leaves the original pin intact");
    CHECK(vk_replay_lease_end(lease[0],real[0]),"an old covering hint extends through the latest phase ticket");
    CHECK(vk_record_test_replay_pin(real[0],&until,&skips) && until==terminal[0],"release covers the LAST adaptive phase");
    CHECK(!vk_replay_lease_end(lease[0],0),"a stale lease token cannot mutate a deferred pin");
    bytes[1]=vk_transient(512,256,&source[1],&offset[1]);
    for (unsigned i=0;i<128;i++) bytes[1][i]=0x12340000u+i;
    frozen[1]=descriptor(source[1],offset[1]); lease[1]=vk_replay_lease_begin(); real[1]=vk_submit_now();
    CHECK(lease[1] && !mtl_wait(real[1],0),"a second real frame owns a distinct lease while the first is still in flight");
    CHECK(vk_record_test_replay_pin(real[1],&until,&skips) && until==UINT64_MAX,
        "second real remains leased while its covering replay has not yet sealed");
    /* A retains original owner + final split source + gated continuation; B
     * adds its real owner. Fill the FOUR remaining slots, then attempt reuse on
     * a separate worker. Only A's gate is queued: recording B's HOST->ALL gate
     * now would assume later event execution cannot hold A's fence frontier. */
    liveness_phase("fill four free slots behind A final split and both real owners",15);
    for (unsigned i=0;i<4;i++) { churn(); (void)vk_submit_now(); }
    vk_rec_unlock();
    wrapped result={0}; pthread_t worker; int error=pthread_create(&worker,NULL,wrap_slot,&result);
    CHECK(!error,"start mixed-frame ring-pressure producer"); if (error) _Exit(1);
    uint64_t start=host_now_ns();
    while (!atomic_load(&result.started) && host_now_ns()-start<UINT64_C(2000000000)) sched_yield();
    CHECK(atomic_load(&result.started),"ring-pressure producer enters the production allocator");
    CHECK(vk_record_test_replay_pin(real[0],&until,&skips) && until==terminal[0],"ring pressure cannot reset the first source before its covering ticket");
    CHECK(vk_record_test_replay_pin(real[1],&until,&skips) && until==UINT64_MAX,"ring pressure cannot reset the second source before its covering replay is even sealed");
    for (unsigned i=64;i<128;i++) {
        CHECK(bytes[0][i]==0x98760000u+i,"first owner's retained transient tail survives all four phases and ring pressure");
        CHECK(bytes[1][i]==0x12340000u+i,"second owner's retained transient tail survives mixed-frame ring pressure");
    }
    CHECK(!mtl_wait(terminal[0],0) && !mtl_wait(real[1],0),"timeouts do not count as either frame's GPU retirement");
    CHECK(!vk_submit_external_disable(),"external failure/cooldown cannot disable in-flight owner retention");
    liveness_phase("open A final gate without any later unsignaled event in the queue",15);
    VKCHECK(vkSetEvent(V.dev,gate[0]));
    wait_ticket(terminal[0],"first covering ticket retires after its actual GPU gate opens");
    CHECK(!vk_record_test_replay_pin(real[0],&until,&skips) || !until,"first owner is released after its final ticket retires");
    CHECK(vk_record_test_replay_pin(real[1],&until,&skips) && until==UINT64_MAX,"first-frame retirement cannot release the second frame's still-open owner");
    liveness_phase("join wrap producer after A retirement while B owner remains leased",15);
    join_bounded(worker,"mixed-frame wrap producer exits after A's GPU gate opens");
    wait_ticket(result.ticket,"first frame, second real and wrap producer retire without a pin deadlock");
    wait_ticket(real[1],"second real GPU fence retires independently of its still-open replay lease");
    CHECK(vk_record_test_replay_pin(real[1],&until,&skips) && until==UINT64_MAX,
        "completed second real still owns its frozen descriptor and transient arena after ring pressure");
    vk_rec_lock();
    wait_gate(gate[1]); replay_descriptor(frozen[1],source[1],offset[1],readback[1]); terminal[1]=vk_submit_now();
    CHECK(vk_replay_lease_end(lease[1],0),"second owner release follows its actual covering replay submission");
    CHECK(vk_record_test_replay_pin(real[1],&until,&skips) && until==terminal[1],
        "second owner remains pinned through its exact later covering ticket");
    CHECK(!mtl_wait(terminal[1],0) && !vk_submit_external_disable(),
        "second gated replay cannot retire or disable retention merely because both real tickets completed");
    vk_rec_unlock();
    liveness_phase("open sequential B replay gate and retire its covering ticket",15);
    VKCHECK(vkSetEvent(V.dev,gate[1]));
    wait_ticket(terminal[1],"second replay retires only after its own sequential GPU gate opens");
    CHECK(vk_record_test_replay_pin(real[1],&until,&skips) && !until,"second owner releases only after its own covering ticket");
    for (unsigned n=0;n<2;n++) for (unsigned i=0;i<128;i++) {
        uint32_t expected=i<64 ? (i^0x12345678u) : (n ? 0x12340000u : 0x98760000u)+i;
        CHECK(pixels[n][i]==expected,"original frozen descriptor and transient owner produce exact bytes after ring wrap");
    }
    CHECK(vk_submit_external_disable(),"all mixed-frame owner and split pins drain after GPU retirement");
    vk_rec_lock();
    for (unsigned i=0;i<16;i++) { churn(); uint64_t ticket=vk_submit_now(); wait_ticket(ticket,"released owner slots return to ordinary reuse"); }
    CHECK(!vk_record_test_replay_pin(real[0],&until,&skips) && !vk_record_test_replay_pin(real[1],&until,&skips),
        "both original slot incarnations are reusable after their covering tickets");
    vk_rec_unlock();
    vkDestroySemaphore(V.dev,signal,NULL);
    for (unsigned n=0;n<2;n++) {
        vkDestroyEvent(V.dev,gate[n],NULL); vkUnmapMemory(V.dev,memory[n]);
        vkDestroyBuffer(V.dev,readback[n],NULL); vkFreeMemory(V.dev,memory[n],NULL);
    }
}
/* Six owner leases plus one external pin must leave the eighth slot runnable.
 * Cancellation before that continuation seals cannot drop an owner early. */
static void lease_pressure_scenario(void) {
    liveness_phase("six owner leases plus external split and reserved cleanup slot",15);
    uint64_t lease[6],source[6],until=0,skips=0;
    VkEvent gate; VkEventCreateInfo ei={VK_STRUCTURE_TYPE_EVENT_CREATE_INFO}; VKCHECK(vkCreateEvent(V.dev,&ei,NULL,&gate));
    VkSemaphore signal; VkSemaphoreCreateInfo si={VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VKCHECK(vkCreateSemaphore(V.dev,&si,NULL,&signal)); vk_submit_external_enable();
    vk_rec_lock();
    for (unsigned i=0;i<6;i++) {
        liveness_phase("retire an individual real fence while retaining its open owner lease",15);
        churn();
        lease[i]=vk_replay_lease_begin(); source[i]=vk_submit_now();
        CHECK(lease[i]!=0,"mixed pressure admits six distinct owner leases");
        if (!lease[i]) { fflush(stderr); _Exit(1); }
        wait_ticket(source[i],"each real fence retires before introducing the single cleanup gate");
        CHECK(vk_record_test_replay_pin(source[i],&until,&skips) && until==UINT64_MAX,
            "each of six GPU-completed real slots stays pinned by its open production owner lease");
    }
    uint64_t split=vk_submit_signal(signal); CHECK(split!=0,"seventh distinct pin admits the external handoff");
    wait_ticket(split,"seventh external input fence completes independently while all six real owners stay leased");
    CHECK(vk_record_test_replay_pin(split,&until,&skips) && until==UINT64_MAX,
        "seventh mixed pin survives its own actual GPU completion until cleanup is sealed");
    liveness_phase("record sole gated cleanup in reserved eighth slot with seven retired-but-pinned inputs",15);
    churn(); uint64_t serial=vk_submit_serial();
    CHECK(!vk_replay_lease_begin() && vk_submit_serial()==serial,"eighth owner admission is refused without sealing or consuming the runnable slot");
    CHECK(!vk_submit_signal(signal) && vk_submit_serial()==serial,"failed/nested external split does not submit or release an existing owner");
    CHECK(vk_replay_lease_end(lease[0],0),"canceled/no-output owner can end before its cleanup continuation seals");
    CHECK(vk_record_test_replay_pin(source[0],&until,&skips) && until==UINT64_MAX,"unsealed cancellation cleanup retains the source indefinitely, not just through the split");
    vk_submit_wait(signal); wait_gate(gate); uint64_t terminal=vk_submit_now();
    for (unsigned i=1;i<6;i++) CHECK(vk_replay_lease_end(lease[i],0),"remaining no-output owner releases defer through the final cleanup ticket");
    for (unsigned i=0;i<6;i++) CHECK(vk_record_test_replay_pin(source[i],&until,&skips) && until==terminal,
        "mixed owner/split pressure keeps all six sources until the same actual covering completion");
    CHECK(!vk_submit_external_disable(),"cancellation/external failure cannot disengage queued lease retention");
    vk_rec_unlock(); VKCHECK(vkSetEvent(V.dev,gate));
    wait_ticket(terminal,"reserved eighth slot submits cleanup without waiting on an open lease");
    for (unsigned i=0;i<6;i++) CHECK(vk_record_test_replay_pin(source[i],&until,&skips) && !until,"covering retirement releases each no-output owner");
    CHECK(vk_submit_external_disable(),"mixed pressure drains every owner and external pin");
    vkDestroySemaphore(V.dev,signal,NULL); vkDestroyEvent(V.dev,gate,NULL);
}
static void lease_no_output_scenario(void) {
    liveness_phase("no-output owner retirement without external admission",15);
    VkEvent gate; VkEventCreateInfo ei={VK_STRUCTURE_TYPE_EVENT_CREATE_INFO}; VKCHECK(vkCreateEvent(V.dev,&ei,NULL,&gate));
    uint64_t until=0,skips=0;
    vk_rec_lock();
    uint64_t serial=vk_submit_serial();
    CHECK(!vk_replay_lease_begin() && vk_submit_serial()==serial,"no current recorder refuses a lease without allocating or submitting");
    churn(); wait_gate(gate); uint64_t lease=vk_replay_lease_begin(),source=vk_submit_now();
    CHECK(lease && vk_replay_lease_end(lease,0),"no generated output releases through the real ticket without external-engine admission");
    CHECK(vk_record_test_replay_pin(source,&until,&skips) && until==source,"dropping the real presenter does not release the real GPU owner");
    CHECK(!mtl_wait(source,0),"no-output timeout is not retirement");
    vk_rec_unlock(); VKCHECK(vkSetEvent(V.dev,gate));
    wait_ticket(source,"no-output owner retires on its genuine real ticket");
    CHECK(vk_record_test_replay_pin(source,&until,&skips) && !until,"no-output lease drains without another submission");
    vkDestroyEvent(V.dev,gate,NULL);
}
static void recorder_primitive_scenario(void) {
    liveness_phase("recorder immutable lease and frozen-copy primitives",20);
    vk_rec_lock();
    VkBuffer buffer; VkDeviceSize offset;
    uint8_t *source=vk_transient(64,16,&buffer,&offset),copy[16],expected[16],retained[16];
    for (unsigned i=0;i<64;i++) source[i]=(uint8_t)(i^0x5a);
    memcpy(expected,source+8,sizeof expected);
    memset(copy,0xcc,sizeof copy);
    CHECK(vk_replay_frozen_buffer_copy(buffer,offset+8,sizeof copy,copy) &&
        !memcmp(copy,expected,sizeof copy),"frozen copy reads exact bytes from a written ring-arena range");
    memcpy(retained,copy,sizeof retained);
    memset(copy,0xcc,sizeof copy);
    CHECK(!vk_replay_frozen_buffer_copy((VkBuffer)(uintptr_t)UINTPTR_MAX,0,sizeof copy,copy) &&
        copy[0]==0xcc,"unknown buffer is rejected without changing destination");
    CHECK(!vk_replay_frozen_buffer_copy(buffer,offset+64,1,copy) &&
        copy[0]==0xcc,"range extending past the written arena prefix is rejected without changing destination");
    CHECK(!vk_replay_frozen_buffer_copy(buffer,UINT64_MAX-3,8,copy) &&
        copy[0]==0xcc,"overflowing range is rejected without changing destination");
    CHECK(!vk_replay_frozen_buffer_copy(buffer,offset,0,copy) &&
        copy[0]==0xcc,"zero-byte copy is rejected without changing destination");
    VkBuffer snapshot; VkDeviceSize snapshot_offset;
    vk_record_test_snapshot_allocate_range(0x12340000,&snapshot,&snapshot_offset);
    uint32_t snapshot_copy[4];
    CHECK(vk_replay_frozen_buffer_copy(snapshot,snapshot_offset,sizeof snapshot_copy,snapshot_copy) &&
        snapshot_copy[0]==0x12340000 && snapshot_copy[3]==0x12340003,
        "frozen copy accepts exactly written bytes from the separate snapshot arena");
    uint64_t ring_ticket=vk_submit_now();
    wait_ticket(ring_ticket,"ring source fence retires before exercising wrap reuse");
    for (unsigned i=0;i<2*8;i++) {
        uint8_t *reuse=vk_transient(64,16,&buffer,&offset);
        memset(reuse,(int)(0x80+i),64);
        wait_ticket(vk_submit_now(),"ring-wrap recorder submission completes");
    }
    CHECK(!memcmp(retained,expected,sizeof retained),
        "caller-owned frozen bytes remain protected across recorder ring wrap and source reuse");
    unsigned snapshots=0,expected_snapshots; uint64_t lease;
    vk_record_test_snapshot_allocate_range(0x23450000,&snapshot,&snapshot_offset);
    (void)vk_cb(); /* Snapshot allocation alone does not open a recorder slot. */
    expected_snapshots=vk_record_test_snapshot_mask();
    lease=vk_replay_lease_begin();
    uint64_t legacy_ticket=vk_submit_now();
    CHECK(lease && expected_snapshots &&
        vk_record_test_replay_snapshots(legacy_ticket,&snapshots) && snapshots==expected_snapshots,
        "legacy lease retains the exact current snapshot-arena mask");
    CHECK(vk_replay_lease_end(lease,legacy_ticket),"legacy snapshot lease releases through its covering ticket");
    wait_ticket(legacy_ticket,"legacy snapshot lease source retires");

    vk_record_test_snapshot_allocate_range(0x34560000,&snapshot,&snapshot_offset);
    (void)vk_cb();
    lease=vk_replay_lease_begin_immutable();
    uint64_t immutable_ticket=vk_submit_now();
    CHECK(lease && vk_record_test_replay_snapshots(immutable_ticket,&snapshots) &&
        snapshots==0,"immutable lease retains its slot but no snapshot arenas");
    CHECK(vk_replay_lease_end(lease,immutable_ticket),"immutable lease releases through its covering ticket");
    wait_ticket(immutable_ticket,"immutable lease source retires");
    vk_rec_unlock();
}
static void immutable_optional_split_scenario(void) {
    liveness_phase("immutable optional split retains only recorder slot resources",15);
    VkEvent gate; VkEventCreateInfo ei={VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
    VKCHECK(vkCreateEvent(V.dev,&ei,NULL,&gate));
    VkSemaphore signal; VkSemaphoreCreateInfo si={VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VKCHECK(vkCreateSemaphore(V.dev,&si,NULL,&signal));
    vk_rec_lock();
    VkBuffer snapshot; VkDeviceSize snapshot_offset;
    vk_record_test_snapshot_allocate_range(0x45670000,&snapshot,&snapshot_offset);
    CHECK(vk_submit_optional_begin_immutable(),"immutable optional lane claims split and consumer slots");
    uint64_t split=vk_submit_signal(signal),until=0,skips=0;
    CHECK(split && !vk_record_test_snapshot_external_until(),
        "immutable external split does not pin global snapshot arenas");
    vk_submit_wait(signal); wait_gate(gate);
    uint64_t terminal=vk_submit_now();
    vk_submit_optional_end();
    CHECK(vk_record_test_replay_pin(split,&until,&skips) && until==terminal,
        "immutable external split still retains its source slot through the actual consumer fence");
    CHECK(!mtl_wait(terminal,0),"immutable consumer slot remains GPU-owned while its event gate is closed");
    vk_rec_unlock();
    VKCHECK(vkSetEvent(V.dev,gate));
    wait_ticket(terminal,"immutable optional consumer retires after the real GPU gate opens");
    CHECK(vk_record_test_replay_pin(split,&until,&skips) && !until,
        "immutable split slot retires only after its covering consumer fence");
    vkDestroySemaphore(V.dev,signal,NULL); vkDestroyEvent(V.dev,gate,NULL);
}
int main(int argc,char **argv) {
    if (argc!=2) { fprintf(stderr,"usage: vkreplaypintest <salt3-compute.spv>\n"); return 2; }
    char seed[PATH_MAX];
    if (!realpath(argv[1],seed)) { perror(argv[1]); return 2; }
    char *slash=strrchr(seed,'/');
    if (!slash) { fprintf(stderr,"missing shader seed parent directory\n"); return 2; }
    if (slash==seed) slash[1]=0; else *slash=0;
    setenv("TOTK_SHADER_SEED_DIR",seed,1);
    setenv("TOTK_FRAMEGEN_ENGINE","qcom",1); setenv("TOTK_FRAMEGEN","locked",1);
    setenv("TOTK_REALTIME","1",1); setenv("TOTK_FPS60","0",1);
    setenv("TOTK_FPS_AUTO","0",1); setenv("TOTK_UNCAPPED","0",1);
    setenv("TOTK_PRESENT_SLOTS","1",1); setenv("TOTK_FRAMEGEN_LEAD_REFRESHES","1",1);
    unsetenv("TOTK_FRAMEGEN_BUDGET_MS");
    liveness_phase("Vulkan initialization, admitted locked policy and salt3 pipeline creation",60);
    pthread_t watchdog; int error=pthread_create(&watchdog,NULL,liveness_timeout,NULL);
    if (error) { fprintf(stderr,"FAIL: replay-pin watchdog creation: %d\n",error); return 1; }
    mtl_init();
    CHECK(vk_fg_enabled() && vk_fg_interpolating() && vk_fg_locked(),
        "fixture admits the actual locked interpolation policy before testing replay readiness and ownership");
    if (failures) { fflush(stderr); _Exit(1); }
    /* Exercise the generic primitive explicitly; host GL admission is refused. */
    vk_submit_external_enable(); make_pipeline(argv[1]);
    recorder_primitive_scenario(); immutable_optional_split_scenario();
    scenario(0); scenario(1);
    owner_lease_scenario(); lease_pressure_scenario(); lease_no_output_scenario();
    uint64_t pool_checks,violations; vk_test_descriptor_pools_stats(&pool_checks,&violations);
    printf("replay pin descriptor pools: %u live stock toggles, %llu pool resets/destroys checked, %llu with an unretired user\n",
        atomic_load(&churns),(unsigned long long)pool_checks,(unsigned long long)violations);
    CHECK(pool_checks && !violations,"live stock toggles never reset or destroy a pinned or unretired slot's pools");
    liveness_phase("completed fixture cleanup",15);
    vkDestroyPipeline(V.dev,pipeline,NULL); vkDestroyPipelineLayout(V.dev,layout,NULL); vkDestroyDescriptorSetLayout(V.dev,dsl,NULL);
    pthread_mutex_lock(&liveness.mu); liveness.done=1; pthread_cond_signal(&liveness.cv); pthread_mutex_unlock(&liveness.mu);
    join_bounded(watchdog,"liveness watchdog exits after every real fixture phase completes");
    printf("external replay pin: %u checks, %u failed (descriptor/transient continuation, four-phase/multi-frame wrap, mixed pin pressure, timeout/cancellation/no-output)\n",checks,failures);
    return failures ? 1 : 0;
}
