"""Execute the production Android startup ordering with host filesystem/config hooks.

The hooks observe worker/driver-setup boundaries only; they do not emulate a
working GPU, Android framework or Vulkan driver. Actual KGSL is replaced by an
explicit deterministic chip observation, as in the pure GMEM decision fixture.
"""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class AndroidDriverStartup(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get('CC', 'cc'))
        if not compiler or not shutil.which(compiler[0]):
            raise unittest.SkipTest('A native C compiler is required')
        scratch = Path(os.environ.get('TMPDIR', ROOT / '.scratch'))
        scratch.mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='android-driver-startup-', dir=scratch)
        cls.addClassCleanup(cls.temp.cleanup)
        cls.directory = Path(cls.temp.name)
        source = (ROOT / 'games/totk/runtime/frontend_android.c').read_text()
        options = source[source.index('/* ---------- options ---------- */'):source.index('/* copy a folder recursively')]
        version = source[source.index('static int game_version('):source.index('/* Minimal arm64 KGSL ABI')]
        # Execute APK asset parsing, but not JNI, in this filesystem/order fixture.
        identity_begin = options.index('static void gpu_profile_identity(')
        identity_jni = options.index('    JNIEnv *env', identity_begin)
        identity_end = options.index('static int is_dir(', identity_begin)
        options = (options[:identity_jni] +
                   '    vk_gpuprof_identity(commit, package, 0, build);\n}\n' + options[identity_end:])
        driver = source[source.index('static int android_native_lib_dir('):source.index('/* ---------- the game ---------- */')]
        native = source[source.index('static void android_native_ui_mode_policy('):source.index('static int start_game(')]
        startup = source[source.index('static int start_game('):source.index('/* ---------- input ----------')]
        constructor_guard = '#if !defined(__ANDROID__) || !defined(TOTK_ANDROID_APP)'
        constructors = ''
        for path in ('input.c', 'threads.c'):
            text = (ROOT / 'games/totk/runtime' / path).read_text()
            begin = text.rindex(constructor_guard)
            constructors += text[begin:text.index('#endif', begin) + len('#endif')] + '\n'
        probe = cls.directory / 'probe.c'
        probe.write_text(r'''#define _GNU_SOURCE
#define __ANDROID__ 1
#define TOTK_ANDROID_APP 1
#define TOTK_BUILD_KIND "host-order"
#include <assert.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "adreno_gmem.h"
typedef struct {
    const char *externalDataPath, *internalDataPath;
    int sdkVersion;
    const char *assetManager;
} ANativeActivity;
typedef FILE AAsset; /* Host fixture reads the test APK asset from an ordinary, bounded file. */
enum { AASSET_MODE_STREAMING = 2, AASSET_MODE_BUFFER = 3 };
enum { ANDROID_LOG_INFO = 4, ANDROID_LOG_WARN = 5 };
static int __android_log_print(int level, const char *tag, const char *format, ...) {
    (void)level; (void)tag; (void)format; return 0;
}
static AAsset *AAssetManager_open(const char *root, const char *name, int mode) {
    (void)mode; char path[4096]; snprintf(path, sizeof path, "%s/assets/%s", root, name); return fopen(path, "rb");
}
static off_t AAsset_getLength(AAsset *asset) {
    off_t at = ftello(asset); assert(!fseeko(asset, 0, SEEK_END));
    off_t size = ftello(asset); assert(!fseeko(asset, at, SEEK_SET)); return size;
}
static int AAsset_read(AAsset *asset, void *data, size_t size) { return (int)fread(data, 1, size, asset); }
static void AAsset_close(AAsset *asset) { assert(!fclose(asset)); }
static char g_data[1024], g_rewritten[1200];
static const char *expected;
static unsigned phase, probes;
static uint32_t observed_chip;
static int probe_success;
static void check_environment(void) {
    const char *value = getenv("TU_DEBUG");
    assert(expected);
    assert(!strcmp(expected, "<unset>") ? value == NULL : value && !strcmp(value, expected));
}
static bool android_kgsl_chip_probe(void *context, uint32_t *chip) {
    (void)context; assert(phase == 1); probes++;
    *chip = observed_chip; return probe_success;
}
static void log_start(void) { assert(phase == 0); phase = 1; }
static void identity_sink(const char *commit, const char *package, uint64_t version, const char *build) {
    (void)package; (void)version; (void)build; check_environment(); assert(phase == 1);
    printf("PROFILE_COMMIT=%s\n", commit);
}
static void (*vk_gpuprof_identity)(const char *, const char *, uint64_t, const char *) = identity_sink;
static void log_mirror_start(void) { assert(phase == 1); }   /* after the log, before any startup boundary */
static void live_ctl_start(void) { assert(phase == 1); }     /* the live-switch reader: after totk.env, before the game */
static void thread_sampler_start(void) { assert(phase == 1); }   /* TOTK_THREAD_SAMPLER's launch value: after totk.env */
static void totk_thread_game_startup(void) { check_environment(); assert(phase == 1); phase = 2; }
static int copy_tree(const char *src, const char *dst) { (void)src; (void)dst; assert(0); return -1; }
static int runtime_handheld_mode(void) { return 1; }
static void runtime_input_diag_start(void) { check_environment(); assert(phase == 3); phase = 4; }
static void runtime_autoload_start(void) { check_environment(); assert(phase == 4); }   /* after the environment files */
static void runtime_thread_diag_start(void) { check_environment(); assert(phase == 4); phase = 5; }
static void runtime_android_feedback_hardware(ANativeActivity *a) { (void)a; check_environment(); assert(phase == 5); }
static void vk_shader_seed_startup(const char *game_dir) {
    (void)game_dir; check_environment(); assert(phase == 5); phase = 6;
    assert(getenv("TOTK_SHADER_SEED_DIR"));
}
static void *game_thread(void *arg) { (void)arg; assert(0); return NULL; }
static int create_hook(pthread_t *thread, const pthread_attr_t *attr,
                       void *(*entry)(void *), void *arg) {
    (void)attr; (void)arg; check_environment(); assert(phase == 6);
    assert(entry == game_thread); *thread = 0; phase = 7; return 0;
}
static int detach_hook(pthread_t thread) { (void)thread; assert(phase == 7); return 0; }
''' + options + version + '''
#define driver_setup production_driver_setup
''' + driver + '''
#undef driver_setup
static void driver_setup(const char *private_dir) {
    check_environment(); assert(phase == 2); production_driver_setup(private_dir); phase = 3;
}
''' + native + '''
static int android_all_files_access(const ANativeActivity *a) { (void)a; return 0; }
static void android_close_requests_start(const char *private_dir) { (void)private_dir; }
#define pthread_create create_hook
#define pthread_detach detach_hook
''' + startup + constructors + r'''
int main(int argc, char **argv) {
    assert(argc == 6);
    expected = argv[3]; observed_chip = (uint32_t)strtoul(argv[4], NULL, 16);
    probe_success = atoi(argv[5]);
    ANativeActivity activity = {argv[1], argv[2], 36, argv[1]};
    assert(start_game(&activity)); assert(phase == 7); check_environment();
    printf("ORDER=affinity,driver,input,thread-diag,seed,game probes=%u TU_DEBUG=%s\n",
           probes, getenv("TU_DEBUG") ? getenv("TU_DEBUG") : "<unset>");
    printf("PSO_KEYS=%s\n", getenv("TOTK_PSO_KEYS") ? getenv("TOTK_PSO_KEYS") : "<unset>");
    static const char *const shown[] = {"TOTK_FPS60", "TOTK_RENDER_RES", "TOTK_OVERLAY", "TOTK_SHADOW_RES",
        "TOTK_ACQUIRE_SLACK", "TOTK_PRESENT_BUFFERS", "TOTK_FRAME_TIMELINE", "TOTK_VK_REC_THREADS"};
    for (size_t i = 0; i < sizeof shown / sizeof *shown; i++)
        printf("ENV %s=%s\n", shown[i], getenv(shown[i]) ? getenv(shown[i]) : "<unset>");
    return 0;
}
''')
        cls.probe = cls.directory / 'probe'
        flags = [flag for name in ('CPPFLAGS', 'CFLAGS', 'LDFLAGS')
                 for flag in shlex.split(os.environ.get(name, ''))]
        result = subprocess.run([*compiler, '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                                 '-Wno-format', *flags, '-pthread', '-I',
                                 str(ROOT / 'games/totk/runtime'), str(probe), '-o', str(cls.probe)],
                                capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        cls.source = source

    def startup(self, lines='', driver='default', chip='43020100', success=True,
                expected='sysmem', inherited=None, asset=None, stale_asset=False, install='{"format":1,"playable":true}', refused=False,
                settings=None, build_asset=None):
        with tempfile.TemporaryDirectory(prefix='case-', dir=self.directory) as path:
            case = Path(path)
            external, private = case / 'game', case / 'private'
            (external / 'rewritten/v143').mkdir(parents=True)
            (external / 'shaders/v143').mkdir(parents=True)
            private.mkdir()
            if settings is not None:
                (private / 'settings.env').write_text(settings)
            if asset is not None:
                (external / 'assets').mkdir()
                (external / 'assets/pso_keys.bin').write_bytes(asset)
            if build_asset is not None:
                (external / 'assets').mkdir(exist_ok=True)
                (external / 'assets/build.txt').write_text(build_asset)
            if stale_asset:
                (private / 'state').mkdir()
                (private / 'state/pso_keys.bin').write_bytes(b'stale list from another APK')
            if driver == 'default':
                (external / 'driver.so').write_bytes(b'host ordering fixture; never loaded')
            elif driver == 'relative':
                (external / 'selected.so').write_bytes(b'host ordering fixture; never loaded')
                lines += 'TOTK_VK_DRIVER=selected.so\n'
            elif driver == 'absolute':
                selected = case / 'selected.so'
                selected.write_bytes(b'host ordering fixture; never loaded')
                lines += f'TOTK_VK_DRIVER={selected}\n'
            elif driver == 'system':
                (external / 'driver.so').write_bytes(b'host ordering fixture; never loaded')
                lines += 'TOTK_VK_DRIVER=system\n'
            elif driver == 'missing-explicit':
                lines += 'TOTK_VK_DRIVER=missing.so\n'
            elif driver != 'missing':
                raise ValueError(driver)
            (external / 'totk.env').write_text(lines)
            if install is not None:
                (external / 'install.json').write_text(install)
            env = {key: value for key, value in os.environ.items()
                   if not key.startswith(('TOTK_', 'DM_')) and key != 'TU_DEBUG'}
            env.update(inherited or {})
            result = subprocess.run([str(self.probe), str(external), str(private), expected,
                                     chip, str(int(success))], env=env, cwd=case,
                                    capture_output=True, text=True)
            if refused:
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn('ORDER=', result.stdout)
                return result.stdout, result.stderr
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('ORDER=affinity,driver,input,thread-diag,seed,game', result.stdout)
            if asset is not None and len(asset) >= 64 and 'TOTK_PSO_KEYS=' not in lines and 'TOTK_PSO_PREWARM=0' not in lines:
                self.assertEqual((private / 'state/pso_keys.bin').read_bytes(), asset)
                self.assertIn('PSO_KEYS=' + str(private / 'state/pso_keys.bin'), result.stdout)
            return result.stdout, result.stderr

    def test_profile_identity_accepts_packaged_short_commit_and_rejects_untrusted_text(self):
        for commit in ('abcdef012345', 'a' * 40, 'b' * 64):
            with self.subTest(commit=commit):
                output, _ = self.startup(lines='TOTK_VK_GPU_PROFILE=1\n', build_asset=f'commit {commit}\n')
                self.assertIn(f'PROFILE_COMMIT={commit}\n', output)
        for value in ('commit abcdef012345-dirty\n', 'commit not-a-commit\n', 'commit abcdef012345 extra\n'):
            output, _ = self.startup(lines='TOTK_VK_GPU_PROFILE=1\n', build_asset=value)
            self.assertIn('PROFILE_COMMIT=\n', output)
        output, _ = self.startup(build_asset='commit abcdef012345\n')
        self.assertNotIn('PROFILE_COMMIT=', output)

    def test_optional_pipeline_key_asset(self):
        output, _ = self.startup(stale_asset=True)
        self.assertIn('PSO_KEYS=<unset>', output)
        self.startup(asset=b'TOTKPS01' + bytes(56))
        output, _ = self.startup(asset=b'short')
        self.assertIn('PSO_KEYS=<unset>', output)
        output, _ = self.startup(asset=b'TOTKPS01' + bytes(56), lines='TOTK_PSO_PREWARM=0\n')
        self.assertIn('PSO_KEYS=<unset>', output)
        output, _ = self.startup(asset=b'TOTKPS01' + bytes(56), lines='TOTK_PSO_KEYS=/explicit/list.bin\n')
        self.assertIn('PSO_KEYS=/explicit/list.bin', output)

    def test_automatic_default_precedes_every_startup_boundary(self):
        for driver in ('default', 'relative', 'absolute'):
            with self.subTest(driver=driver):
                output, log = self.startup(driver=driver)
                self.assertIn('probes=1 TU_DEBUG=sysmem', output)
                self.assertEqual(log.count('Adreno GMEM workaround:'), 1)
                self.assertLess(log.index('Adreno GMEM workaround:'), log.index('Vulkan driver:'))

    def test_file_and_inherited_overrides_keep_precedence(self):
        for value in ('', '0', 'other', 'sysmem'):
            with self.subTest(value=value):
                output, log = self.startup(lines=f'TU_DEBUG={value}\n', expected=value,
                                           inherited={'TU_DEBUG': 'inherited'})
                self.assertIn(f'probes=0 TU_DEBUG={value}', output)
                self.assertNotIn('Adreno GMEM workaround:', log)
                output, _ = self.startup(expected=value, inherited={'TU_DEBUG': value})
                self.assertIn('probes=0', output)

    def test_other_missing_system_and_failed_probes_do_not_default(self):
        for driver in ('system', 'missing', 'missing-explicit'):
            with self.subTest(driver=driver):
                output, log = self.startup(driver=driver, expected='<unset>')
                self.assertIn('probes=0 TU_DEBUG=<unset>', output)
                self.assertNotIn('Adreno GMEM workaround:', log)
        for chip, success in (('43020101', True), ('0', True), ('43020100', False)):
            with self.subTest(chip=chip, success=success):
                output, log = self.startup(chip=chip, success=success, expected='<unset>')
                self.assertIn('probes=1 TU_DEBUG=<unset>', output)
                self.assertNotIn('Adreno GMEM workaround:', log)

    def test_inherited_diagnostics_cannot_start_from_android_constructors(self):
        output, _ = self.startup(inherited={'TOTK_INPUT_SCRIPT': '1:1:1',
                                           'TOTK_INPUT_FILE': '/never/opened/by/order/hooks',
                                           'TOTK_LISTCHECK': '1', 'TOTK_DUMP_AFTER': '1'})
        self.assertIn('probes=1 TU_DEBUG=sysmem', output)

    def test_unfinished_or_missing_setup_refuses_before_anything_starts(self):
        for install, why in ((None, 'not set up'), ('{"format":1,"playable":false}', 'setup not finished'),
                             ('{"format":2,"playable":true}', 'another format')):
            with self.subTest(install=install):
                _, log = self.startup(install=install, refused=True)
                self.assertIn(why, log)
                self.assertNotIn('Vulkan driver:', log)

    def test_bundled_turnip_beside_the_library_is_used_in_place(self):
        bundled = self.directory / 'libtotk_turnip.so'   # dladdr: the probe stands in for libtotk.so
        bundled.write_bytes(b'host ordering fixture; never loaded')
        self.addCleanup(bundled.unlink)
        output, log = self.startup(driver='missing')
        self.assertIn('probes=1 TU_DEBUG=sysmem', output)   # the A722 default applies to the bundled Turnip too
        self.assertIn(f'Vulkan driver: {bundled} (bundled Turnip)', log)
        _, log = self.startup(driver='default')             # the player's own driver.so still wins
        self.assertIn('(copied from', log)
        _, log = self.startup(driver='system', expected='<unset>')
        self.assertNotIn('bundled Turnip', log)

    def test_seed_pack_selects_indexed_seed_io_and_game_data_default(self):
        self.assertIn('seed.vkpack", g_data, version); if (is_file(p)) env_default("TOTK_VK_SEED_IO"', self.source)
        self.assertIn('env_default("TOTK_GAME_DATA", "%s", p)', self.source)

    def test_decision_is_immediate_after_env_file_not_driver_copy(self):
        start = self.source.index('static int start_game(')
        after_file = self.source.index('env_file(p);', start) + len('env_file(p);')
        self.assertTrue(self.source[after_file:].lstrip().startswith('android_driver_env_policy();'))
        driver = self.source[self.source.index('static void driver_setup('):
                             self.source.index('/* ---------- the game ---------- */')]
        self.assertNotIn('setenv("TU_DEBUG"', driver)

    def test_menu_settings_apply_before_totk_env_and_only_from_the_allowlist(self):
        settings = ('# Generated by the TotK Native menu\n'
                    'TOTK_FPS60=0\nTOTK_RENDER_RES=960x540\nTOTK_SHADOW_RES=288\nTOTK_OVERLAY=fps\n'
                    'TOTK_VK_REC_THREADS=1\nTU_DEBUG=nobin\nnot a line\n')
        output, log = self.startup(settings=settings, lines='TOTK_OVERLAY=full\n')
        for value in ('TOTK_FPS60=0', 'TOTK_RENDER_RES=960x540', 'TOTK_SHADOW_RES=288'):
            self.assertIn('ENV ' + value, output)
        self.assertIn('ENV TOTK_OVERLAY=full', output)                 # the developer's totk.env wins
        self.assertIn('ENV TOTK_VK_REC_THREADS=<unset>', output)       # not a menu option
        self.assertIn('probes=1 TU_DEBUG=sysmem', output)              # nor are driver variables
        self.assertIn('[totk-app] settings.env (launcher menu; totk.env overrides): TOTK_FPS60=0 '
                      'TOTK_RENDER_RES=960x540 TOTK_SHADOW_RES=288 TOTK_OVERLAY=fps\n', log)
        self.assertEqual(log.count('[totk-app] settings.env: ignored TOTK_VK_REC_THREADS (not a launcher menu option)'), 1)
        self.assertEqual(log.count('[totk-app] settings.env: ignored TU_DEBUG (not a launcher menu option)'), 1)
        self.assertIn('[totk-app] settings.env: ignored line "not a line"', log)
        self.assertLess(log.index('settings.env (launcher menu'), log.index('totk.env: TOTK_OVERLAY=full'))

    def test_absent_settings_file_keeps_shipping_defaults(self):
        output, log = self.startup()
        self.assertNotIn('settings.env', log)
        self.assertIn('ENV TOTK_FPS60=0', output)
        self.assertIn('ENV TOTK_ACQUIRE_SLACK=1', output)
        self.assertIn('ENV TOTK_RENDER_RES=<unset>', output)

    def test_frame_generation_timing_block_is_allowed(self):
        block = ('TOTK_FPS60=0 TOTK_FRAMEGEN=locked TOTK_REALTIME=1 TOTK_FRAME_LAG=0 TOTK_FPS_AUTO=0 DM_PRESENT_HZ=60 '
                 'DM_PRESENT_INTERVAL=game TOTK_PRESENT_SLOTS=1 TOTK_VK_PRESENT=ahb TOTK_ACQUIRE_SLACK=0 '
                 'TOTK_ACQUIRE_CLOCK=0 TOTK_PACE=0 TOTK_ASYNC_REPLAY=0').split()
        _, log = self.startup(settings='\n'.join(block) + '\n')
        self.assertNotIn('ignored', log)
        self.assertIn('settings.env (launcher menu; totk.env overrides): ' + ' '.join(block) + '\n', log)

    def test_acquire_slack_two_no_longer_enables_the_frame_timeline(self):
        output, _ = self.startup(settings='TOTK_ACQUIRE_SLACK=2\n')
        self.assertIn('ENV TOTK_ACQUIRE_SLACK=2', output)
        self.assertIn('ENV TOTK_PRESENT_BUFFERS=4', output)
        self.assertIn('ENV TOTK_FRAME_TIMELINE=<unset>', output)
        output, _ = self.startup(lines='TOTK_ACQUIRE_SLACK=predict\n')
        self.assertIn('ENV TOTK_FRAME_TIMELINE=1', output)
        output, _ = self.startup(lines='TOTK_ACQUIRE_SLACK=2\nTOTK_FRAME_TIMELINE=1\n')
        self.assertIn('ENV TOTK_FRAME_TIMELINE=1', output)
