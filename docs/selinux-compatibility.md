# SELinux compatibility

TotK Native must work as an ordinary Android app with **SELinux enforcing**. Root, Magisk, a custom SELinux policy,
`setenforce 0`, and `appops` overrides are not installation prerequisites. Root used by the benchmark tools is for the
measurement harness, not a permission the game or setup process should need.

## What the app needs

- Read the player's selected dump and keys through Storage Access Framework grants, or direct file access when the
  player has enabled **All files access** in Android Settings. The optional permission does not bypass SELinux and
  does not grant access to other apps' private directories.
- Read and write ordinary files in its private storage, its app-specific SD-card directory, or a supported folder
  selected by the player. A picked game-data folder needs All files access; a document-provider grant alone is not a
  general-purpose native filesystem path. Saves use the app's private working copy and the app's Import/Export UI.
- Run the bundled `libhactool.so` and `libtotk_xlate.so` helper executables from the APK's installed native-library
  directory. Do not copy executables into writable game data, cache, Download, or an SD card and execute them there.
  Helper input descriptors are inherited; their pipes do not require a shared-storage executable or shell script.
- Create the container merge's temporary FIFO in app-private `noBackupFilesDir/setup-work`, with mode `0600`, and
  remove it after use. Only ordinary RomFS output files belong on the selected game-data volume. Shared-storage
  FUSE mounts and removable filesystems do not provide a portable place for FIFOs, regardless of free space or All
  files access. A filesystem rejection may have **no AVC denial**; it is still a compatibility failure.
- Load guest modules into anonymous memory and seal code pages read/execute after relocation. This is distinct from
  executing a file from writable app storage. The runtime reads its own `/proc/self` mappings for diagnostics and
  storage mounts; it must not depend on access to another process's private state.

The FIFO placement fix is in `games/totk/android/setup/containers.c`; helper spawning is in `setup/hactool_run.c` and
`setup/seed/translate.c`. Runtime misses use `gfx/vk_shader_xlate.c`. Keep their executable paths tied to the installed
APK rather than introducing a writable-directory fallback. Preserve the original `errno` in filesystem diagnostics
before cleanup, so a permission failure is not reported as a different error.

## Odin 3 audit, 4 November 2026

This is a bounded observation, **not an enforcing-mode pass**. The Android 15 build fingerprint was
`qti/sun/sun:15/AQ3A.250728.001/eng.Odin3.20260204.171202:user/dev-keys`. The clean source build was
`e654bcfcf549`, including the private-FIFO fix, repacked as the separate release-signed `com.aure.totk.test`
application. Its APK SHA-256 was `a321ba3a9835647e8707e93386857aafb072d17c11918ec7579103c9afea9512`.

| Flow | Permissive result | Enforcing result |
| --- | --- | --- |
| Decrypted dump → private internal storage, including shaders/companion | Completed; 24,783 shaders ready | Not exercised |
| Start game → load Hateno Village → 60 seconds of camera movement in gameplay | Completed | Not exercised |
| Saves Export / Import through document pickers | Export contained 32 save payloads; import completed and made a backup | Not exercised |
| Base + update + keys → picked internal shared folder, All files access granted in Settings | Completed; container-route manifest marked all stages done | Not exercised |
| Base + update + keys → SD (RP5), `/storage/AAAA-AAAA` | Completed; same verified container-route install manifest | Not exercised |

The test app's observed source context was `u:r:untrusted_app_32:s0:c175,c257,c512,c768`. After deduplicating logcat's
two copies and dmesg by audit event number, 16 delivered app-context AVC events had only these two signatures:

| Target context | Class / permission | Processes / target | Classification |
| --- | --- | --- | --- |
| `u:object_r:shell_test_data_file:s0` | `dir` / `search` | `libhactool.so`, `libtotk_xlate.s`; directory named `tests` | Optional startup probe candidate; no app feature failed in permissive mode. Full path and enforcing outcome were not established. |
| `u:object_r:postinstall_mnt_dir:s0` | `dir` / `getattr` | Same helpers; `/postinstall` | Optional platform-linker probe candidate, not a required game-data path; enforcing outcome unverified. |

**[INFERENCE]** These identical helper-startup patterns are consistent with Android's dynamic-linker namespace
configuration, rather than a .NET-only or game-data dependency. The
[Android 15 linker implementation](https://android.googlesource.com/platform/bionic/+/refs/heads/android15-release/linker/linker_config.cpp?format=HTML)
explicitly tolerates failed path canonicalization, including `/postinstall`, and retains the unresolved path.
No syscall stack was captured to prove the callsite on this ROM. Do not add permissions or a policy exception for
these paths, and do not hide future helper failures on the strength of this classification.

No delivered app AVC concerned FIFO creation; both container destinations completed with the private-FIFO fix.
The kernel audit stream also reported rate limiting and lost records amid unrelated OEM/root-service denials, so
the delivered list is not proof that every denied operation was logged.

At device time **03:12:16** the authorized `setenforce 1` transition succeeded and `getenforce` returned `Enforcing`.
The test package was removed for a fresh installation, but before it could be reinstalled the OEM
`com.odin.gameassistant` entered a crash loop. Its `system_app` domain was denied setting
`persist.vendor.debug.mode` (`vendor_default_prop`, `property_service`, `set`) and finding `PServerBinder`
(`default_android_service`, `service_manager`, `find`). At **03:12:36.623** Android logged
`Rebooting, reason: RescueParty`; adb disconnected and TCP port 5555 stopped accepting connections. An attempted
`adb reboot` could not run because the device was offline. These are device/ROM failures, not denials attributed to
the test app. No application setup, gameplay, or saves flow was observed under enforcement.

This ROM cannot be treated as an enforcing acceptance device merely by toggling its global mode. Do not weaken its
policy, suppress the vendor crashes in app code, or claim stock-phone compatibility from its permissive results.

## Test on an enforcing device

Use a stock enforcing device for release acceptance where possible. A rooted permissive device is useful for finding
candidate denials, but a successful run there does **not** establish compatibility with stock phones. Never change
SELinux mode on a shared test device without explicit authorization and exclusive ownership of that device.

1. Record the APK commit/SHA-256, device build/Android version, storage volumes, and the output of `getenforce` (root
   may be needed to read it on some ROMs). Use a separately signed test application id if a fresh install is needed;
   never uninstall the player's real app or overwrite their saves. Export and hash the original saves first.
2. Clear `logcat -b all -c`, record the current dmesg position, and collect both logs for the whole run. Record the
   test app's UID and complete process SELinux context, including its MLS categories, from `ps -AZ`. The main and
   `:setup` processes and their children must be attributed to the test app, not just matched by a short `comm` name.
   Record audit rate-limit/lost-record warnings too; if the kernel dropped records, report that coverage limitation
   rather than calling a filtered list exhaustive.
3. On a permissive test device, first exercise all flows with logging only. Keep every matching
   `avc: denied ... permissive=1` record, including the full source/target context, class, permission set, path, and
   process name. Classify each as an unnecessary probe, a feature blocker (name the feature), or a known fixed path.
   System services, root tools, launchers and unrelated apps can produce many denials; do not count those as the app.
4. With SELinux **Enforcing**, repeat from fresh **test-package** data:
   - Decrypted dump to private internal storage, through the actual file picker and setup UI, including shader and
     companion preparation.
   - Start game, load a known save, and play for at least one minute.
   - Saves Export and Import through their document pickers; compare the exported file contents with the imported set.
   - Set up again using base container + update + keys into a picked folder on internal shared storage. Grant All
     files access using the Android Settings UI, not an `appops` command.
   - Repeat the container route on an SD card if one is present; otherwise record the skip and why.
5. For each failure, capture the visible screen, setup/runtime logs (`totk.log` where the game ran), and matching AVCs.
   Distinguish SELinux denials from missing grants, Unix permissions, unsupported filesystem operations, and input
   validation failures. A passing FIFO creation alone is not a full container/setup acceptance test.
6. Fix the denied operation rather than weakening the policy or suppressing its error, rebuild from a clean committed
   checkout, and repeat the affected flows with SELinux enforcing. Record flows which did not finish as unverified.
7. Restore the original SELinux mode and verify it, remove the test package and all pushed inputs (especially keys),
   exported archives and test folders, release any controller inputs, and restore device settings. Re-export the real
   app's saves and compare every payload hash with the original merged set before releasing the device.

For an explicitly authorized temporary enforcing test on a rooted device, the mode changes are `su -c 'setenforce 1'`
and, **after testing even when a flow fails**, `su -c 'setenforce 0'` when the device originally was permissive. Verify
with `su -c getenforce` after both changes. If enforcing prevents adb/root recovery, reboot the authorized test device
and verify its mode after reconnecting; do not leave it in an unknown state.

The controller, screenshots, save restoration and hash-comparison workflow is documented in
[the benchmark skill](../.omp/skills/totk-benchmark/SKILL.md). Device-specific test evidence should identify the exact
build and policy tested; one vendor's result is not proof for every Android policy.
