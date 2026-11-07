# Building from source

Players do not need to: install the release APK ([android-install.md](android-install.md)). The release APK is built
from this repository alone; no game file, key or anything derived from them is read or packed.

## The release APK

```sh
python3 games/totk/android/build_release.py --ndk <Android NDK r28> --turnip <Turnip driver.so> \
  --key <your signing key .p12> --key-pass-env <VAR> -o totk-native.apk
```

- **Prerequisites** (Linux x86-64): Python 3.12+ with `cryptography` (`pip install cryptography==50.0.2`), git, make,
  cmake, javac 17+, the Android SDK (platform 33+ and build-tools), the NDK r28 (28.2.13676358), network access for the
  pinned sources (opus, libvpx, hactool, capstone, zstd, libwebp), and for the shader translator either the .NET 10
  SDK or a prebuilt translator passed with `--translator`. The full list, the work folder and what each step does:
  [games/totk/android/README.md, *Building the public APK*](../games/totk/android/README.md#building-the-public-apk).
- **Signing key**: yours, never generated or stored by the build (PEM or PKCS#12). Its password goes in the environment
  variable named by `--key-pass-env`, never on the command line. Every later release must be signed with the same key,
  or Android refuses the update. Release APKs of this project are signed with one release key (certificate SHA-256
  `a9e7a2a9fad6fdaede18c45a458e4555c0904658c9d7f4b74a73d9c83b67e2bf`).
- **Turnip**: the only prebuilt binary in the APK besides the open fonts. Its reproducible recipe (Mesa at a pinned
  commit plus the patches in this repository) is [games/totk/android/turnip/](../games/totk/android/turnip/README.md);
  the build accepts only the pinned driver (sha256 `9b87afeb3d8d30c6a0ad5aab3e0703bc1500e0321e9f17a922106fa556389ec6`).
- **Shader translator**: [games/totk/tools/totk_xlate/](../games/totk/tools/totk_xlate/) (Ryujinx's shader translator,
  published with NativeAOT for Android).
- **Audit**: the build signs the APK and audits its contents (`games/totk/android/apk/audit_apk.py`); anything not on
  the allowlist, or anything that looks like game data, removes the APK and stops the build.
  `build_release.py --audit <apk>` re-runs only the audit.

## Release checklist

- Build from a clean committed checkout with the release key; keep the APK's commit and SHA-256 with the test results.
- Publish a **full release, not a pre-release**, tagged **`vX.Y.Z`**, so GitHub's `releases/latest` endpoint sees it.
  Keep the exact asset names **`TotK-Native-<ver>.apk`** and **`TotK-Native-<ver>.apk.sha256`**; automatic updates
  derive these names. The checksum file must be `sha256sum` format: `<64 hex>  TotK-Native-<ver>.apk`.
  Keep versionName `X.Y.Z` and a strictly increasing versionCode; names containing `-` disable automatic checks.
- **Test with SELinux enforcing**, not just on rooted permissive devices: decrypted setup to private storage, game
  startup/save loading and gameplay, Saves Import/Export, and container setup into picked internal shared storage and
  an SD card when present. Use the actual Settings UI for All files access. Follow
  [SELinux compatibility](selinux-compatibility.md) for denial attribution, evidence, and cleanup.
- Preserve the installed player's app and saves; use a separate test package for fresh setup, then remove test inputs
  and keys and verify the original saves. Restore any explicitly authorized temporary SELinux-mode change.

## App id and versions

- The app id is `com.aure.totk` (`games/totk/android/apk/build_apk.py` `PACKAGE`). Earlier test builds used
  `com.bernalr.totk`, a separate app on the device ([android-install.md](android-install.md#where-the-game-data-lives-internal-storage-sd-card-or-a-folder-you-choose)
  explains moving saves).
- The app version follows semantic versioning; the version code is major × 10000 + minor × 100 + patch (1.0.0 is
  10000). Changes per version: [CHANGELOG.md](../CHANGELOG.md).
- Test builds signed with another key cannot be updated in place by a release build: export the saves, uninstall,
  install the release and import them.

## Developer build and tests

- The developer build from your own game files on a Linux PC (`games/totk/android/build_android.py`), running the game
  over `adb shell`, and the fixture tests without game data:
  [games/totk/android/README.md](../games/totk/android/README.md).
- The app, its menu, setup and companion in depth: [games/totk/android/APP.md](../games/totk/android/APP.md); the
  Vulkan backend: [games/totk/android/VULKAN.md](../games/totk/android/VULKAN.md); debugging and diagnostic switches:
  [games/totk/android/DEBUGGING.md](../games/totk/android/DEBUGGING.md).
- Tests:

  ```sh
  python -m unittest discover -s tests
  python -m unittest discover -s tests/setup -t tests/setup
  python3 tools/bench/check_baselines.py
  ```

- The macOS path inherited from MacNativeGames (`build_app.py`) is kept: it shares the runtime, and several tests
  compile against it ([how-it-works.md](how-it-works.md)).
- Measuring performance and checking a change against the recorded baselines: [benchmarks/](benchmarks/) and
  `tools/bench/README.md`.
