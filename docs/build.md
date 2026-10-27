# Build and manual release

## Toolchain

macOS and Linux: JDK 17, Gradle wrapper 8.11.1, AGP 8.10.1, Kotlin/Compose
compiler 2.2.21, SDK 36, NDK 29.0.14206865, SDK CMake 3.31.6.
Install `uv` for Python build helpers and tools.
Use `ANDROID_HOME` or an untracked `local.properties` containing `sdk.dir`.
First builds require network access for Maven dependencies and pinned
libadrenotools sources. Termux and native Windows builds are not supported.

The app and standalone runners use the pinned SDK tools. Host Vulkan tests
additionally need Vulkan headers and a loader; GPU tests need a working Vulkan
implementation (MoltenVK on macOS). `VULKAN_INCLUDE` can select host headers.
Standalone Python tools requiring packages should run through `uv run --with`.

## Release APK

```sh
./gradlew :app:assembleRelease :app:testReleaseUnitTest :app:lintRelease
```

With no credentials, output is `app/build/outputs/apk/release/app-release-unsigned.apk`.
For private signing, create a key outside the checkout; keytool prompts for passwords:

```sh
keytool -genkeypair -keystore /private/path/rawr-release.jks -alias rawr \
  -keyalg RSA -keysize 4096 -validity 10000
```

Back up the keystore and credentials securely. Supply all four environment variables:
`RAWR_SIGNING_STORE_FILE`, `RAWR_SIGNING_STORE_PASSWORD`, `RAWR_SIGNING_KEY_ALIAS`,
and `RAWR_SIGNING_KEY_PASSWORD`. Alternatively use `rawr.signing.STORE_FILE`,
`rawr.signing.STORE_PASSWORD`, `rawr.signing.KEY_ALIAS`, and `rawr.signing.KEY_PASSWORD`
in your external Gradle user properties. Do not put credentials in command-line
arguments or repository files. Partial configuration fails explicitly.

Rebuild to produce `app-release.apk`. Verify with the SDK's `apksigner verify
--verbose --print-certs` and `uv run --no-project tools/release/release_artifact_check.py <apk>`.
The check verifies non-debuggable packaging, arm64 libraries, alignment, and
memory-mapped assets, and prints APK/DEX sizes. Preserve release mapping and
native symbol outputs with your private release records.

A private key cannot update the old debug-signed `com.rawr.camera` installation;
export needed files and reinstall. Future updates must retain the private key
and increase `versionCode`. Publishing is manual.

## Local tooling

Debug APKs use `debug-<short commit SHA>` as their version name, derived from
the checked-out Git commit in CI and local builds. Settings > About and native
photo metadata use the same version. Uncommitted edits do not change this SHA.
Release versions continue to use `versionName` and `versionCode` in `app/build.gradle.kts`.

Use Gradle directly for APK builds and `adb install -r <apk>` for installation.
[Offline replay](replay.md) has one launcher: `uv run --no-project tools/replay.py`.
[Host behavior tests](../tests/README.md) build together with CMake/CTest.

Generated review images, logs, environments, and captures belong under `tmp/`.
Native build trees are ignored. Gradle copies the canonical film asset from
`native/spektrafilm/assets` into generated APK assets. Committed binary fixtures are intentional test
inputs; generated source tables and film assets are required build inputs.
