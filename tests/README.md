# Host behavior tests

These tests exercise results and data contracts, not source-file layout.
They need CMake, a C++20 compiler, Vulkan headers, uv, and FFmpeg. Python checks run through `uv run`.

```sh
cmake -S tests -B tmp/host-tests -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build tmp/host-tests --parallel 4
ctest --test-dir tmp/host-tests --output-on-failure
```

DNG capture round trips check explicit UTC offsets and millisecond subseconds,
including known zero values, in all three Exif date groups.

For memory checks, configure a separate build with `-DRAWR_SANITIZERS=ON`.
Native libraries also retain their CMake test targets and reference fixtures;
GPU tests require a suitable Vulkan implementation. Kotlin tests run via Gradle.
