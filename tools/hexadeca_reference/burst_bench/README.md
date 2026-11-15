# CPU upload / GPU conversion overlap benchmark

Standalone only: no camera, app, merge, or lens-option wiring. Tests the Java-style case of three CPU-resident 200MP frames becoming three GPU-resident 50MP Bayer images via the existing Detail path, optionally with phase fit.

Two persistently mapped HOST_VISIBLE | HOST_COHERENT storage buffers form an upload ring. CPU memcpy for the next frame runs while the prior GPU submission executes. No transfer queue or dedicated GPU DMA upload is claimed: the GPU reads the mapped source directly on this unified-memory device. A fence guards each source buffer, descriptor set and command buffer before reuse. Three separate destination images retain all burst outputs. One ordered compute queue serializes the remosaicker's shared scratch; scratch must not resize during an in-flight burst.

For an apples-to-apples comparison, sequential uses the same allocations but waits immediately after every submission. Overlap waits only when reusing a ring slot and at burst completion. Timer covers first source memcpy through completion of all three Bayer images, including command recording/submission and fence waits. File IO, input preparation, validation readback and merging are outside timing. Allocation is before timing, but first record may allocate scratch; warm-up is excluded.

The three input frames are distinct synthetic variants of the supplied t200 RAW (offsets 0, +7, +14 DN, clamped to 10-bit). They exercise resource lifetime and ordering; they are NOT a real temporal camera burst or a merge-quality test. Every output in every burst is compared byte-for-byte against the first sequential run, including all borders. This validates scheduling equivalence, not independent demosaic quality; the existing CPU/GPU tests cover that.

## Build/run

```
cmake -S tools/hexadeca_reference/burst_bench -B tmp/quad-overlap/android -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
  -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release -DGLSLC=/path/to/host/glslc
cmake --build tmp/quad-overlap/android
adb push tmp/quad-overlap/android/quad_burst_bench /data/local/tmp/rawr-quad-bench/
# Input: unpacked little-endian RAW16, 10-bit samples, tight rows.
adb shell '/data/local/tmp/rawr-quad-bench/quad_burst_bench /data/local/tmp/rawr-quad-bench/input.raw16 16320 12288 3 1 3'
```

Arguments: input, width, height, CFA order, phase-fit flag, measured rounds. Set fit to 0 for plain Detail. Each round tests both schedules, alternating order; round 0 is the warm-up pair.

## Phone result (2026-11-15)

V2562, Adreno 840, three 16320x12288 CPU frames. Median of three measured rounds after warm-up:

| Pipeline | Sequential burst | Overlapped burst | Reduction |
|---|---:|---:|---:|
| Detail |409.6ms (393.8–428.6)|331.1ms (322.4–351.1)|19.2%|
| Detail + phase fit |551.0ms (543.8–570.0)|508.5ms (498.9–512.4)|7.7%|

All 48 output checks (16 bursts × 3 frames, including warm-up) are byte-identical to sequential. Output is Bayer ready on GPU; no RGB demosaic or merge is included.

Persistent warm upload buffers matter: copy medians summed across three frames were70–111ms without fit,75–79ms with fit. This differs from earlier fresh-buffer map/clear/copy timings and the quoted Xiaomi measurement. Do not combine these results with the earlier0.7s transfer figure. First sequential bursts were675ms without fit and1382ms with fit; they include first-use costs and are not directly comparable cold benchmarks. Test order, clock state, cache residency and shared bandwidth affect these short measurements. No sustained thermal or actual Camera2 ingress claim.

Memory tradeoff: two full source buffers =802,160,640 logical bytes, three retained output images =300,810,240 logical bytes, plus remosaic scratch. Harness additionally holds three CPU inputs, a readback buffer, and three reference outputs. This is not measured app peak memory and must be budgeted before integration. The harness keeps three outputs for future merge, but does not implement merging.

Logs and machine-readable summary: tmp/quad-overlap/phone-{fit,no-fit}.log and results.json.
