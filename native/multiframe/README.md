# Multiframe burst stack

Used only by the multiframe capture path and
`tools/multiframe_moltenvk_replay`. `CMakeLists.txt` here is the single entry
point for both.

| Package | Target | Role |
|---|---|---|
| `alignment/` | (header) | Alignment config type (no shaders left). |
| `merge_wronski/` | (header) | Shared noise / merge config types (`Config`, `CfaNoiseProfile`); its shaders were removed with the super-resolution merge. |
| `merge_hdrplus/` | (header + shaders) | HDR+ Quality: tile alignment and per-frequency merge (Bayer output), ported from hdr-plus-swift. |
| `merge_rexture/` | (header + shaders) | Rexture kernel-regression Bayer merge. |
| `pipeline/` | `raw_gpu_pipeline` | Executor, recorders and burst coordinator. Compiles the merge shaders in production and scalar-trace variants. |
| `output/` | `raw_multiframe_output` | Projects the merged CFA plane to 16-bit for DNG and prepares the JPEG path's RGB and clip evidence. |
| `sharpness/` | `raw_sharpness`, `raw_sharpness::reference` | Burst frame sharpness scoring for reference selection. |

Include paths are unchanged by the grouping (`rawr/raw_alignment_gpu/…`,
`rawr/raw_gpu_pipeline/…`, `raw_sharpness/…`).

Tests: `pipeline/` host unit tests (`-DRAW_GPU_PIPELINE_BUILD_TESTS=ON`),
the sharpness CMake/CTest targets. Validation notes: `pipeline/validation/`.
