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

Bracketed exposure (Rawr "HDR+ Bracketed", frequency merge only): based on
the upstream non-uniform-exposure path of `align_merge_frequency_domain` —
companions scaled to the reference exposure in `hdrp_prepare` (black-subtracted
signal times reference/frame exposure), `calculate_mismatch_rgba` with the
exposure factor, `calculate_highlights_norm_rgba` (`hdrq_highlights_norm`), no
magnitude norm. The reference is the darkest frame (chosen by the app).
Alignment uses the exposure paths of `compute_tile_differences_exposure25` and
`correct_upsampling_error`: each candidate displacement rescales the companion
by the ratio of tile means, clamped to +-10%, absorbing residual exposure
mismatch from the metadata. Not ported: the exposure-control output curves
(the app lifts brightness at render time instead). A burst whose exposures are
all equal merges exactly like the uniform path.

Rawr changes to the bracketed path (measured against noise-free truth with
22 frames at +3 EV and 4 dark frames; upstream averaged ~1-3 dark-frame
equivalents near highlights, these reach ~80-115):
- Exposure-weighted mean instead of upstream's equal-weight mean: each frame
  adds exposureFactor * (1 - w) * aligned to the spectrum and to a per-bin
  weight sum seeded with the reference at weight 1; `hdrq_normalize` divides
  before deconvolution. Brighter frames carry the shadows (inverse shot-noise
  variance) and rejected bins add nothing.
- Per-frame norms replace upstream's burst-averaged exposure_corr1/2, 28.5
  constant and min(4, ef) motion boost: the uniform constants, with the noise
  term scaled by (1 + 1/ef) / 2 (expected variance of the reference-companion
  difference). Equal-exposure frames merge exactly like the uniform path.
- Companions as dark as the reference merge first; their running merge
  (inverse transformed mid-pass) replaces brighter companions' cells near
  their own clip point (`hdrq_warp_rgba`, 0.90-0.98 of white). Scaled-down
  clipped samples sit below the reference white and turned magenta after WB;
  substituting the reference itself would add a noisy reference copy per
  bright frame and swamp the dark frames.
- Brighter companions align against a reference pyramid whose finest-level
  input is clamped just below their clip point (their own pyramid gets the
  same clamp): otherwise clipped plateaus, where the noisy dark reference
  still has detail, misalign whole tiles next to highlights.
- The highlights norm ramp starts at 0.90 of white (upstream 0.50), since
  clipped cells are already substituted.
