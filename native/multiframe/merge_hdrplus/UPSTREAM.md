# HDR+ spatial merge (Burst Photo)

Source: https://github.com/martin-marek/hdr-plus-swift
Revision: 69cb0572bb6712e160c448260125cb6099bdfd87 (2024-08-24).
GPL-3.0; compatible with Rawr's GPL-3.0-only (see repository LICENSE).

Ported to GLSL compute: the "Fast" (spatial-domain) merge path,
`burstphoto/align/align.{swift,metal}`, `burstphoto/merge/spatial.{swift,metal}`
and the texture helpers it uses (`blur_mosaic_texture`, `avg_pool`,
`upsample_*`, `texture_mean`, `add_texture_weighted`). Matches the upstream
exposure-control-off path (plain pyramid, no black/WB normalization), which
is how uniform-exposure bursts are merged. Parity against the Metal original
on RZSL bursts is ~80 dB PSNR (differences at integer-rounding level).

Local changes:
- Frames stay in raw units re-based onto the reference black; the output is
  normalized per CFA phase into the RGBA16F merge output (value in every
  channel) so the existing CFA projection / packed-CFA consumers are reused.
- Hot pixels use Rawr's sensor hot-pixel list (median-of-4 concealment)
  instead of upstream's burst-average detector.
- Tile alignment vectors live in SSBOs; tile costs use shared-memory staging
  and split per-lane partial sums (same costs up to float summation order).
  Weight upsample, lerp and accumulate are fused into one pass; the noise
  mean stays on the GPU. Tile size 64 ("Large") is not supported.
- Odd half-padding is rounded to keep both padded sides even (Bayer phase).

Bracketed exposure (Rawr "HDR+ Bracketed", frequency merge only): ports the
upstream non-uniform-exposure path of `align_merge_frequency_domain` —
companions scaled to the reference exposure in `hdrp_prepare` (black-subtracted
signal times reference/frame exposure), `calculate_mismatch_rgba` with the
exposure factor, `calculate_highlights_norm_rgba` (`hdrq_highlights_norm`), the
28.5 robustness constant with `exposure_corr1/2`, the per-frame
`min(4, ef) * sqrt(max_motion_norm)` and no magnitude norm. The reference is
the darkest frame (chosen by the app). Not ported: the exposure-aware tile
cost (`compute_tile_differences_exposure25`) — frames are already exposure
matched before alignment — and the exposure-control output curves (the app
lifts brightness at render time instead). A burst whose exposures are all
equal merges exactly like the uniform path.
