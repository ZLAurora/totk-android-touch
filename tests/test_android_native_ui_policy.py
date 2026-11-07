"""Compile the production Android startup policy and pure guest/window extent helper.

No Android framework/GPU is mocked as passing: real copy-presentation pixels are
covered separately by vkoverlaytest --guest-output. TMPDIR selects scratch output.
"""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class AndroidNativeUiPolicy(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get('CC', 'cc'))
        if not compiler or not shutil.which(compiler[0]):
            raise unittest.SkipTest('A native C compiler is required')
        scratch = Path(os.environ.get('TMPDIR', ROOT / '.scratch'))
        scratch.mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='android-ui-policy-', dir=scratch)
        cls.addClassCleanup(cls.temp.cleanup)
        directory = Path(cls.temp.name)
        cls.source = (ROOT / 'games/totk/runtime/frontend_android.c').read_text()
        start = cls.source.index('static void android_native_ui_mode_policy(void) {')
        end = cls.source.index('static int start_game(', start)
        policy = cls.source[start:end]
        probe = directory / 'probe.c'
        probe.write_text('''#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "vk_present_policy.h"
static int handheld;
static int runtime_handheld_mode(void) { return handheld; }
''' + policy + '''
static const char *value(const char *name) { const char *v=getenv(name); return v ? v : "<unset>"; }
int main(int argc,char **argv) {
    if (argc==6 && !strcmp(argv[1],"extent")) {
        vk_present_rect r=vk_present_guest_fit(strtoul(argv[2],0,10),strtoul(argv[3],0,10),
            strtoul(argv[4],0,10),strtoul(argv[5],0,10));
        printf("%d %d %u %u\\n",r.x,r.y,r.w,r.h); return 0;
    }
    if (argc==9 && !strcmp(argv[1],"rotation")) {
        int turns=vk_present_transform_turns(atoi(argv[2]));
        if (turns<0) { printf("-1\\n"); return 0; }
        uint32_t w=strtoul(argv[3],0,10),h=strtoul(argv[4],0,10);
        vk_present_rect r={atoi(argv[5]),atoi(argv[6]),strtoul(argv[7],0,10),strtoul(argv[8],0,10)};
        vk_present_extent native=vk_present_rotate_extent(w,h,(unsigned)turns);
        vk_present_rect rotated=vk_present_rotate_rect(r,w,h,(unsigned)turns);
        vk_present_rect restored=vk_present_rotate_rect(rotated,native.w,native.h,(4-(unsigned)turns)&3);
        float matrix[4]; vk_present_rotation_matrix((unsigned)turns,matrix);
        printf("%d %d %u %u %d %d %u %u %d %d %u %u %.0f %.0f %.0f %.0f\\n",
            turns,vk_present_turns_transform((4-(unsigned)turns)&3),native.w,native.h,
            rotated.x,rotated.y,rotated.w,rotated.h,restored.x,restored.y,restored.w,restored.h,
            matrix[0],matrix[1],matrix[2],matrix[3]);
        return 0;
    }
    if (argc==6 && !strcmp(argv[1],"ahb-layout")) {
        printf("%d\\n",vk_present_ahb_layout_matches(strtoull(argv[2],0,10),strtoull(argv[3],0,10),
            strtoul(argv[4],0,10),strtoul(argv[5],0,10))!=0);
        return 0;
    }
    if (argc==3 && !strcmp(argv[1],"ubwc-driver")) {
        printf("%d\\n",vk_present_ubwc_driver_known(argv[2]));
        return 0;
    }
    if (argc!=2) return 2;
    handheld=atoi(argv[1]);
    android_native_ui_mode_policy(); android_native_ui_mode_policy();
    printf("%s\\n%s\\n%s\\n",value("TOTK_NATIVE_UI"),value("TOTK_OUTPUT_RES"),value("TOTK_APP_WIDTH"));
    return 0;
}
''')
        cls.probe = directory / 'probe'
        flags = [flag for name in ('CPPFLAGS', 'CFLAGS', 'LDFLAGS')
                 for flag in shlex.split(os.environ.get(name, ''))]
        result = subprocess.run([*compiler, '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                                 *flags, '-I', str(ROOT / 'games/totk/android/gfx'),
                                 str(probe), '-o', str(cls.probe)], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        callback_start = cls.source.index('static void on_window_resized(')
        callback_end = cls.source.index('static void on_queue_created(', callback_start)
        callbacks = cls.source[callback_start:callback_end]
        timeline_start = cls.source.index('static runtime_present_timelines present_timeline_snapshot;')
        timeline_end = cls.source.index('typedef struct {', timeline_start)
        timeline_lifecycle = cls.source[timeline_start:timeline_end]
        lifecycle_probe = directory / 'lifecycle_probe.c'
        lifecycle_probe.write_text('''#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include "present_timeline.h"
static pthread_mutex_t display_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t lifecycle_now = 1000;
static int adaptive_lifecycle_calls, adaptive_window, adaptive_paused;
static uint64_t runtime_now_ns(void) { return ++lifecycle_now; }
static int runtime_android_adaptive_slots_active(void) { return 1; }
static void adaptive_lifecycle(int window, int paused) {
    adaptive_lifecycle_calls++; adaptive_window=window; adaptive_paused=paused;
}
static void (*runtime_fps_adaptive_lifecycle)(int,int)=adaptive_lifecycle;
static int watch_paused=-1;   /* the stall watchdog's pause: no window or in the background */
static void runtime_progress_pause(int paused) { watch_paused=paused; }
''' + timeline_lifecycle + '''
typedef struct ANativeWindow { int id; } ANativeWindow;
typedef struct ANativeActivity { int unused; } ANativeActivity;
typedef struct ARect { int left, top, right, bottom; } ARect;
static ANativeWindow current_window = {1}, stale_window = {2};
static ANativeWindow *g_window = &current_window;
static uint64_t g_window_epoch = 7;
static int notification_count, retry_count;
static ANativeWindow *notified_window, *retried_window;
static void vk_window_resized(ANativeWindow *window, uint64_t epoch) {
    if (window == g_window && epoch == g_window_epoch) { notification_count++; notified_window = window; }
}
static void surface_parent_retry(ANativeActivity *a, ANativeWindow *window, const char *event) {
    (void)a; (void)event;
    if (window == g_window) { retry_count++; retried_window = window; }
}
''' + callbacks + '''
int main(int argc, char **argv) {
    ANativeActivity activity = {0};
    ARect rect = {0, 0, 100, 100};
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "timeline-lifecycle")) {
        present_timeline_generation=7; present_timeline_snapshot.count=2;
        present_timeline_window=1; present_timeline_paused=0;
        present_timeline_lifecycle(-1,1);
        int paused=present_timeline_generation==8 && !present_timeline_snapshot.count &&
            present_timeline_window==1 && present_timeline_paused==1 &&
            adaptive_slots_epoch_ns==1001 && adaptive_lifecycle_calls==1 && adaptive_window==-1 && adaptive_paused==1 &&
            watch_paused==1;
        present_timeline_snapshot.count=2;
        present_timeline_lifecycle(0,-1);
        int destroyed=present_timeline_generation==9 && !present_timeline_snapshot.count &&
            !present_timeline_window && present_timeline_paused==1 &&
            adaptive_slots_epoch_ns==1002 && adaptive_lifecycle_calls==2 && adaptive_window==0 && adaptive_paused==-1 &&
            watch_paused==1;
        present_timeline_lifecycle(1,0);
        int resumed=present_timeline_generation==10 && present_timeline_window==1 && !present_timeline_paused &&
            adaptive_slots_epoch_ns==1003 && adaptive_lifecycle_calls==3 && adaptive_window==1 && adaptive_paused==0 &&
            !watch_paused;
        present_timeline_snapshot.count=2;
        present_timeline_lifecycle(-1,-1);
        printf("%d %d %d %d\\n",paused,destroyed,resumed,
            present_timeline_generation==11 && !present_timeline_snapshot.count &&
            adaptive_slots_epoch_ns==1004 && adaptive_lifecycle_calls==4 && adaptive_window==-1 && adaptive_paused==-1);
        return 0;
    }
    if (!strcmp(argv[1], "resize-current")) on_window_resized(&activity, &current_window);
    else if (!strcmp(argv[1], "resize-stale")) on_window_resized(&activity, &stale_window);
    else if (!strcmp(argv[1], "redraw-current")) on_window_redraw(&activity, &current_window);
    else if (!strcmp(argv[1], "redraw-stale")) on_window_redraw(&activity, &stale_window);
    else if (!strcmp(argv[1], "content")) on_content_rect_changed(&activity, &rect);
    else if (!strcmp(argv[1], "configuration")) on_configuration_changed(&activity);
    else if (!strcmp(argv[1], "stale-epoch")) vk_window_resized(&current_window, g_window_epoch - 1);
    else return 2;
    printf("%d %d %d %d\\n", notification_count, notified_window == &current_window,
           retry_count, retried_window == &current_window);
    return 0;
}
''')
        cls.lifecycle_probe = directory / 'lifecycle_probe'
        result = subprocess.run([*compiler, '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                                 *flags, '-pthread', '-I', str(ROOT / 'games/totk/runtime'),
                                 str(lifecycle_probe), '-o', str(cls.lifecycle_probe)],
                                capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        # Compile the production UI-thread hint transition with counted scheduler
        # notifications. This is lifecycle-policy coverage, not Android/HWC admission.
        cls.draw_source = (ROOT / 'games/totk/android/gfx/vk_draw.c').read_text()
        hint_start = cls.draw_source.index('void vk_window_transform_hint(')
        hint_end = cls.draw_source.index('/* UI lifecycle notification,', hint_start)
        selector_start = cls.draw_source.index('static int pres_slots_requested(void)')
        selector_end = cls.draw_source.index('\n}\n', selector_start) + 3
        hint_probe = directory / 'hint_probe.c'
        hint_probe.write_text('''#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdatomic.h>
#include "present_clock.h"
#include "vk_present_policy.h"
typedef struct ANativeWindow { int id; } ANativeWindow;
static ANativeWindow window={1},other={2};
static ANativeWindow *g_win=&window;
static pthread_mutex_t g_win_mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_pres_cv=PTHREAD_COND_INITIALIZER;
static uint64_t g_win_attachment_epoch=7,g_win_generation=10;
static int g_win_prerotate,g_win_ubwc,g_win_transform_hint=-1;
static int cancellations,wakes,fg_config_calls;
static char log_buffer[1024];
#define LOGV(...) ((void)snprintf(log_buffer,sizeof log_buffer,__VA_ARGS__))
static int vk_fg_adaptive(void) { fg_config_calls++; return 1; }
static void pres_slots_end_locked(void) { cancellations++; }
static void pres_adaptive_wake_locked(void) { wakes++; }
''' + cls.draw_source[selector_start:selector_end] + cls.draw_source[hint_start:hint_end] + '''
int main(int argc,char **argv) {
    if (argc==2 && !strcmp(argv[1],"selector")) {
        int selected=pres_slots_requested();
        int repeated=pres_slots_requested();
        printf("%d %d %d\\n",selected,repeated,fg_config_calls);
        return 0;
    }
    if (argc<7) return 2;
    g_win_prerotate=atoi(argv[1]); g_win_ubwc=atoi(argv[2]);
    setenv("TOTK_FG_ADAPTIVE_SLOTS",atoi(argv[3])?"1":"0",1);
    int kind=atoi(argv[4]);
    ANativeWindow *selected=kind==0?NULL:kind==2?&other:&window;
    if (kind==3) g_win=NULL;
    uint64_t epoch=strtoull(argv[5],NULL,10);
    for (int i=6;i<argc;i++) vk_window_transform_hint(selected,epoch,atoi(argv[i]));
    printf("%llu %d %d %d\\n",(unsigned long long)(g_win_generation-10),
        g_win_transform_hint,cancellations,wakes);
    return 0;
}
''')
        cls.hint_probe = directory / 'hint_probe'
        result = subprocess.run([*compiler, '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                                 *flags, '-pthread', '-I', str(ROOT / 'games/totk/android/gfx'),
                                 '-I', str(ROOT / 'games/totk/runtime'),
                                 str(hint_probe), '-o', str(cls.hint_probe)],
                                capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        cls.build_f3_trace_probes(directory, compiler, flags)

    @classmethod
    def build_f3_trace_probes(cls, directory, compiler, flags):
        # Compile the actual Android AHB pipeline/recording functions against a
        # counted Vulkan command sink. This observes shader/layout/push work,
        # not AHB import, GPU execution, foreign release, or HWC admission.
        pipeline_start = cls.draw_source.index('static int pres_pipeline(')
        pipeline_end = cls.draw_source.index('\n#endif', pipeline_start)
        trace = directory / 'ahb_trace.c'
        trace.write_text(r'''#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "vk_present_policy.h"
#include "vk_present_spv.h"
#include "vk_present_rotated_spv.h"
typedef struct { void *mem; struct { uint64_t size; } L; } tex_t;
typedef struct { int unused; } vk_tex;
typedef struct {
    int set_levels,nlevels,set_layers,nlayers,set_target,target,set_swizzle,swz[4];
} view_t;
typedef struct { VkImageView view; uint32_t w,h; } vk_post_output;
typedef struct { VkImage image; VkImageView view[2]; int foreign; } pres_ahb;
static struct { VkDevice dev; int ahb_present; uint32_t qfam; } V={.ahb_present=1};
static pres_ahb g_ahb[2];
static _Atomic int g_ahb_off,g_ahb_surface_off;
static VkSampler g_pres_sampler,g_pres_pixel_sampler;
static VkDescriptorSetLayout g_pres_dsl;
static VkPipelineLayout g_pres_layout,g_pres_rotated_layout;
static VkPipeline g_pres_pipe[2],g_pres_rotated_pipe[2],g_bound_pipe;
static int g_dyn_dirty;
static VkDescriptorSet g_bound_set[1][2];
enum { VK_PASS_END_PRESENT, VK_NUI_PRESENT_AHB };
static unsigned legacy_shaders,rotated_shaders,layouts16,layouts32,pipelines,samplers,descriptors;
static unsigned matrix_calls,extent_calls,rect_calls,push_bytes,prepare_ubwc,live_calls;
static uint32_t prepare_w,prepare_h,overlay_w,overlay_h,overlay_rotation;
static VkPipelineLayout bound_layout,push_layout;
static VkPipeline bound_pipeline;
static float pushed[8];
static VkRect2D render_area,scissor;
static VkViewport viewport;
static int trace_bad;
#define LOGV(...) ((void)0)
#define ASSERT(c) do { if (!(c)) { fprintf(stderr,"AHB trace assertion line %d: %s\n",__LINE__,#c); return 1; } } while (0)
static void ignore(const void *first,...) { (void)first; }
#define vkDestroyShaderModule ignore
#define vkDestroyPipeline ignore
#define vkDestroyPipelineLayout ignore
#define vkDestroyDescriptorSetLayout ignore
#define vkDestroySampler ignore
#define vkCmdPipelineBarrier2 ignore
#define vkUpdateDescriptorSets ignore
#define vkCmdDraw ignore
#define vkCmdEndRendering ignore
#define vk_barrier_all ignore
#define vk_native_gpu_end ignore
static int vk_native_ui_enabled(void) { return 1; }
static VkResult vkCreateSampler(VkDevice d,const VkSamplerCreateInfo *ci,const void *a,VkSampler *out) {
    (void)d;(void)ci;(void)a; *out=(VkSampler)(uintptr_t)(++samplers); return VK_SUCCESS;
}
static VkResult vkCreateDescriptorSetLayout(VkDevice d,const VkDescriptorSetLayoutCreateInfo *ci,
        const void *a,VkDescriptorSetLayout *out) {
    (void)d;(void)ci;(void)a; *out=(VkDescriptorSetLayout)(uintptr_t)(++descriptors); return VK_SUCCESS;
}
static VkResult vkCreatePipelineLayout(VkDevice d,const VkPipelineLayoutCreateInfo *ci,
        const void *a,VkPipelineLayout *out) {
    (void)d;(void)a;
    unsigned bytes=ci->pPushConstantRanges[0].size;
    trace_bad|=ci->pushConstantRangeCount!=1 || ci->setLayoutCount!=1 ||
        ci->pPushConstantRanges[0].stageFlags!=VK_SHADER_STAGE_VERTEX_BIT ||
        ci->pPushConstantRanges[0].offset!=0 || ci->pSetLayouts[0]!=g_pres_dsl;
    if (bytes==16) layouts16++;
    else if (bytes==32) layouts32++;
    else trace_bad=1;
    *out=(VkPipelineLayout)(uintptr_t)bytes; return VK_SUCCESS;
}
static VkResult vkCreateShaderModule(VkDevice d,const VkShaderModuleCreateInfo *ci,
        const void *a,VkShaderModule *out) {
    (void)d;(void)a;
    if (ci->pCode==pres_vert_spv) {
        legacy_shaders++; trace_bad|=ci->codeSize!=sizeof pres_vert_spv;
    } else if (ci->pCode==pres_rotated_vert_spv) {
        rotated_shaders++; trace_bad|=ci->codeSize!=sizeof pres_rotated_vert_spv;
    } else trace_bad|=ci->pCode!=pres_frag_spv || ci->codeSize!=sizeof pres_frag_spv;
    *out=(VkShaderModule)(uintptr_t)(ci->pCode==pres_frag_spv?2:1); return VK_SUCCESS;
}
static VkResult vk_pcache_graphics(const VkGraphicsPipelineCreateInfo *ci,VkPipeline *out) {
    *out=(VkPipeline)(uintptr_t)((uintptr_t)ci->layout*10+(pipelines++&1)); return VK_SUCCESS;
}
static VkFormat vk_tex_format(vk_tex *x) { (void)x; return VK_FORMAT_R8G8B8A8_UNORM; }
static int vk_tex_is3d(vk_tex *x) { (void)x; return 0; }
static VkImageView vk_tex_view(vk_tex *x,const tex_t *t,const view_t *v,int layer) {
    (void)x;(void)t;(void)v;(void)layer; return (VkImageView)(uintptr_t)3;
}
static uint32_t vk_tex_width(vk_tex *x,int level) { (void)x;(void)level; return 64; }
static uint32_t vk_tex_height(vk_tex *x,int level) { (void)x;(void)level; return 32; }
static int pres_ahb_prepare(pres_ahb *a,uint32_t w,uint32_t h,int ubwc) {
    (void)a; prepare_w=w;prepare_h=h;prepare_ubwc=(unsigned)ubwc; return 1;
}
static int vk_lag_uniform_private(void) { live_calls++; return 0; }
static void vk_ub_touch(uint64_t addr,uint64_t size) { (void)addr;(void)size; live_calls++; }
static void vk_ub_flush(void) { live_calls++; }
static void vk_end_rendering_at(int reason,int detail) { (void)reason;(void)detail;live_calls++; }
static VkCommandBuffer vk_cb(void) { live_calls++; return (VkCommandBuffer)(uintptr_t)4; }
static int vk_native_gpu_begin(VkCommandBuffer cb,int scope) { (void)cb;(void)scope;live_calls++;return 0; }
static VkDescriptorSet vk_alloc_set(VkDescriptorSetLayout layout) {
    (void)layout; return (VkDescriptorSet)(uintptr_t)5;
}
static void vkCmdBeginRendering(VkCommandBuffer cb,const VkRenderingInfo *ri) {
    (void)cb; render_area=ri->renderArea;
}
static void vkCmdBindPipeline(VkCommandBuffer cb,VkPipelineBindPoint point,VkPipeline pipeline) {
    (void)cb;(void)point;bound_pipeline=pipeline;
}
static void vkCmdBindDescriptorSets(VkCommandBuffer cb,VkPipelineBindPoint point,VkPipelineLayout layout,
        uint32_t first,uint32_t count,const VkDescriptorSet *sets,uint32_t dynamic,const uint32_t *offsets) {
    (void)cb;(void)point;(void)first;(void)count;(void)sets;(void)dynamic;(void)offsets;bound_layout=layout;
}
static void vkCmdPushConstants(VkCommandBuffer cb,VkPipelineLayout layout,VkShaderStageFlags stages,
        uint32_t offset,uint32_t size,const void *data) {
    (void)cb; trace_bad|=stages!=VK_SHADER_STAGE_VERTEX_BIT || offset!=0 || size>sizeof pushed;
    push_layout=layout; push_bytes=size; if (size<=sizeof pushed) memcpy(pushed,data,size);
}
static void vkCmdSetViewport(VkCommandBuffer cb,uint32_t first,uint32_t count,const VkViewport *vp) {
    (void)cb;(void)first;(void)count;viewport=*vp;
}
static void vkCmdSetScissor(VkCommandBuffer cb,uint32_t first,uint32_t count,const VkRect2D *sc) {
    (void)cb;(void)first;(void)count;scissor=*sc;
}
static void vk_overlay_record(VkCommandBuffer cb,VkImage image,VkImageLayout layout,
        uint32_t w,uint32_t h,unsigned slot,unsigned rotation) {
    (void)cb;(void)image;(void)layout;(void)slot;overlay_w=w;overlay_h=h;overlay_rotation=rotation;
}
static void counted_matrix(unsigned rotation,float matrix[4]) {
    matrix_calls++; vk_present_rotation_matrix(rotation,matrix);
}
static vk_present_extent counted_extent(uint32_t w,uint32_t h,unsigned rotation) {
    extent_calls++; return vk_present_rotate_extent(w,h,rotation);
}
static vk_present_rect counted_rect(vk_present_rect r,uint32_t w,uint32_t h,unsigned rotation) {
    rect_calls++; return vk_present_rotate_rect(r,w,h,rotation);
}
#define vk_present_rotation_matrix counted_matrix
#define vk_present_rotate_extent counted_extent
#define vk_present_rotate_rect counted_rect
''' + cls.draw_source[pipeline_start:pipeline_end] + r'''
static int record(unsigned rotation,int srgb,int ubwc) {
    tex_t texture={0}; vk_tex source={0};
    int ok=pres_ahb_draw(0,&texture,&source,srgb,128,64,4,2,32,16,12,4,96,48,
        NULL,rotation,ubwc,(VkCommandBuffer)(uintptr_t)7);
    ASSERT(ok && !trace_bad && !live_calls && !g_ahb[0].foreign);
    ASSERT(push_bytes==(rotation?32:16) && push_layout==bound_layout &&
        bound_layout==(rotation?g_pres_rotated_layout:g_pres_layout) &&
        bound_pipeline==(rotation?g_pres_rotated_pipe[srgb]:g_pres_pipe[srgb]));
    ASSERT(pushed[0]==.0625f && pushed[1]==.0625f && pushed[2]==.5f && pushed[3]==.5f);
    ASSERT(prepare_w==(rotation&1?64u:128u) && prepare_h==(rotation&1?128u:64u) &&
        render_area.extent.width==prepare_w && render_area.extent.height==prepare_h &&
        prepare_ubwc==(unsigned)ubwc && overlay_w==128 && overlay_h==64 && overlay_rotation==rotation);
    static const int rectangles[4][4]={{12,4,96,48},{12,12,48,96},{20,12,96,48},{4,20,48,96}};
    ASSERT(viewport.x==rectangles[rotation][0] && viewport.y==rectangles[rotation][1] &&
        viewport.width==rectangles[rotation][2] && viewport.height==rectangles[rotation][3] &&
        scissor.offset.x==viewport.x && scissor.offset.y==viewport.y &&
        scissor.extent.width==viewport.width && scissor.extent.height==viewport.height);
    if (rotation) {
        static const float matrices[3][4]={{0,-1,1,0},{-1,0,0,-1},{0,1,-1,0}};
        ASSERT(!memcmp(pushed+4,matrices[rotation-1],sizeof matrices[0]));
    }
    return 0;
}
int main(int argc,char **argv) {
    ASSERT(argc==2);
    int ubwc=!strcmp(argv[1],"ubwc");
    ASSERT(!record(0,0,ubwc) && !record(0,1,ubwc));
    ASSERT(legacy_shaders==1 && !rotated_shaders && layouts16==1 && !layouts32 &&
        pipelines==2 && samplers==2 && descriptors==1 && !matrix_calls && !extent_calls && !rect_calls &&
        !g_pres_rotated_layout && !g_pres_rotated_pipe[0] && !g_pres_rotated_pipe[1]);
    if (!strcmp(argv[1],"rotate")) {
        ASSERT(!record(1,0,0) && !record(2,1,0) && !record(3,0,0) && !record(0,1,0));
        ASSERT(legacy_shaders==1 && rotated_shaders==1 && layouts16==1 && layouts32==1 &&
            pipelines==4 && samplers==2 && descriptors==1 &&
            matrix_calls==3 && extent_calls==3 && rect_calls==3 && push_bytes==16);
    }
    puts("ok"); return 0;
}
''')
        cls.ahb_trace = directory / 'ahb_trace'
        result = subprocess.run([*compiler, '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                                 '-Wno-missing-field-initializers', *flags,
                                 '-I', str(ROOT / 'games/totk/android/gfx'),
                                 str(trace), '-o', str(cls.ahb_trace)], capture_output=True, text=True)
        if result.returncode:
            if ('vulkan/vulkan.h' in result.stderr and
                    ('No such file or directory' in result.stderr or 'file not found' in result.stderr)):
                cls.ahb_trace = None
            else:
                raise AssertionError(result.stdout + result.stderr)
        recorder = (ROOT / 'games/totk/android/gfx/nvn_vk.c').read_text()
        hook = ('if (!g_cur && __builtin_expect(atomic_load_explicit('
                '&vk_fg_slot_boundary_active,memory_order_relaxed),0)) {')
        if recorder.count(hook) != 2 or recorder.count('vk_fg_slot_boundary();') != 3:
            raise AssertionError('Two legacy recorder gates plus the separately selected chunk hook are required')
        def recorder_hook(function, end_marker):
            begin = recorder.index(function)
            begin = recorder.index('    int slot_trace=', begin)
            end = recorder.index(end_marker, begin)
            # Keep actual admission/trace code; lock/init are separate caller
            # prerequisites, covered by the recorder-order integration test.
            return recorder[begin:end].replace('    vk_rec_lock(); mtl_init();\n', '')
        begin_hook = recorder_hook('void mtl_sub_begin(void)', '    if (vk_xlate_stats_on())')
        commit_hook = recorder_hook('uint64_t mtl_sub_commit(void)', '    if (ub_private_on())')
        selector_start = cls.draw_source.index('static int pres_slots_requested(void)')
        selector_end = cls.draw_source.index('\n}\n', selector_start) + 3
        selector = cls.draw_source[selector_start:selector_end].replace(
            'pres_slots_requested(void)', 'production_pres_slots_requested(void)', 1)
        prepare = cls.draw_source.index('        g_slot_prepared_count++;')
        drain = cls.draw_source.index('        g_slot_prepared_count--;')
        prepare_gate = cls.draw_source[prepare:cls.draw_source.index('        pthread_mutex_unlock', prepare)]
        drain_gate = cls.draw_source[drain:cls.draw_source.index('        pthread_mutex_unlock', drain)]
        boundary = directory / 'boundary_trace.c'
        boundary.write_text(r'''#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "present_clock.h"
static _Atomic int vk_fg_slot_boundary_active;
static unsigned g_slot_prepared_count,boundaries,selectors,fg_config_calls,cancellations;
static void *g_cur;
static int adaptive=1,job_trace;
static unsigned trace_getters,trace_clocks,trace_logs;
static int g_in_sub;
static uint64_t g_gpu_guest_ns,g_gpu_origin_ticks,g_gpu_guest_frame;
static int vk_fg_job_trace_enabled(void) { trace_getters++; return job_trace; }
static uint64_t host_now_ns(void) { return ++trace_clocks; }
static uint64_t slot_submit(int boundary,int reason) { (void)boundary;(void)reason;return 77; }
static void vk_rec_service_run(int boundary) { (void)boundary; }
static void trace_log(const char *format,...) { (void)format;trace_logs++; }
#define LOGV(...) trace_log(__VA_ARGS__)
#define VK_PASS_END_SUBMISSION 1
static int vk_fg_adaptive(void) { fg_config_calls++; return adaptive; }
''' + selector + r'''
static void vk_fg_slot_boundary(void) {
    boundaries++; selectors++;
    if (!production_pres_slots_requested()) cancellations++;
}
static void begin_hook(void) {
''' + begin_hook + r'''
}
static void commit_hook(void) {
''' + commit_hook + r'''
}
static void prepare(void) {
''' + prepare_gate + r'''
}
static void drain(void) {
''' + drain_gate + r'''
}
int main(void) {
    const char *trace=getenv("TOTK_FRAMEGEN_JOB_TRACE");
    job_trace=trace && !strcmp(trace,"1");
    for (unsigned i=0;i<1000;i++) { begin_hook();commit_hook(); }
    printf("%u %u %u\n",boundaries,selectors,fg_config_calls);
    if (trace_getters || trace_clocks || trace_logs) return 1;
    if (!production_pres_slots_requested()) return 0;
    prepare();prepare();begin_hook();commit_hook();
    g_cur=(void *)(uintptr_t)1;begin_hook();commit_hook();g_cur=NULL;
    adaptive=0;begin_hook();commit_hook();
    drain();begin_hook();drain();commit_hook();
    printf("%u %u %u %u %u %d\n",boundaries,selectors,fg_config_calls,cancellations,
        g_slot_prepared_count,atomic_load(&vk_fg_slot_boundary_active));
    if (job_trace ? (!trace_getters || !trace_clocks || !trace_logs) :
            (trace_clocks || trace_logs)) return 1;
    return 0;
}
''')
        cls.boundary_trace = directory / 'boundary_trace'
        result = subprocess.run([*compiler, '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                                 *flags, '-I', str(ROOT / 'games/totk/runtime'),
                                 str(boundary), '-o', str(cls.boundary_trace)], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def test_ahb_identity_keeps_original_shader_layout_and_push_work(self):
        if self.ahb_trace is None:
            self.skipTest('Vulkan development headers are required for the AHB command-trace fixture')
        for path in ('identity', 'ubwc', 'rotate'):
            with self.subTest(path=path):
                result = subprocess.run([str(self.ahb_trace), path],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(result.stdout, 'ok\n')

    def test_recorder_hooks_skip_selector_until_admitted_template_and_drain_after_disable(self):
        for value in (None, '0', '1'):
            for trace in ('0', '1'):
                with self.subTest(value=value, trace=trace):
                    env = os.environ.copy()
                    env.pop('TOTK_FG_ADAPTIVE_SLOTS', None)
                    env['TOTK_FRAMEGEN_JOB_TRACE'] = trace
                    if value is not None:
                        env['TOTK_FG_ADAPTIVE_SLOTS'] = value
                    result = subprocess.run([str(self.boundary_trace)], env=env,
                                            capture_output=True, text=True, check=True)
                    self.assertEqual(result.stdout.splitlines(),
                                     ['0 0 0', '5 5 6 3 0 0'] if value == '1' else ['0 0 0'])

    def hint_lifecycle(self, prerotate, ubwc, slots, window, epoch, *hints):
        result = subprocess.run([str(self.hint_probe),
                                 *map(str, (prerotate, ubwc, slots, window, epoch, *hints))],
                                capture_output=True, text=True, check=True)
        return tuple(map(int, result.stdout.split()))

    def test_hint_changes_invalidate_and_wake_without_duplicate_notifications(self):
        for prerotate, ubwc in ((1, 0), (0, 1), (1, 1)):
            for slots in (0, 1):
                with self.subTest(prerotate=prerotate, ubwc=ubwc, slots=slots):
                    self.assertEqual(self.hint_lifecycle(prerotate, ubwc, slots, 1, 7, 4, 4, 3, 0, -1),
                                     (4, -1, 4 * slots, 4))
                    # 180-degree and unsupported/mirror changes still retire the
                    # old generation, even when the logical extent is unchanged.
                    self.assertEqual(self.hint_lifecycle(prerotate, ubwc, slots, 1, 7, 3, 1, 7),
                                     (3, 7, 3 * slots, 3))

    def test_hint_default_off_stale_epoch_wrong_window_and_detach_are_inert(self):
        self.assertEqual(self.hint_lifecycle(0, 0, 1, 1, 7, 4, 3), (0, -1, 0, 0))
        for window, epoch in ((0, 7), (2, 7), (3, 7), (1, 6), (1, 8)):
            with self.subTest(window=window, epoch=epoch):
                self.assertEqual(self.hint_lifecycle(1, 1, 1, window, epoch, 4, 3), (0, -1, 0, 0))

    def test_slot_selector_default_off_does_not_initialize_framegen(self):
        for value in (None, '', '0', '1', 'bad'):
            with self.subTest(value=value):
                env = os.environ.copy()
                env.pop('TOTK_FG_ADAPTIVE_SLOTS', None)
                if value is not None:
                    env['TOTK_FG_ADAPTIVE_SLOTS'] = value
                result = subprocess.run([str(self.hint_probe), 'selector'], env=env,
                                        capture_output=True, text=True, check=True)
                self.assertEqual(tuple(map(int, result.stdout.split())),
                                 (0, 0, 0) if value in (None, '0') else (1, 1, 2))

    def test_hint_bridge_tracks_attached_root_and_native_attachment_epoch(self):
        java = (ROOT / 'games/totk/android/java/com/aure/totk/TotkActivity.java').read_text()
        jni = (ROOT / 'games/totk/runtime/companion_jni.c').read_text()
        self.assertIn('decor.isAttachedToWindow() ? decor.getRootSurfaceControl() : null', java)
        self.assertIn('presentRoot == root && presentEpoch == epoch', java)
        self.assertIn('nativePresentTransformHint(epoch, root.getBufferTransformHint())', java)
        self.assertIn('updatePresentPrerotate(0);', java)
        self.assertIn('{"nativePresentTransformHint", "(JI)V", (void *)native_present_transform_hint}', jni)
        self.assertIn('runtime_android_present_transform_hint((uint64_t)epoch,hint);', jni)
        self.assertIn('vk_window_transform_hint(g_window,epoch,hint);', self.source)
        self.assertIn('present_transform_update(a,g_window_epoch);', self.source)
        clear = self.source[self.source.index('static void window_clear('):
                            self.source.index('static void surface_parent_retry(')]
        self.assertLess(clear.index('present_transform_update(a,0);'), clear.index('vk_window_set(NULL'))

    def test_private_ahb_commands_keep_real_snapshot_and_publish_ownership_only_after_submit(self):
        ahb = self.draw_source[self.draw_source.index('static int pres_ahb_draw('):
                               self.draw_source.index('uint64_t vk_window_set(')]
        self.assertIn('unsigned rotation, int ubwc, VkCommandBuffer prepared_cb', ahb)
        self.assertIn('pres_ahb_prepare(a, native.w, native.h, ubwc)', ahb)
        self.assertIn('.renderArea = {{0, 0}, {native.w, native.h}}', ahb)
        self.assertIn('prepared_cb ? prepared_cb : vk_cb()', ahb)
        self.assertIn('prepared_cb ? -1 : vk_native_gpu_begin', ahb)
        self.assertIn('if (!prepared_cb) vk_native_gpu_end', ahb)
        live = ahb[ahb.index('if (!prepared_cb) {'):ahb.index('VkCommandBuffer cb =')]
        self.assertIn('vk_ub_touch', live)
        self.assertIn('vk_ub_flush', live)
        self.assertIn('vk_end_rendering_at', live)
        self.assertNotRegex(ahb, r'a->foreign\s*=')
        generated = self.draw_source[self.draw_source.index('static void present_generated('):
                                     self.draw_source.index('void vk_fg_slot_boundary(')]
        record = generated.index('if (pres_ahb_draw(')
        for snapshot in ('b->rotation=real->rotation;', 'b->ubwc=real->ubwc;'):
            self.assertLess(generated.index(snapshot), record)
        self.assertIn('&post,b->rotation,b->ubwc,prepare_slots?cb:VK_NULL_HANDLE', generated)
        self.assertLess(generated.index('vk_fg_generation_prepare_finish(prepared)'),
                        generated.index('g_slot_prepared[i]='))
        self.assertLess(generated.index('b->fence=vk_submit_now();'),
                        generated.index('g_ahb[i].foreign=1;'))
        boundary = self.draw_source[self.draw_source.index('void vk_fg_slot_boundary('):
                                    self.draw_source.index('static void present_window(')]
        self.assertLess(boundary.index('ticket=vk_fg_generation_seal('),
                        boundary.index('g_ahb[job.output_index].foreign=1;'))

    def lifecycle(self, event):
        result = subprocess.run([str(self.lifecycle_probe), event],
                                capture_output=True, text=True, check=True)
        return tuple(map(int, result.stdout.split()))

    def test_window_resize_and_redraw_notify_current_window_only(self):
        for event in ('resize-current', 'redraw-current'):
            with self.subTest(event=event):
                self.assertEqual(self.lifecycle(event), (1, 1, 1, 1))
        for event in ('resize-stale', 'redraw-stale'):
            with self.subTest(event=event):
                self.assertEqual(self.lifecycle(event), (0, 0, 0, 0))

    def test_stale_attachment_epoch_does_not_notify_even_for_current_pointer(self):
        self.assertEqual(self.lifecycle('stale-epoch'), (0, 0, 0, 0))

    def test_content_rect_and_configuration_readiness_notify_current_window(self):
        for event in ('content', 'configuration'):
            with self.subTest(event=event):
                self.assertEqual(self.lifecycle(event), (1, 1, 1, 1))

    def test_timeline_lifecycle_invalidates_real_snapshot_and_preserves_other_facet(self):
        self.assertEqual(self.lifecycle('timeline-lifecycle'), (1, 1, 1, 1))

    def policy(self, handheld, ui, output=None, width=None, scene=None, aa=None):
        env = os.environ.copy()
        for key, value in (('TOTK_NATIVE_UI', ui), ('TOTK_OUTPUT_RES', output), ('TOTK_APP_WIDTH', width), ('TOTK_SCENE_SCALE', scene), ('TOTK_SCENE_AA', aa)):
            env.pop(key, None)
            if value is not None:
                env[key] = value
        result = subprocess.run([str(self.probe), str(int(handheld))], env=env,
                                capture_output=True, text=True, check=True)
        return result.stdout.splitlines(), result.stderr

    def test_docked_missing_or_empty_width_uses_guest_for_each_native_ui_state(self):
        for ui in (None, '0', '1'):
            for width in (None, ''):
                for output in (None, '', 'auto', '0', '2560x1440'):
                    with self.subTest(ui=ui, width=width, output=output):
                        values, log = self.policy(False, ui, output, width)
                        expected_output = ('0' if ui == '1' and output in (None, '', 'auto')
                                           else output if output is not None else '<unset>')
                        self.assertEqual(values, ['0' if ui == '1' else ui or '<unset>',
                                                  expected_output, 'guest'])
                        if ui == '1':
                            self.assertEqual(log.count('requested=1 effective=0'), 1)
                            self.assertIn('totk.env unchanged', log)
                        else:
                            self.assertEqual(log, '')

    def test_explicit_nonempty_width_is_authoritative(self):
        for ui in (None, '0', '1'):
            for width in ('960', '1920', 'guest', '0'):
                with self.subTest(ui=ui, width=width):
                    values, _ = self.policy(False, ui, '1920x1080', width)
                    self.assertEqual(values, ['0' if ui == '1' else ui or '<unset>',
                                              '1920x1080', width])

    def test_handheld_width_and_output_are_unchanged(self):
        for ui in (None, '0', '1'):
            for output in (None, '', 'auto', '0', '2560x1440'):
                for width in (None, '', '960'):
                    with self.subTest(ui=ui, output=output, width=width):
                        values, log = self.policy(True, ui, output, width)
                        self.assertEqual(values, [ui if ui is not None else '<unset>',
                                                  output if output is not None else '<unset>',
                                                  width if width is not None else '<unset>'])
                        self.assertEqual(log, '')

    def test_scene_scale_opt_in_uses_native_composition_in_both_modes(self):
        for handheld in (True, False):
            for ui in (None, '0', '1'):
                for scene in ('sgsr', 'fsr'):
                    with self.subTest(handheld=handheld, ui=ui, scene=scene):
                        values, log = self.policy(handheld, ui, '2560x1440', 'guest', scene)
                        self.assertEqual(values, ['1', '0', '1920'])
                        self.assertIn('native composition effective=1', log)
                        self.assertIn('totk.env unchanged', log)

    def test_scene_aa_opt_in_and_invalid_values(self):
        for handheld in (True, False):
            for scene in (None, 'off', 'sgsr', 'fsr'):
                values, log = self.policy(handheld, '0', '2560x1440', 'guest', scene, 'smaa')
                self.assertEqual(values, ['1', '0', '1920'])
                self.assertIn('AA=smaa', log)
            for aa in (None, '', '0', 'off', 'invalid'):
                values, log = self.policy(handheld, '0', '0', 'guest', None, aa)
                self.assertEqual(values, ['0', '0', 'guest'])
                self.assertEqual(log, '')

    def test_policy_runs_after_mode_default_before_output_consumers(self):
        mode = self.source.index('env_default("TOTK_OPERATION_MODE"')
        policy = self.source.index('    android_native_ui_mode_policy();', mode)
        self.assertLess(mode, policy)
        self.assertLess(policy, self.source.index('env_default("TOTK_ROMFS_MMAP"', mode))

    def test_guest_extent_smaller_and_non_sixteen_by_nine_windows(self):
        cases = ((1920,1080,1920,1080,(0,0,1920,1080)),
                 (1920,1080,1280,720,(0,0,1280,720)),
                 (1920,1080,1280,800,(0,40,1280,720)),
                 (1920,1080,2400,1080,(240,0,1920,1080)),
                 (1920,1080,1000,700,(0,69,1000,562)),
                 (1920,1080,2560,1440,(320,180,1920,1080)),
                 (2560,1440,1920,1080,(0,0,1920,1080)))
        for gw, gh, ww, wh, expected in cases:
            with self.subTest(canvas=(gw,gh), window=(ww,wh)):
                result = subprocess.run([str(self.probe), 'extent', *map(str,(gw,gh,ww,wh))],
                                        capture_output=True, text=True, check=True)
                self.assertEqual(tuple(map(int, result.stdout.split())), expected)

    def test_unready_resize_detach_and_recreation_have_no_cached_extent(self):
        for ww, wh, expected in ((0,0,(0,0,0,0)), (1920,0,(0,0,0,0)),
                                 (1920,1080,(0,0,1920,1080)), (1280,800,(0,40,1280,720)),
                                 (0,0,(0,0,0,0)), (2400,1080,(240,0,1920,1080))):
            result = subprocess.run([str(self.probe), 'extent', '1920', '1080', str(ww), str(wh)],
                                    capture_output=True, text=True, check=True)
            self.assertEqual(tuple(map(int, result.stdout.split())), expected)

    def rotation(self, hint, width, height, rect):
        result = subprocess.run([str(self.probe), 'rotation',
                                 *map(str, (hint, width, height, *rect))],
                                capture_output=True, text=True, check=True)
        return tuple(map(int, result.stdout.split()))

    def test_prerotate_all_quarter_turns_and_inverse_surface_transform(self):
        rect = (10, 20, 30, 40)
        cases = ((0, 0, 0, (1920, 1080), (10, 20, 30, 40), (1, 0, 0, 1)),
                 (4, 1, 7, (1080, 1920), (1020, 10, 40, 30), (0, -1, 1, 0)),
                 (3, 2, 3, (1920, 1080), (1880, 1020, 30, 40), (-1, 0, 0, -1)),
                 (7, 3, 4, (1080, 1920), (20, 1880, 40, 30), (0, 1, -1, 0)))
        for hint, turns, inverse, extent, rotated, matrix in cases:
            with self.subTest(hint=hint):
                self.assertEqual(self.rotation(hint, 1920, 1080, rect),
                                 (turns, inverse, *extent, *rotated, *rect, *matrix))

    def test_prerotate_rectangles_round_trip_without_pixel_loss(self):
        for width, height in ((1920, 1080), (1080, 1920), (1280, 800), (7, 5)):
            for rect in ((0, 0, width, height), (0, 0, 1, 1),
                         (width - 1, height - 1, 1, 1), (1, 2, width - 2, height - 3)):
                for hint in (0, 4, 3, 7):
                    with self.subTest(extent=(width, height), rect=rect, hint=hint):
                        value = self.rotation(hint, width, height, rect)
                        nw, nh, x, y, w, h = value[2:8]
                        self.assertEqual(value[8:12], rect)
                        self.assertGreaterEqual(x, 0)
                        self.assertGreaterEqual(y, 0)
                        self.assertLessEqual(x + w, nw)
                        self.assertLessEqual(y + h, nh)

    def test_prerotate_rejects_reflections_unavailable_and_unknown_flags(self):
        for hint in (-1, 1, 2, 5, 6, 8, 12, 255):
            with self.subTest(hint=hint):
                self.assertEqual(self.rotation(hint, 1920, 1080, (0, 0, 1920, 1080)), (-1,))

    def test_compressed_ahb_requires_exact_post_bind_size_and_supported_memory_type(self):
        cases = ((8192, 8192, 1, 0, True), (8192, 8192, 0x80000000, 31, True),
                 (8192, 4096, 1, 0, False), (8192, 12288, 1, 0, False),
                 (0, 0, 1, 0, False), (8192, 8192, 0, 0, False),
                 (8192, 8192, 2, 0, False), (8192, 8192, 1, 32, False))
        for allocation, required, types, index, expected in cases:
            with self.subTest(allocation=allocation, required=required, types=types, index=index):
                result = subprocess.run([str(self.probe), 'ahb-layout',
                                         *map(str, (allocation, required, types, index))],
                                        capture_output=True, text=True, check=True)
                self.assertEqual(bool(int(result.stdout)), expected)

    def test_compressed_ahb_driver_allowlist_matches_complete_individual_builds_only(self):
        known = ('Mesa 27.0.0-devel (git-u1p4c1b809)',)
        unknown = ('', 'Mesa 27.0.0-devel', 'Mesa 27.0.0-devel (git-u1b6356089)',
                   'Mesa 27.0.0-devel (git-u1b42b0c8a)', 'Mesa 27.0.0-devel (git-u1b5356089)',
                   'Mesa 27.0.0-devel (git-u1b42dc14e)', 'Mesa 27.0.0-devel (git-u1b5d24d8f)',
                   'Mesa 27.0.1 (git-u1p4c1b809)',
                   *(driver + suffix for driver in known for suffix in (' ', '-new')),
                   *(driver[:-1] for driver in known))
        for driver in (*known, *unknown):
            with self.subTest(driver=driver):
                result = subprocess.run([str(self.probe), 'ubwc-driver', driver],
                                        capture_output=True, text=True, check=True)
                self.assertEqual(bool(int(result.stdout)), driver in known)


if __name__ == '__main__':
    unittest.main()
