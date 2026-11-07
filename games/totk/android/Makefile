# Linux / Android build of the native runtime (headless: no window, null GPU backend).
#
#   make -C android TARGET=android NDK=/path/to/android-ndk  VERSION=v143  OUT=<dir>  OPUS=<opus prefix>
#   make -C android TARGET=linux   CC=aarch64-linux-gnu-gcc  ...            (arm64 Linux, e.g. under qemu-aarch64)
#
# The runtime builds from this repository alone: what it needs from the player's game files (module tables, RomFS and
# shared-font hashes) is read at start from the game data folder (runtime/game_data.h, TOTK_GAME_DATA), which the app's
# setup writes on the device and build_android.py writes on a PC. VERSION=v143 also compiles in the pinned 1.4.3 hashes
# (totk_pins.h from assets/expected-v1.4.3.json): the runtime then refuses game data other than the pinned one.
# OPUS / VPX are install prefixes (include/, lib/) of libopus and, optionally, libvpx built for the same target;
# without libvpx the game's own movie decoder runs (cutscenes abort, as on iOS).
# The on-device setup library (setup/setup.mk: libtotk_setup.so, libhactool.so; make setup-android) is included below.
# Sources are reached through pattern rules, so make may count them as intermediate files and delete them after the
# build (it did once, under `make -o`). GNU make >= 4.4: no file is ever intermediate.
.NOTINTERMEDIATE:
TOTK := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
TARGET ?= android
VERSION ?= v0
OUT ?= $(TOTK)/build/android-$(VERSION)
PYTHON ?= python3
OPUS ?=
VPX ?=
API ?= 33
# Dev is the default; release builds pass DEV_BUILD=0.
DEV_BUILD ?= 1
ifneq ($(DEV_BUILD),0)
  ifneq ($(DEV_BUILD),1)
    $(error DEV_BUILD must be 0 or 1)
  endif
endif
DEV_BUILD_STAMP := $(OUT)/.dev-build
# GPU backend: vk (android/gfx/nvn_vk.c, Vulkan) or null (android/gfx/nvn_null.c, no GPU)
GPU ?= vk
# APP=1: the runtime as libtotk.so for the Android app (NativeActivity, runtime/frontend_android.c) instead of the
# totk-native executable; android/apk/build_apk.py packs it into an APK.
APP ?= 0
# Keep leaf frames too for simpleperf's frame-pointer unwinder (DEBUGGING.md).
PROFILE_FP ?= 0
ifeq ($(PROFILE_FP),1)
  PROFILE_FLAGS := -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer
endif
# Profile-guided optimization. PGO=gen: instrumented build; the app writes TOTK_PGO_FILE itself (runtime/pgo_dump.c).
# PGO=<path>.profdata: optimize with that merged profile (llvm-profdata merge). Empty: no PGO.
PGO ?=
ifeq ($(PGO),gen)
  PGO_CFLAGS := -fprofile-instr-generate -DTOTK_PGO_GEN
  PGO_LDFLAGS := -fprofile-instr-generate
  PGO_ID := gen
else ifneq ($(PGO),)
  ifeq ($(wildcard $(PGO)),)
    $(error PGO profile $(PGO) does not exist)
  endif
  PGO_CFLAGS := -fprofile-instr-use=$(abspath $(PGO)) -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date
  PGO_ID := use-$(shell sha256sum $(PGO) | cut -c1-16)
endif
# LTO=thin: ThinLTO across the runtime and NVN layer (compile and link). Combines with PGO.
LTO ?=
ifeq ($(LTO),thin)
  PGO_CFLAGS += -flto=thin
  PGO_LDFLAGS += -flto=thin -Wl,--thinlto-jobs=4
  PGO_ID := $(PGO_ID)-lto
else ifneq ($(LTO),)
  $(error LTO must be empty or thin)
endif

ifeq ($(TARGET),android)
  ifeq ($(NDK),)
    $(error set NDK to the Android NDK directory (r26 or newer))
  endif
  HOST_TAG := $(shell uname -s | tr A-Z a-z)-x86_64
  CC := $(NDK)/toolchains/llvm/prebuilt/$(HOST_TAG)/bin/clang --target=aarch64-linux-android$(API)
  PLATFORM_LIBS := -laaudio -llog -ldl -lm
else
  CC ?= aarch64-linux-gnu-gcc
  PLATFORM_LIBS := -lpthread -ldl -lm
endif

ifeq ($(VERSION),v0)
  VDEF :=
  PATCHES := runtime/game_patches.c
else ifeq ($(VERSION),v143)
  VDEF := -DTOTK_GAME_V143=1
  PATCHES := runtime/game_patches_v143.c
else
  VDEF := -DTOTK_GAME_$(shell echo $(VERSION) | tr a-z A-Z)=1
  PATCHES := runtime/game_patches.c
endif

RUNTIME_C := runtime/loader.c runtime/game_data.c $(PATCHES) runtime/world_survey.c runtime/game_slot.c runtime/services.c runtime/romfs.c \
  runtime/threads.c runtime/frame_timeline.c runtime/frame_fence.c runtime/hle_stack.c runtime/gfx_bridge.c runtime/account.c runtime/input.c runtime/hid_feedback.c \
  runtime/motion_fusion.c runtime/autoload.c runtime/gfx_probe.c runtime/memory_probe.c runtime/shared_font.c runtime/ipc.c \
  runtime/audio_renderer.c runtime/audio_mixer.c runtime/audio_reverb.c runtime/audio_effects.c runtime/fs_host.c \
  runtime/opus_codec.c runtime/movie_host.c runtime/time_host.c runtime/applet_host.c runtime/swkbd_host.c \
  runtime/host_tp.c runtime/host_os.c runtime/pgo_dump.c runtime/android_perf.c runtime/android_thermal.c \
  $(if $(filter 1,$(APP)),runtime/frontend_android.c runtime/feedback_android.c runtime/companion_snapshot.c runtime/companion_actions.c runtime/companion_jni.c,runtime/frontend_headless.c)
RUNTIME_S := runtime/gate.S runtime/hle_stack.S runtime/thread_enter.S
# The NVN layer (portable C), without the Mac's text overlay (CoreGraphics) and system-load reader (sysctl).
ifeq ($(GPU),vk)
  BACKEND_C := $(TOTK)/android/gfx/nvn_vk.c $(TOTK)/android/gfx/vk_load.c $(TOTK)/android/gfx/vk_fmt.c $(TOTK)/android/gfx/vk_res.c $(TOTK)/android/gfx/vk_upload_pool.c \
    $(TOTK)/android/gfx/vk_shader.c $(TOTK)/android/gfx/vk_compute.c $(TOTK)/android/gfx/vk_pcache.c $(TOTK)/android/gfx/vk_seed_io.c $(TOTK)/android/gfx/vk_shader_xlate.c $(TOTK)/android/gfx/vk_draw.c $(TOTK)/android/gfx/vk_canvas.c $(TOTK)/android/gfx/vk_post.c $(TOTK)/android/gfx/vk_framegen.c $(TOTK)/android/gfx/vk_record.c $(TOTK)/android/gfx/vk_gpuprof.c $(TOTK)/android/gfx/vk_overlay.c \
    $(TOTK)/runtime/fps_hybrid_runtime.c $(TOTK)/runtime/fps_adaptive_runtime.c
  BACKEND_C += $(TOTK)/android/gfx/vk_framegen_qcom.c $(TOTK)/android/gfx/vk_framegen_lsfg.c
  # Runtime framegen policies need GPU_VK (non-Vulkan callers use header no-ops).
  BACKEND_DEF := -DGPU_LOG_PREFIX='"[nvn-vk] "' -DGPU_VK
else ifeq ($(GPU),null)
  BACKEND_C := $(TOTK)/android/gfx/nvn_null.c
  BACKEND_DEF :=
else
  $(error GPU must be vk or null)
endif
GFX_C := $(filter-out %/fps_overlay.c %/totk_sysload.c,$(wildcard $(TOTK)/third_party/d2r_gfx/gfx/nvn/*.c $(TOTK)/third_party/d2r_gfx/gfx/tex/*.c)) \
  $(BACKEND_C) $(TOTK)/android/gfx/nvn_stubs.c
GFX_S := $(TOTK)/third_party/d2r_gfx/gfx/nvn/nvn_thunks.S $(TOTK)/third_party/d2r_gfx/gfx/nvn/nvn_entry.S

ifeq ($(APP),1)
  PICFLAG := -fPIC -DTOTK_ANDROID_APP -Dmain=totk_host_main
  OBJNAME := obj-app
else
  PICFLAG := -fPIE
  OBJNAME := obj
endif
# The pinned 1.4.3 hashes (scripts/make_pins_header.py). TOTK_PINS_H tells setup/setup.mk that this file owns the rule.
.DEFAULT_GOAL := all
TOTK_PINS_H := $(OUT)/totk_pins.h
$(TOTK_PINS_H): $(TOTK)/scripts/make_pins_header.py $(TOTK)/assets/expected-v1.4.3.json
	@mkdir -p $(dir $@)
	$(PYTHON) $(TOTK)/scripts/make_pins_header.py $(TOTK)/assets/expected-v1.4.3.json $@
ifeq ($(VERSION),v143)
  PINS_DEF := -DTOTK_PINNED=1
  PINS_DEP := $(TOTK_PINS_H)
endif
# No host path in what is built (__FILE__ strings, DWARF): the repository's files as games/totk/..., the build folder as
# out/..., the NDK as ndk/... (content audit F6; build_release.py also strips the debug info from the APK's copies).
PREFIX_MAP := -ffile-prefix-map=$(abspath $(TOTK)/../..)/= -ffile-prefix-map=$(abspath $(OUT))/=out/ \
  $(if $(NDK),-ffile-prefix-map=$(abspath $(NDK))/=ndk/)
COMMON := $(PREFIX_MAP) -O2 -g $(PICFLAG) -fno-omit-frame-pointer -D_GNU_SOURCE -DTOTK_DEV_BUILD=$(DEV_BUILD) $(VDEF) $(PINS_DEF) -I$(OUT) -I$(TOTK)/runtime \
  -I$(TOTK)/third_party/d2r_gfx/gfx/nvn -I$(TOTK)/third_party/d2r_gfx/gfx/nvn/include
COMMON += $(PROFILE_FLAGS) $(PGO_CFLAGS)
PSO_COLLECT ?= 0
COMMON += -DTOTK_PSO_COLLECT_DEFAULT=$(PSO_COLLECT)
ifneq ($(OPUS),)
  COMMON += -I$(OPUS)/include/opus -I$(OPUS)/include
  OPUS_LIB := $(OPUS)/lib/libopus.a
endif
ifneq ($(VPX),)
  COMMON += -I$(VPX)/include
  VPX_LIB := $(VPX)/lib/libvpx.a
endif
WARN := -std=gnu11 -Wall -Wno-format -Wno-misleading-indentation -Wno-unused-function
COMPAT := -I$(TOTK)/android/compat/include -include $(TOTK)/android/compat/include/darwin_compat.h -I$(TOTK)/third_party/d2r_gfx/host

OBJ := $(OUT)/$(OBJNAME)
RUNTIME_O := $(patsubst runtime/%,$(OBJ)/rt/%.o,$(RUNTIME_C) $(RUNTIME_S))
GFX_O := $(patsubst $(TOTK)/%,$(OBJ)/gfx/%.o,$(GFX_C) $(GFX_S))

ifeq ($(APP),1)
all: $(OUT)/libtotk.so $(OUT)/libdl_android.so
else
all: $(OUT)/totk-native
endif

# A single current-value stamp also invalidates objects when switching back to a previous build kind.
# FORCE checks each invocation, but an unchanged value leaves the timestamp (and objects) untouched.
$(DEV_BUILD_STAMP): FORCE
	@mkdir -p $(dir $@)
	@if [ ! -f "$@" ] || [ "$$(cat "$@")" != "$(DEV_BUILD) $(PROFILE_FP) $(PGO_ID)" ]; then printf '%s\n' '$(DEV_BUILD) $(PROFILE_FP) $(PGO_ID)' > "$@"; fi
FORCE:

# -MMD -MP: each object gets a .d file listing the headers it read, so changing a header (a struct in vk_int.h, say)
# rebuilds every object that includes it instead of linking objects built against the old layout.
DEPFLAGS := -MMD -MP
$(OBJ)/rt/%.c.o: $(TOTK)/runtime/%.c $(PINS_DEP) $(DEV_BUILD_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(WARN) $(COMMON) $(DEPFLAGS) -c $< -o $@
$(OBJ)/rt/%.S.o: $(TOTK)/runtime/%.S $(DEV_BUILD_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(COMMON) $(DEPFLAGS) -c $< -o $@
$(OBJ)/gfx/%.c.o: $(TOTK)/%.c $(DEV_BUILD_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(WARN) -Wno-unused-parameter $(COMMON) $(COMPAT) $(BACKEND_DEF) $(DEPFLAGS) -c $< -o $@
$(OBJ)/gfx/%.S.o: $(TOTK)/%.S $(DEV_BUILD_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(COMMON) $(DEPFLAGS) -c $< -o $@
-include $(RUNTIME_O:.o=.d) $(GFX_O:.o=.d)

# -rdynamic: the NVN layer and the host-stack wrappers name host functions with dladdr in diagnostics.
$(OUT)/totk-native: $(RUNTIME_O) $(GFX_O)
	$(CC) -pie -rdynamic $(PGO_LDFLAGS) -o $@ $^ $(OPUS_LIB) $(VPX_LIB) $(PLATFORM_LIBS)
	@echo "built $@ ($(TARGET), $(VERSION), GPU $(GPU)$(if $(PGO_ID), PGO $(PGO_ID)))"

# libtotk.map: only ANativeActivity_onCreate and the companion's JNI_OnLoad are exported, so the runtime's symbols bind
# inside the library.
$(OUT)/libtotk.so: $(RUNTIME_O) $(GFX_O) $(TOTK)/android/libtotk.map $(TOTK)/android/Makefile
	@rm -f $(OBJ)/rt/frontend_android.c.o; $(CC) $(WARN) $(COMMON) -c $(TOTK)/runtime/frontend_android.c -o $(OBJ)/rt/frontend_android.c.o   # the build time in the log is this link's
	$(CC) -shared $(PGO_LDFLAGS) -Wl,--build-id=sha1 -Wl,--version-script=$(TOTK)/android/libtotk.map -Wl,--no-undefined -Wl,-soname,libtotk.so -o $@ $(RUNTIME_O) $(GFX_O) $(OPUS_LIB) $(VPX_LIB) $(PLATFORM_LIBS) -landroid
	@echo "built $@ ($(TARGET), $(VERSION), GPU $(GPU), app$(if $(PGO_ID), PGO $(PGO_ID)))"
# The app's libdl_android.so (android/apk/dl_android_stub.c): what the Vulkan driver's system dependencies get instead of
# the platform's linker-namespace API.
# No symbol versions: bionic accepts an unversioned definition for any version a library asks for.
$(OUT)/libdl_android.so: $(TOTK)/android/apk/dl_android_stub.c $(TOTK)/android/Makefile
	$(CC) -O2 -fPIC -shared -Wl,-soname,libdl_android.so -o $@ $<

# Keep volk/C++ dispatch private to an opt-in dlopen library, not libtotk.
ifeq ($(TARGET)-$(APP)-$(GPU),android-1-vk)
LSFG := $(TOTK)/android/third_party/lsfg
LSFG_INPUTS := $(shell find $(LSFG) -type f)
all: $(OUT)/libtotk_lsfg.so
$(OUT)/libtotk_lsfg.so: $(LSFG_INPUTS)
	cmake -S $(LSFG) -B $(OUT)/lsfg-build -DCMAKE_TOOLCHAIN_FILE=$(NDK)/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-$(API) -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=Release -DCMAKE_LIBRARY_OUTPUT_DIRECTORY=$(OUT) \
	  "-DCMAKE_C_FLAGS=$(strip $(PREFIX_MAP))" "-DCMAKE_CXX_FLAGS=$(strip $(PREFIX_MAP))"
	cmake --build $(OUT)/lsfg-build -j4 --target totk_lsfg
endif

# The Vulkan capability probe (android/vkprobe): needs no game files.
#   make -C android vkprobe NDK=/path/to/android-ndk
vkprobe: $(OUT)/vkprobe
$(OUT)/vkprobe: $(TOTK)/android/vkprobe/vkprobe.c
	@mkdir -p $(dir $@)
	$(CC) -O2 -Wall -fPIE -pie -o $@ $< -ldl
	@echo "built $@"

# The Vulkan backend's own tests (no game files): pools/copies/ordering, textured resource correctness,
# cross-mode recorder pixels/boundaries, immutable span lookup, and async pipelines/cache locking.
# Their shader seed comes from android/gfx/test_vkdraw_seed.py, which needs glslangValidator.
#   make -C android vktest TARGET=linux CC=cc OUT=<scratch>/vk
# Builds vktest, vkdrawtest, vkrectest, vkrecspantest, vkpipetest, threadplacetest and vkdraw-seed/.
VKTEST_SRC := $(TOTK)/android/gfx/nvn_vk.c $(TOTK)/android/gfx/vk_load.c $(TOTK)/android/gfx/vk_fmt.c $(TOTK)/android/gfx/vk_res.c $(TOTK)/android/gfx/vk_upload_pool.c \
  $(TOTK)/android/gfx/vk_shader.c $(TOTK)/android/gfx/vk_compute.c $(TOTK)/android/gfx/vk_pcache.c $(TOTK)/android/gfx/vk_seed_io.c $(TOTK)/android/gfx/vk_shader_xlate.c $(TOTK)/android/gfx/vk_draw.c $(TOTK)/android/gfx/vk_canvas.c $(TOTK)/android/gfx/vk_post.c $(TOTK)/android/gfx/vk_framegen.c $(TOTK)/android/gfx/vk_record.c $(TOTK)/android/gfx/vk_gpuprof.c $(TOTK)/android/gfx/vk_overlay.c $(TOTK)/third_party/d2r_gfx/gfx/nvn/nvn_tex.c \
  $(TOTK)/third_party/d2r_gfx/gfx/nvn/nvn_dirty.c $(TOTK)/third_party/d2r_gfx/gfx/nvn/totk_lifelog.c \
  $(TOTK)/third_party/d2r_gfx/gfx/tex/nvn_deswizzle.c $(TOTK)/android/gfx/host_test_support.c $(TOTK)/runtime/frame_timeline.c $(TOTK)/runtime/frame_fence.c \
  $(TOTK)/runtime/fps_hybrid_runtime.c $(TOTK)/runtime/fps_adaptive_runtime.c
VKTEST_SRC += $(TOTK)/android/gfx/vk_framegen_qcom.c $(TOTK)/android/gfx/vk_framegen_lsfg.c
VKTEST_FLAGS := -O1 -g $(WARN) -Wno-unused-parameter -D_GNU_SOURCE -DGPU_VK -DTOTK_VK_TESTS -I$(TOTK)/runtime -I$(TOTK)/third_party/d2r_gfx/gfx/nvn \
  -I$(TOTK)/third_party/d2r_gfx/gfx/nvn/include $(COMPAT) -fPIE -pie
VKTEST_LIBS := -ldl -lm $(if $(filter linux,$(TARGET)),-lpthread)
VKTEST_FRONTEND := $(OUT)/test_vktexcopy_frontend.o
$(VKTEST_FRONTEND): $(TOTK)/android/gfx/test_vktexcopy_frontend.c $(TOTK)/third_party/d2r_gfx/gfx/nvn/hle_nvn.c
	@mkdir -p $(dir $@)
	$(CC) $(VKTEST_FLAGS) -fno-omit-frame-pointer -ffunction-sections -fdata-sections -MMD -MP -MF $(@:.o=.d) -c -o $@ $<
-include $(VKTEST_FRONTEND:.o=.d)
$(addprefix $(OUT)/,vktest vkdrawtest vkrecspantest vkpipetest vkdrawtest-app vkoverlaytest vkposttest vkframegentest vknativeuitest vkxlatetest): $(TOTK)/runtime/fps_hybrid_runtime.h $(TOTK)/android/gfx/fps_hybrid.h $(TOTK)/android/gfx/fps_auto_scene.h
$(addprefix $(OUT)/,vktest vkdrawtest vkrecspantest vkpipetest vkdrawtest-app vkoverlaytest vkposttest vkframegentest vknativeuitest vkxlatetest): $(TOTK)/runtime/fps_adaptive_runtime.h
$(addprefix $(OUT)/,vktest vkdrawtest vkrecspantest vkpipetest vkdrawtest-app vkoverlaytest vkposttest vkframegentest vknativeuitest vkxlatetest vkpcachetest poolfintest vkreplaypintest): $(TOTK)/android/gfx/framegen_adaptive_present.h $(TOTK)/android/gfx/framegen_adaptive_defaults.h
$(addprefix $(OUT)/,vktest vkdrawtest vkrecspantest vkpipetest vkdrawtest-app vkoverlaytest vkposttest vkframegentest vknativeuitest vkxlatetest vkpcachetest poolfintest vkreplaypintest): $(TOTK)/android/gfx/vk_post_spv.h $(TOTK)/android/gfx/vk_post_lut.h $(TOTK)/android/gfx/vk_framegen_spv.h $(TOTK)/android/gfx/vk_clear_mask_spv.h
$(addprefix $(OUT)/,vktest vkdrawtest vkrecspantest vkpipetest vkdrawtest-app vkoverlaytest vkposttest vkframegentest vknativeuitest vkxlatetest vkpcachetest poolfintest vkreplaypintest): $(TOTK)/android/gfx/vk_framegen_qcom.h $(TOTK)/android/gfx/vk_framegen_qcom_spv.h $(TOTK)/android/gfx/framegen_budget_external.h $(TOTK)/android/gfx/framegen_qcom_policy.h
$(addprefix $(OUT)/,vktest vkdrawtest vkrecspantest vkpipetest vkdrawtest-app vkoverlaytest vkposttest vkframegentest vknativeuitest vkxlatetest vkpcachetest poolfintest vkreplaypintest): $(TOTK)/android/gfx/vk_framegen_lsfg.h $(TOTK)/android/gfx/framegen_budget_lsfg.h
$(addprefix $(OUT)/,vktest vkdrawtest vkrecspantest vkpipetest vkdrawtest-app vkoverlaytest vkposttest vkframegentest vknativeuitest vkxlatetest vkpcachetest poolfintest vkreplaypintest): $(TOTK)/android/gfx/framegen_drs.h $(TOTK)/runtime/drs_framegen.h
$(addprefix $(OUT)/,vktest vkdrawtest vkrecspantest vkpipetest vkdrawtest-app vkoverlaytest vkposttest vkframegentest vknativeuitest vkxlatetest vkpcachetest poolfintest vkreplaypintest): $(TOTK)/android/gfx/vk_pcache.h $(TOTK)/android/gfx/vk_pcache_store.h $(TOTK)/android/gfx/vk_pcache_identity.h
$(addprefix $(OUT)/,vktest vkdrawtest vkrecspantest vkpipetest vkdrawtest-app vkoverlaytest vkposttest vkframegentest vknativeuitest vkxlatetest vkpcachetest poolfintest vkreplaypintest vkrectest vkcanvastest): $(TOTK)/android/gfx/pso_trace_key.h
$(addprefix $(OUT)/,vktest vkdrawtest vkrecspantest vkpipetest vkdrawtest-app vkoverlaytest vkposttest vkframegentest vknativeuitest vkxlatetest vkpcachetest poolfintest vkreplaypintest): $(TOTK)/android/gfx/vk_screen_classify.h
$(addprefix $(OUT)/,vktest vkdrawtest vkrecspantest vkpipetest vkdrawtest-app vkoverlaytest vkposttest vkframegentest vknativeuitest vkxlatetest vkpcachetest poolfintest vkreplaypintest): $(TOTK)/android/gfx/vk_alpha_test.h
$(addprefix $(OUT)/,vktest vkdrawtest vkrecspantest vkpipetest vkdrawtest-app vknativeuitest vkpcachetest poolfintest vkreplaypintest): $(TOTK)/android/gfx/vk_pso_keys.h $(TOTK)/android/gfx/vk_pso_runtime.inc
$(addprefix $(OUT)/,vktest vkdrawtest vkrecspantest vkpipetest vkdrawtest-app vkoverlaytest vkposttest vkframegentest vknativeuitest vkxlatetest vkpcachetest poolfintest vkreplaypintest vkrectest vkcanvastest): $(TOTK)/android/gfx/pso_trace_key.h
$(addprefix $(OUT)/,vktest vkdrawtest vkrecspantest vkpipetest vkdrawtest-app vkoverlaytest vkposttest vkframegentest vknativeuitest vkxlatetest vkpcachetest poolfintest vkreplaypintest): $(TOTK)/android/gfx/vk_screen_classify.h
$(addprefix $(OUT)/,vktest vkdrawtest vkrecspantest vkpipetest vkdrawtest-app vkoverlaytest vkposttest vkframegentest): $(TOTK)/runtime/frame_lag.h $(TOTK)/runtime/guest_clock.h $(TOTK)/android/gfx/vk_present_policy.h $(TOTK)/android/gfx/vk_timestamp.h $(TOTK)/android/gfx/vk_gpu_time.h
$(addprefix $(OUT)/,vktest vkdrawtest vkrecspantest vkpipetest vkdrawtest-app vkoverlaytest vkposttest vkframegentest): $(TOTK)/android/gfx/vk_present_job.h
$(addprefix $(OUT)/,vktest vkdrawtest vkrecspantest vkpipetest vkdrawtest-app vknativeuitest vkrectest vkposttest vkframegentest vkoverlaytest vkpcachetest poolfintest): $(TOTK)/runtime/frame_timeline.h $(TOTK)/runtime/present_timeline.h $(TOTK)/android/gfx/vk_present_slots.h
vktest: $(OUT)/vktest $(OUT)/vkdrawtest $(OUT)/vkrectest $(OUT)/vkrecspantest $(OUT)/vkpipetest $(OUT)/threadplacetest $(OUT)/vknativeuitest $(OUT)/vkdraw-seed/.done
vktest: $(OUT)/vkcanvastest
vktest: $(OUT)/vkseedpacktest $(OUT)/vkseediotest
vktest: $(OUT)/vkcomputestoretest
vktest: $(OUT)/vksubgrouptest
vksubgrouptest: $(OUT)/vksubgrouptest
$(OUT)/vksubgrouptest: $(TOTK)/android/gfx/test_vk_subgroup.c $(TOTK)/android/gfx/vk_subgroup.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -O1 -g $(WARN) -o $@ $< -lvulkan
$(addprefix $(OUT)/,vktest vkdrawtest vkrecspantest vkpipetest vkdrawtest-app vkoverlaytest vkposttest vkframegentest vknativeuitest vkxlatetest vkpcachetest poolfintest vkreplaypintest vkrectest vkcanvastest): $(TOTK)/android/gfx/vk_subgroup.h
vktest: $(OUT)/vkpositiontest
.PHONY: vkpositiontest
vkpositiontest: $(OUT)/vkpositiontest
$(OUT)/vkpositiontest: $(TOTK)/android/gfx/test_vk_position.c $(TOTK)/android/gfx/vk_position.h $(TOTK)/android/gfx/vk_seed_pack.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -D_GNU_SOURCE -O1 -g $(WARN) -o $@ $<
$(addprefix $(OUT)/,vktest vkdrawtest vkrecspantest vkpipetest vkdrawtest-app vkoverlaytest vkposttest vkframegentest vknativeuitest vkxlatetest vkpcachetest poolfintest vkreplaypintest vkrectest vkcanvastest): $(TOTK)/android/gfx/vk_position.h
vktest: $(OUT)/vkreplaypintest
$(OUT)/vkreplaypintest: $(TOTK)/android/gfx/test_vkreplaypin.c $(VKTEST_SRC) $(TOTK)/android/gfx/vk_int.h $(TOTK)/android/gfx/vk_framegen.h
	@mkdir -p $(dir $@)
	$(CC) $(VKTEST_FLAGS) -o $@ $(filter %.c,$^) $(VKTEST_LIBS)
$(OUT)/vkcomputestoretest: $(TOTK)/android/gfx/test_vk_compute_store.c $(TOTK)/android/gfx/vk_compute_store.h
	@mkdir -p $(dir $@)
	$(CC) -O1 -g $(WARN) -D_GNU_SOURCE -o $@ $(filter %.c,$^)
$(addprefix $(OUT)/,vktest vkdrawtest vkrecspantest vkpipetest vkdrawtest-app vknativeuitest vkpcachetest): $(TOTK)/android/gfx/vk_compute_store.h
vktest: $(OUT)/texelregiontest
vktest: $(OUT)/vkpcachetest
vktest: $(OUT)/poolfintest
poolfintest: $(OUT)/poolfintest
$(OUT)/poolfintest: $(TOTK)/tests/pool_finalize/probe.c $(TOTK)/tests/guest_waits/host_support.c $(VKTEST_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(VKTEST_FLAGS) -DDM_THREADS_HOST_TEST -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ $(filter %.c,$^) $(VKTEST_LIBS)
$(OUT)/vkpcachetest: $(TOTK)/android/gfx/test_vkpcache.c $(VKTEST_SRC) $(TOTK)/android/gfx/vk_pcache.h $(TOTK)/android/gfx/vk_pcache_store.h
	@mkdir -p $(dir $@)
	$(CC) $(VKTEST_FLAGS) -o $@ $(filter %.c,$^) $(VKTEST_LIBS)
texelregiontest: $(OUT)/texelregiontest
$(OUT)/texelregiontest: $(TOTK)/android/gfx/test_texel_region.c $(TOTK)/third_party/d2r_gfx/gfx/tex/nvn_deswizzle.c $(TOTK)/third_party/d2r_gfx/gfx/nvn/nvn_tex.c $(TOTK)/third_party/d2r_gfx/gfx/tex/nvn_deswizzle.h $(TOTK)/third_party/d2r_gfx/gfx/nvn/nvn_tex.h
	@mkdir -p $(dir $@)
	$(CC) -O2 -g $(WARN) -o $@ $(filter %.c,$^)
# Parallel upload deswizzle: byte identity against the serial decoder with 0/1/3 helpers (no Vulkan). Under
# CC='clang -fsanitize=thread' it is the pool's race check; --bench prints serial vs helper times.
vktest: $(OUT)/uploadpooltest
uploadpooltest: $(OUT)/uploadpooltest
$(OUT)/uploadpooltest: $(TOTK)/android/gfx/test_upload_pool.c $(TOTK)/android/gfx/vk_upload_pool.c $(TOTK)/third_party/d2r_gfx/gfx/tex/nvn_deswizzle.c $(TOTK)/third_party/d2r_gfx/gfx/nvn/nvn_tex.c \
  $(TOTK)/android/gfx/vk_upload_pool.h $(TOTK)/third_party/d2r_gfx/gfx/tex/nvn_deswizzle.h $(TOTK)/third_party/d2r_gfx/gfx/nvn/nvn_tex.h
	@mkdir -p $(dir $@)
	$(CC) -O2 -g $(WARN) -D_GNU_SOURCE -o $@ $(filter %.c,$^) -lpthread
$(addprefix $(OUT)/,vktest vkdrawtest vkrecspantest vkpipetest vkdrawtest-app vkoverlaytest vkposttest vkframegentest vknativeuitest vkxlatetest): $(TOTK)/android/gfx/vk_seed_io.h $(TOTK)/android/gfx/vk_seed_pack.h
$(OUT)/vkseedpacktest: $(TOTK)/android/gfx/test_vk_seed_pack.c $(TOTK)/android/gfx/vk_seed_pack.h
	@mkdir -p $(dir $@)
	$(CC) -O2 -g $(WARN) -o $@ $(filter %.c,$^)
$(OUT)/vkseediotest: $(TOTK)/android/gfx/test_vk_seed_io.c $(TOTK)/android/gfx/vk_seed_io.c $(TOTK)/android/gfx/vk_shader_xlate.c $(TOTK)/android/gfx/host_test_support.c $(TOTK)/android/gfx/vk_seed_io.h $(TOTK)/android/gfx/vk_seed_pack.h
	@mkdir -p $(dir $@)
	$(CC) $(VKTEST_FLAGS) -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ $(filter %.c,$^) $(VKTEST_LIBS)
$(OUT)/vkcanvastest: $(TOTK)/android/gfx/test_vkcanvas.c $(TOTK)/android/gfx/vk_canvas.c $(TOTK)/android/gfx/vk_canvas.h
	@mkdir -p $(dir $@)
	$(CC) -O1 -g $(WARN) -o $@ $(filter %.c,$^)
vktest-native-ui: $(OUT)/vknativeuitest $(OUT)/vkdraw-seed/.done
$(OUT)/vknativeuitest: $(TOTK)/android/gfx/vk_int.h $(TOTK)/android/gfx/vk_present_policy.h
$(OUT)/vknativeuitest: $(TOTK)/android/gfx/test_vknativeui.c $(VKTEST_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(VKTEST_FLAGS) -DTOTK_GAME_V143=1 -o $@ $(filter %.c,$^) $(VKTEST_LIBS)
	@echo "built $@"
$(OUT)/vktest: $(TOTK)/runtime/mprotect_stats.h
$(OUT)/vktest: $(TOTK)/android/gfx/test_vk.c $(VKTEST_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(VKTEST_FLAGS) -o $@ $(filter %.c,$^) $(VKTEST_LIBS)
	@echo "built $@"
$(OUT)/vkdrawtest $(OUT)/vktest $(OUT)/vkrecspantest $(OUT)/vkpipetest: $(TOTK)/android/gfx/vk_load_proof.h
$(OUT)/vkdrawtest: $(TOTK)/android/gfx/test_vkdraw.c $(VKTEST_SRC) $(VKTEST_FRONTEND)
	@mkdir -p $(dir $@)
	$(CC) $(VKTEST_FLAGS) -Wl,--gc-sections -o $@ $(filter %.c %.o,$^) $(VKTEST_LIBS)
	@echo "built $@"
# Real profiler/recorder integration; Python drives isolated launch modes and JSONL rotation.
.PHONY: vkgpuproftest
vkgpuproftest: $(OUT)/vkgpuproftest $(OUT)/vkdraw-seed/.done
vktest: $(OUT)/vkgpuproftest
$(OUT)/vkgpuproftest: $(TOTK)/android/gfx/test_vk_gpuprof.c $(VKTEST_SRC) $(TOTK)/android/gfx/vk_int.h $(TOTK)/android/gfx/vk_record.h $(TOTK)/android/gfx/vk_load_proof.h
$(OUT)/vkgpuproftest: $(TOTK)/android/gfx/vk_gpuprof_time.h
	@mkdir -p $(dir $@)
	$(CC) $(VKTEST_FLAGS) -o $@ $(filter %.c,$^) $(VKTEST_LIBS)
	@echo "built $@"
$(OUT)/vkrectest: $(TOTK)/android/gfx/test_vkrecord.c
	@mkdir -p $(dir $@)
	$(CC) $(VKTEST_FLAGS) -o $@ $< $(VKTEST_LIBS)
	@echo "built $@"
$(OUT)/vkrecspantest: $(TOTK)/android/gfx/test_vkrecord_spans.c $(VKTEST_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(VKTEST_FLAGS) -o $@ $(filter %.c,$^) $(VKTEST_LIBS)
	@echo "built $@"
# Build the pipeline fixture and its shader seed; choose --screen-classify to run the aligned-target arm.
vkpipetest: $(OUT)/vkpipetest $(OUT)/vkdraw-seed/.done $(OUT)/screenclassifytest
$(OUT)/vkpipetest: $(TOTK)/android/gfx/test_vkpipeline.c $(VKTEST_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(VKTEST_FLAGS) -o $@ $(filter %.c,$^) $(VKTEST_LIBS)
	@echo "built $@"
vkxlatetest: $(OUT)/vkxlatetest
$(OUT)/vkxlatetest: $(TOTK)/android/gfx/test_vkshader_xlate.c $(VKTEST_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(VKTEST_FLAGS) -o $@ $(filter %.c,$^) $(VKTEST_LIBS)
# Per-thread placement/parser tests, no Vulkan or game required.
threadplacetest: $(OUT)/threadplacetest
$(OUT)/threadplacetest: $(TOTK)/android/gfx/test_thread_place.c $(TOTK)/runtime/thread_place.h
	@mkdir -p $(dir $@)
	$(CC) -O1 -g $(WARN) -D_GNU_SOURCE -I$(TOTK)/runtime -fPIE -pie -o $@ $< $(VKTEST_LIBS)
# Deterministic clock/fence/GPU timing helper tests: no game files, Vulkan or device.
#   make -C android TARGET=linux CC=cc OUT=<scratch> frametimelinetest framefencetest acquireclocktest displaypacetest acquireslacktest gpuitimingtest
HOSTTEST_FLAGS := -O1 -g -std=gnu11 -Wall -Wextra -D_GNU_SOURCE
# Canonical graphics pipeline trace-key hashing; pure host test, no Vulkan dependency.
psotracetest: $(OUT)/psotracetest
$(OUT)/psotracetest: $(TOTK)/android/gfx/test_pso_trace_key.c $(TOTK)/android/gfx/pso_trace_key.h
	@mkdir -p $(dir $@)
	$(CC) $(HOSTTEST_FLAGS) -O2 -Werror -o $@ $<
# The helper arm shares fixture cases but has no Vulkan, shader-seed, or game-file dependency.
screenclassifytest: $(OUT)/screenclassifytest
vktest: $(OUT)/screenclassifytest
$(OUT)/screenclassifytest: $(TOTK)/android/gfx/test_vkpipeline.c $(TOTK)/android/gfx/vk_screen_classify.h
	@mkdir -p $(dir $@)
	$(CC) $(HOSTTEST_FLAGS) -Werror -DTOTK_SCREEN_CLASSIFY_ONLY -o $@ $<
FRAME_TEST_SRC := $(TOTK)/runtime/frame_timeline.c $(TOTK)/runtime/frame_fence.c
FRAME_TEST_HEADERS := $(TOTK)/runtime/frame_timeline.h $(TOTK)/runtime/frame_fence.h
frametimelinetest: $(OUT)/frametimelinetest
$(OUT)/frametimelinetest: $(TOTK)/android/gfx/test_frame_timeline.c $(FRAME_TEST_SRC) $(FRAME_TEST_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(HOSTTEST_FLAGS) -DTOTK_FRAME_TIMELINE_TEST -pthread -o $@ $(filter %.c,$^) -lm
framefencetest: $(OUT)/framefencetest
$(OUT)/framefencetest: $(TOTK)/android/gfx/test_frame_fence.c $(FRAME_TEST_SRC) $(FRAME_TEST_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(HOSTTEST_FLAGS) -DTOTK_FRAME_TIMELINE_TEST -pthread -o $@ $(filter %.c,$^) -lm
acquireclocktest: $(OUT)/acquireclocktest
$(OUT)/acquireclocktest: $(TOTK)/android/gfx/test_acquire_clock.c $(TOTK)/android/gfx/acquire_clock.h $(TOTK)/android/gfx/acquire_slack.h $(TOTK)/android/gfx/vk_present_slots.h $(TOTK)/runtime/present_clock.h
	@mkdir -p $(dir $@)
	$(CC) $(HOSTTEST_FLAGS) -o $@ $<
acquirerunaheadtest: $(OUT)/acquirerunaheadtest
$(OUT)/acquirerunaheadtest: $(TOTK)/android/gfx/test_acquire_runahead.c $(TOTK)/android/gfx/acquire_clock.h $(TOTK)/android/gfx/acquire_slack.h $(TOTK)/android/gfx/vk_present_slots.h $(TOTK)/runtime/present_clock.h
	@mkdir -p $(dir $@)
	$(CC) $(HOSTTEST_FLAGS) -o $@ $<
displaypacetest: $(OUT)/displaypacetest
$(OUT)/displaypacetest: $(TOTK)/android/gfx/test_display_pace.c $(TOTK)/android/gfx/display_pace.h
	@mkdir -p $(dir $@)
	$(CC) $(HOSTTEST_FLAGS) -o $@ $<
displayclocktest: $(OUT)/displayclocktest
$(OUT)/displayclocktest: $(TOTK)/android/gfx/test_display_clock.c $(TOTK)/android/gfx/display_clock.h $(TOTK)/android/gfx/vk_present_slots.h $(TOTK)/android/gfx/acquire_clock.h $(TOTK)/android/gfx/predict_pace.h $(TOTK)/runtime/present_clock.h
	@mkdir -p $(dir $@)
	$(CC) $(HOSTTEST_FLAGS) -o $@ $<
acquireslacktest: $(OUT)/acquireslacktest
$(OUT)/acquireslacktest: $(TOTK)/android/gfx/test_acquire_slack.c $(TOTK)/third_party/d2r_gfx/gfx/nvn/nvn_pace.h
	@mkdir -p $(dir $@)
	$(CC) $(HOSTTEST_FLAGS) -o $@ $<
predictpacetest: $(OUT)/predictpacetest
$(OUT)/predictpacetest: $(TOTK)/android/gfx/test_predict_pace.c $(TOTK)/android/gfx/predict_pace.h $(TOTK)/android/gfx/vk_present_slots.h $(TOTK)/runtime/present_predict.h
	@mkdir -p $(dir $@)
	$(CC) $(HOSTTEST_FLAGS) -o $@ $<
predictbudgettest: $(OUT)/predictbudgettest
$(OUT)/predictbudgettest: $(TOTK)/android/gfx/test_predict_budget.c $(TOTK)/runtime/predict_budget.h
	@mkdir -p $(dir $@)
	$(CC) $(HOSTTEST_FLAGS) -o $@ $<
adrenogmemtest: $(OUT)/adrenogmemtest
$(OUT)/adrenogmemtest: $(TOTK)/android/gfx/test_adreno_gmem.c $(TOTK)/runtime/adreno_gmem.h
	@mkdir -p $(dir $@)
	$(CC) $(HOSTTEST_FLAGS) -o $@ $<
fpsautotest: $(OUT)/fpsautotest
$(OUT)/fpsautotest: $(TOTK)/android/gfx/test_fps_auto.c $(TOTK)/android/gfx/fps_auto.h $(TOTK)/android/gfx/acquire_clock.h $(TOTK)/android/gfx/vk_present_slots.h $(TOTK)/runtime/present_clock.h $(TOTK)/runtime/fps_auto_runtime.h
	@mkdir -p $(dir $@)
	$(CC) $(HOSTTEST_FLAGS) -o $@ $<
fpshybridtest: $(OUT)/fpshybridtest
$(OUT)/fpshybridtest: $(TOTK)/android/gfx/test_fps_hybrid.c $(TOTK)/android/gfx/fps_hybrid.h $(TOTK)/runtime/fps_hybrid_runtime.c $(TOTK)/runtime/fps_hybrid_runtime.h $(TOTK)/android/gfx/fps_auto_scene.h
	@mkdir -p $(dir $@)
	$(CC) $(HOSTTEST_FLAGS) -D_GNU_SOURCE -DGPU_VK -DTOTK_VK_TESTS -o $@ $(filter %.c,$^) -lm -lpthread
fpsadaptivetest: $(OUT)/fpsadaptivetest
$(OUT)/fpsadaptivetest: $(TOTK)/runtime/test_fps_adaptive_runtime.c $(TOTK)/runtime/fps_adaptive_runtime.c $(TOTK)/runtime/fps_adaptive_runtime.h $(TOTK)/runtime/fps_hybrid_runtime.c $(TOTK)/runtime/fps_hybrid_runtime.h $(TOTK)/android/gfx/fps_auto_scene.h $(TOTK)/android/gfx/fps_hybrid.h
	@mkdir -p $(dir $@)
	$(CC) $(HOSTTEST_FLAGS) -D_GNU_SOURCE -DGPU_VK -DTOTK_VK_TESTS -o $@ $(filter %.c,$^) -lm -lpthread
fps60patchtest: $(OUT)/fps60patchtest
$(OUT)/fps60patchtest: $(TOTK)/runtime/test_v143_fps60_patch_table.c $(TOTK)/runtime/game_patches_v143.c $(TOTK)/runtime/game_patches.h $(TOTK)/runtime/runtime.h
	@mkdir -p $(dir $@)
	$(CC) $(HOSTTEST_FLAGS) -DTOTK_GAME_V143=1 -I$(TOTK)/android/compat/include -include $(TOTK)/android/compat/include/darwin_compat.h \
	  -I$(TOTK)/third_party/d2r_gfx/host -I$(TOTK)/runtime -o $@ $< -lm -lpthread
fpsautoscenetest: $(OUT)/fpsautoscenetest
$(OUT)/fpsautoscenetest: $(TOTK)/android/gfx/test_fps_auto_scene.c $(TOTK)/android/gfx/fps_auto_scene.h $(TOTK)/android/gfx/fps_auto.h
	@mkdir -p $(dir $@)
	$(CC) $(HOSTTEST_FLAGS) -o $@ $<
fpsautoblocktest: $(OUT)/fpsautoblocktest
$(OUT)/fpsautoblocktest: $(TOTK)/android/gfx/test_fps_auto_block.c $(TOTK)/runtime/fps_auto_block.h $(TOTK)/runtime/fps_auto_runtime.h
	@mkdir -p $(dir $@)
	$(CC) $(HOSTTEST_FLAGS) -o $@ $<
presentslottest: $(OUT)/presentslottest
$(OUT)/presentslottest: $(TOTK)/android/gfx/test_present_slots.c $(TOTK)/android/gfx/vk_present_slots.h $(TOTK)/android/gfx/framegen_slots.h $(TOTK)/android/gfx/vk_present_fence.h $(TOTK)/android/gfx/vk_present_cadence.h $(TOTK)/runtime/present_clock.h $(TOTK)/runtime/present_timeline.h $(TOTK)/runtime/present_predict.h
	@mkdir -p $(dir $@)
	$(CC) $(HOSTTEST_FLAGS) -O2 -Werror -o $@ $<
presentfencetest: $(OUT)/presentfencetest
$(OUT)/presentfencetest: $(TOTK)/android/gfx/test_present_fence.c $(TOTK)/android/gfx/vk_present_fence.h $(TOTK)/android/gfx/vk_present_cadence.h $(TOTK)/runtime/present_predict.h
	@mkdir -p $(dir $@)
	$(CC) $(HOSTTEST_FLAGS) -o $@ $<
# Actual bounded retirement worker with deterministic pthread/pipe stand-ins.
#   make -C games/totk/android TARGET=linux CC=cc OUT=<scratch> presentretiretest
#   <scratch>/presentretiretest
presentretiretest: $(OUT)/presentretiretest
$(OUT)/presentretiretest: $(TOTK)/android/gfx/test_present_retire.c $(TOTK)/android/gfx/vk_present_retire.h $(TOTK)/android/gfx/vk_present_fence.h $(TOTK)/android/gfx/vk_present_cadence.h $(TOTK)/android/gfx/framegen_adaptive_present.h $(TOTK)/android/gfx/framegen_slots.h $(TOTK)/android/gfx/framegen_job_trace.h $(TOTK)/runtime/present_predict.h
	@mkdir -p $(dir $@)
	$(CC) $(HOSTTEST_FLAGS) -O2 -Werror -pthread -o $@ $<
# The same draw test with the app's window presenter against a stand-in window (desktop only: TARGET=linux).
vktest-app: $(OUT)/vkdrawtest-app $(OUT)/vkdraw-seed/.done
$(OUT)/vkdrawtest-app: $(TOTK)/android/gfx/test_vkdraw.c $(TOTK)/android/gfx/test_window_mock.c $(VKTEST_SRC) $(VKTEST_FRONTEND)
	@mkdir -p $(dir $@)
	$(CC) $(VKTEST_FLAGS) -DTOTK_ANDROID_APP -I$(TOTK)/android/gfx/test_shim -Wl,--gc-sections -o $@ $(filter %.c %.o,$^) $(VKTEST_LIBS)
	@echo "built $@"
# Isolated native overlay/presenter pixel tests; run fresh processes for each startup environment.
vktest-overlay: $(OUT)/vkoverlaytest
$(OUT)/vkoverlaytest: $(TOTK)/android/gfx/test_vkoverlay.c $(VKTEST_SRC) $(TOTK)/runtime/build_kind.h $(DEV_BUILD_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(VKTEST_FLAGS) -O2 -DTOTK_GAME_V143=1 -DTOTK_DEV_BUILD=$(DEV_BUILD) -DTOTK_ANDROID_APP -I$(TOTK)/android/gfx/test_shim -o $@ $(filter %.c,$^) $(VKTEST_LIBS)
	@echo "built $@"
# Presentation post chain: run separate processes with AA/scale environment switches.
vkposttest: $(OUT)/vkposttest
$(OUT)/vkposttest: $(TOTK)/android/gfx/test_vkpost.c $(VKTEST_SRC) $(TOTK)/android/gfx/test_sgsr_reference.h
	@mkdir -p $(dir $@)
	$(CC) $(VKTEST_FLAGS) -DTOTK_GAME_V143=1 -o $@ $(filter %.c,$^) $(VKTEST_LIBS)
	@echo "built $@"
# Pure midpoint/deadline fixture; writes only to the caller-selected OUT.
framegenslottest: $(OUT)/framegenslottest
$(OUT)/framegenslottest: $(TOTK)/android/gfx/test_framegen_slots.c $(TOTK)/android/gfx/framegen_slots.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -o $@ $<
	@echo "built $@"
framegenmathtest: $(OUT)/framegenmathtest
$(OUT)/framegenmathtest: $(TOTK)/../../tests/test_framegen_math.c $(TOTK)/android/gfx/framegen_math.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -o $@ $< -lm
	@echo "built $@"
framegenbudgettest: $(OUT)/framegenbudgettest
$(OUT)/framegenbudgettest: $(TOTK)/../../tests/test_framegen_budget.c $(TOTK)/android/gfx/framegen_budget.h $(TOTK)/android/gfx/framegen_budget_external.h $(TOTK)/android/gfx/framegen_budget_lsfg.h $(TOTK)/android/gfx/framegen_qcom_policy.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -o $@ $< -lm
	@echo "built $@"
framegenadaptivetest: $(OUT)/framegenadaptivetest
$(OUT)/framegenadaptivetest: $(TOTK)/android/gfx/test_framegen_adaptive.c $(TOTK)/android/gfx/framegen_adaptive.h $(TOTK)/android/gfx/framegen_adaptive_plan.h $(TOTK)/android/gfx/framegen_adaptive_pace.h $(TOTK)/android/gfx/framegen_budget.h $(TOTK)/android/gfx/framegen_budget_external.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -o $@ $< -lm
	@echo "built $@"
framegenadaptivepresenttest: $(OUT)/framegenadaptivepresenttest
$(OUT)/framegenadaptivepresenttest: $(TOTK)/android/gfx/test_framegen_adaptive_present.c $(TOTK)/android/gfx/framegen_adaptive_present.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -o $@ $<
	@echo "built $@"
framegenadaptiveslottest: $(OUT)/framegenadaptiveslottest
$(OUT)/framegenadaptiveslottest: $(TOTK)/android/gfx/test_framegen_adaptive_slots.c $(TOTK)/android/gfx/framegen_adaptive_slots.h $(TOTK)/android/gfx/framegen_adaptive.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -o $@ $< -lm
framegenadaptivelatchtest: $(OUT)/framegenadaptivelatchtest
$(OUT)/framegenadaptivelatchtest: $(TOTK)/android/gfx/test_framegen_adaptive_latch.c $(TOTK)/android/gfx/framegen_adaptive_latch.h $(TOTK)/android/gfx/framegen_adaptive_defaults.h $(TOTK)/android/gfx/framegen_adaptive_slots.h $(TOTK)/android/gfx/framegen_adaptive_present.h $(TOTK)/runtime/present_timeline.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -o $@ $< -lm
adaptiveslotclocktest: $(OUT)/adaptiveslotclocktest
$(OUT)/adaptiveslotclocktest: $(TOTK)/android/gfx/test_adaptive_slot_clock.c $(TOTK)/runtime/present_clock.h $(TOTK)/runtime/present_timeline.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -o $@ $<
framegenadaptivepublicationtest: $(OUT)/framegenadaptivepublicationtest
$(OUT)/framegenadaptivepublicationtest: $(TOTK)/android/gfx/test_framegen_adaptive_publication.c $(TOTK)/android/gfx/framegen_adaptive_publication.h $(TOTK)/android/gfx/framegen_adaptive_slots.h $(TOTK)/runtime/present_clock.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -o $@ $<
framegenqcompreparedtest: $(OUT)/framegenqcompreparedtest
$(OUT)/framegenqcompreparedtest: $(TOTK)/android/gfx/test_framegen_qcom_prepared.c $(TOTK)/android/gfx/framegen_qcom_policy.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -o $@ $< -lm
framegenslottest: $(OUT)/presentslottest
framegendeptest: $(OUT)/framegendeptest
$(OUT)/framegendeptest: $(TOTK)/../../tests/test_framegen_dependencies.c $(TOTK)/android/gfx/framegen_dependencies.h $(TOTK)/android/gfx/framegen_passes.h $(TOTK)/android/gfx/vk_canvas.c $(TOTK)/android/gfx/vk_canvas.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -o $@ $(filter %.c,$^)
	@echo "built $@"
framegenpasstest: $(OUT)/framegenpasstest
$(OUT)/framegenpasstest: $(TOTK)/../../tests/test_framegen_passes.c $(TOTK)/android/gfx/framegen_passes.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -o $@ $< -lm
	@echo "built $@"
framegentrackbench: $(OUT)/framegentrackbench
$(OUT)/framegentrackbench: $(TOTK)/../../tests/bench_framegen_tracking.c $(TOTK)/android/gfx/framegen_passes.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -O3 -Wall -Wextra -Werror -o $@ $< -lm
	@echo "built $@"
gpuitimingtest: $(OUT)/gpuitimingtest
$(OUT)/gpuitimingtest: $(TOTK)/android/gfx/test_vk_gpu_time.c $(TOTK)/android/gfx/vk_gpu_time.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -o $@ $<
	@echo "built $@"
# Synthetic scene/depth/camera goldens through the actual generation shaders.
vkframegentest: $(OUT)/vkframegentest
$(OUT)/vkframegentest: $(TOTK)/../../tests/test_vkframegen.c $(TOTK)/../../tests/framegen_test_replay.h $(TOTK)/android/gfx/vk_framegen_test_spv.h $(VKTEST_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(VKTEST_FLAGS) -DTOTK_GAME_V143=1 -o $@ $(filter %.c,$^) $(VKTEST_LIBS)
	@echo "built $@"
vkframegenpreparedtest: $(OUT)/vkframegenpreparedtest
$(OUT)/vkframegenpreparedtest: $(TOTK)/../../tests/test_vkframegen_prepared.c $(TOTK)/../../tests/framegen_test_chunk.h $(TOTK)/../../tests/test_vkframegen.c $(TOTK)/../../tests/framegen_test_replay.h $(TOTK)/android/gfx/vk_framegen_test_spv.h $(VKTEST_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(VKTEST_FLAGS) -DTOTK_GAME_V143=1 -o $@ $< $(filter %.c,$(VKTEST_SRC)) $(VKTEST_LIBS)
$(OUT)/vkdraw-seed/.done: $(TOTK)/android/gfx/test_vkdraw_seed.py $(TOTK)/android/vk_reflect.py
	@command -v glslangValidator >/dev/null 2>&1 || { \
	  echo "error: vkdraw-seed requires glslangValidator in PATH (install glslang-tools); do not run draw tests without a completed seed." >&2; exit 1; }
	python3 $< $(OUT)/vkdraw-seed && touch $@
# The dual-screen companion's game-state reader and item actions against a fake 1.4.3 memory layout (no game files).
companiontest: $(OUT)/companiontest
$(OUT)/companiontest: $(TOTK)/tests/companion/test_companion.c $(TOTK)/runtime/companion_snapshot.c $(TOTK)/runtime/companion_actions.c $(TOTK)/runtime/companion.h
	@mkdir -p $(dir $@)
	$(CC) -O1 -g $(WARN) -D_GNU_SOURCE -DTOTK_GAME_V143=1 -I$(TOTK)/runtime -DCOMPANION_ACTIONS_TEST -fsanitize=address,undefined -o $@ \
	  $(TOTK)/tests/companion/test_companion.c $(TOTK)/runtime/companion_snapshot.c $(TOTK)/runtime/companion_actions.c -lm $(if $(filter linux,$(TARGET)),-lpthread)
	@echo "built $@"

clean:
	rm -rf $(OUT)/obj $(OUT)/obj-app $(OUT)/totk-native $(OUT)/libtotk.so $(OUT)/vkprobe $(OUT)/vktest $(OUT)/vkdrawtest $(OUT)/vkrectest $(OUT)/vkrecspantest $(OUT)/vkpipetest $(OUT)/threadplacetest $(OUT)/frametimelinetest $(OUT)/framefencetest $(OUT)/acquireclocktest $(OUT)/displaypacetest $(OUT)/displayclocktest $(OUT)/acquireslacktest $(OUT)/predictpacetest $(OUT)/predictbudgettest $(OUT)/adrenogmemtest $(OUT)/fpsautotest $(OUT)/fpsautoscenetest $(OUT)/fpsautoblocktest $(OUT)/presentfencetest $(OUT)/vkdrawtest-app $(OUT)/vkoverlaytest $(OUT)/vkposttest $(OUT)/vkdraw-seed $(OUT)/companiontest $(DEV_BUILD_STAMP)
	rm -f $(OUT)/framegenslottest $(OUT)/presentslottest $(OUT)/framegenmathtest $(OUT)/framegenbudgettest $(OUT)/vkframegentest $(OUT)/framegendeptest $(OUT)/gpuitimingtest $(OUT)/fpshybridtest $(OUT)/psotracetest $(OUT)/uploadpooltest
	rm -f $(OUT)/framegenadaptivetest $(OUT)/framegenadaptivepresenttest $(OUT)/fpsadaptivetest $(OUT)/presentretiretest
	rm -f $(OUT)/framegenadaptiveslottest $(OUT)/adaptiveslotclocktest $(OUT)/framegenqcompreparedtest $(OUT)/framegenadaptivepublicationtest $(OUT)/framegenadaptivelatchtest
	rm -f $(OUT)/vkframegenpreparedtest
	rm -f $(OUT)/screenclassifytest
	rm -f $(OUT)/vknativeuitest $(OUT)/vkreplaypintest
	rm -f $(OUT)/texelregiontest
	rm -f $(OUT)/vkcanvastest
	rm -f $(OUT)/poolfintest
	rm -f $(VKTEST_FRONTEND) $(VKTEST_FRONTEND:.o=.d)
.PHONY: all clean vkprobe vktest vktest-app threadplacetest frametimelinetest framefencetest acquireclocktest displaypacetest displayclocktest acquireslacktest predictpacetest predictbudgettest adrenogmemtest fpsautotest fpsautoscenetest fpsautoblocktest presentfencetest vktest-overlay vkposttest companiontest FORCE psotracetest
.PHONY: framegenslottest presentslottest vkframegentest framegenmathtest framegenbudgettest framegendeptest gpuitimingtest
.PHONY: fpshybridtest fps60patchtest
.PHONY: framegenadaptivetest framegenadaptivepresenttest fpsadaptivetest presentretiretest
.PHONY: framegenadaptiveslottest adaptiveslotclocktest framegenqcompreparedtest framegenadaptivepublicationtest framegenadaptivelatchtest
.PHONY: vkframegenpreparedtest
.PHONY: vktest-native-ui
.PHONY: poolfintest
.PHONY: vkpipetest screenclassifytest
.PHONY: vksubgrouptest

# The on-device setup library (libtotk_setup.so, the hactool executable libhactool.so, host totk-setup): targets
# setup-android / setup-host; it needs HACTOOL_SRC and the capstone prefixes from the setup deps helpers
# (android/setup/*/deps.py, called by build_release.py and build_android.py). Included last so `all` stays the default goal.
include $(TOTK)/android/setup/setup.mk
