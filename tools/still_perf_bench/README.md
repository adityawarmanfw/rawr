# still_perf_bench

Host (MoltenVK) benchmark of the single-frame still path, built from the production native libraries in app
order:

| Stage | Code |
|---|---|
| RAW upload, clip pack + LSC (vignette) | `raw_demosaic/common/shaders/sensor_clip_pack.comp` |
| AMaZE | `native/raw_demosaic/amaze` |
| WB, highlight recovery, distortion, defringe, FCC | `native/post_demosaic` |
| Gain map | `native/gainmap` |
| Readback, UltraHDR fit + colour restore, strip JPEG (base and map) | `encoding/jpeg/UltraHdrMapFit.h`, `StripJpegEncoder` |

Each shot builds its engines and images fresh, as the app does, so `*.build` and the memory peaks include
allocation. Device memory is counted by interposing `vkAllocateMemory`/`vkFreeMemory`. The tone stage is not
run: the SDR base is a format-converting blit of the post-demosaic image, which keeps real texture for the
JPEG encoders.

```sh
cmake -S tools/still_perf_bench -B tmp/stillbench/build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C tmp/stillbench/build still_perf_bench
tmp/stillbench/build/still_perf_bench --cfa raw.u16 --width W --height H --black B --white WL --wb R 1 B \
  [--reps 3] [--fcc 2] [--defringe 1] [--hl 0|1|2] [--distortion 0|1] [--uhdr 1] [--full-res-map 1] \
  [--capture-color 1] [--slots 16] [--persistent 0] [--dump PREFIX]
```

`raw.u16` is the visible CFA as little-endian uint16 (for example `rawpy`'s `raw_image_visible`). The first
shot warms the driver and is dropped; the median over the rest is printed. Run one 50 MP job at a time (about
1 GB RSS and 1.6 GB of device memory).

`--dump PREFIX` writes the post-demosaic output, the final gain map and the fit inputs of the last shot, for
bit-identity checks between builds. `BENCH_KEEP_SCRATCH=1` keeps post-demosaic scratch alive (the old
allocation order) for memory comparisons.

GPU times are per-stage submissions; MoltenVK collapses timestamps inside one compute encoder, so per-pass
AMaZE timing needs isolation runs, not in-pass timestamps. Mac GPU times do not transfer to Adreno ratios;
confirm hot spots on the phone.
