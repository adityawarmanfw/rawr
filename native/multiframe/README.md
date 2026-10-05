# Multiframe burst stack

Used only by the multiframe (MFSR) capture path and
`tools/multiframe_moltenvk_replay`. `CMakeLists.txt` here is the single entry
point for both.

| Package | Target | Role |
|---|---|---|
| `alignment/` | (header + shaders) | Pyramid, block match, LK refine, dense flow. |
| `merge_wronski/` | (header + shaders) | Wronski kernel-regression merge and robustness. |
| `merge_hdrplus/` | (header + shaders) | HDR+ spatial merge (tile alignment + robust average, Bayer output), ported from hdr-plus-swift. |
| `pipeline/` | `raw_gpu_pipeline` | Executor, recorder and burst coordinator. Compiles the alignment and merge shaders in production and scalar-trace variants. |
| `output/` | `raw_multiframe_output` | Projects the merged RGB back to CFA for DNG and prepares the JPEG path's RGB and clip evidence. |
| `sharpness/` | `raw_sharpness`, `raw_sharpness::reference` | Burst frame sharpness scoring for reference selection. |

Include paths are unchanged by the grouping (`rawr/raw_alignment_gpu/…`,
`rawr/raw_gpu_pipeline/…`, `raw_sharpness/…`).

Tests: `pipeline/` host unit tests (`-DRAW_GPU_PIPELINE_BUILD_TESTS=ON`),
the sharpness CMake/CTest targets. Validation notes: `pipeline/validation/`.
