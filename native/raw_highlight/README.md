# Highlight reconstruction

This module owns both highlight reconstruction methods. They are selected by the
`capture_highlight_method` setting:

| Method | Name | Origin | Domain |
|---|---|---|---|
| 0 | Colour propagation | In-house | White-balanced camera RGB |
| 1 | Inpaint Opposed | RawTherapee 5.13 "Coloropp" | Pre-WB demosaiced camera RGB, plus an SDR-only tone tap |

Clip classification happens before either method, in sensor space and before
LSC/WB: `native/raw_demosaic/common/shaders/sensor_clip_pack.comp` for stills,
`raw_preview_cfa_state.comp` for preview, the video RAW stage for video, and
`highlight_clip_state.comp` when only RGB is available. DNG writers never
consume reconstructed RGB or the clip-state image.

## Colour propagation (method 0)

With reconstruction **off**, every white-balanced channel is hard-limited to
the lowest post-WB channel ceiling. This common clamp prevents unequal WB
headroom from becoming false magenta and leaves highlight rolloff to the
colour transform and tonemapper.

With reconstruction **on**:

1. `highlight_guide_seed.comp` builds a low-resolution colour guide from bright,
   fully valid, locally smooth pixels.
2. `highlight_guide_propagate.comp` fills clipped regions by jump flooding.
3. `highlight_guide_smooth.comp` runs four smoothing passes.
4. The apply pass fits the guide to the channels that still carry detail and
   raises only sensor-clipped channels (`raw_highlight_post_rgb.glsl`). Lens
   shading may lift valid RGB above the nominal WB ceiling; those channels stay
   reliable because classification uses the pre-LSC clip mask. The fit uses
   pixel-local surviving channels for intensity; spatial smoothing applies only
   to the colour guide, so neighboring brightness cannot create a colored rim. If every
   channel is lost, the pixel takes the guide colour at the clip edge and fades
   to a bright neutral core with depth (share of fully clipped mask texels
   within +-12 texels), so a light keeps a soft coloured rim instead of a
   one-pixel white contour.

`GuideChain` (host) records steps 1–3; each caller owns the apply pass:

- Preview: `highlight_apply_inplace.comp`, `RAWR_HL_SCALE=1` (half-resolution
  image, 4 px guide cells).
- Still and video: `highlight_recover.comp`, `RAWR_HL_SCALE=2` (full
  resolution, 8 px guide cells), through `PostDemosaicProcessor`.

Both cover 8 sensor pixels per guide cell and repair the same sensor-clipped
channels, so preview matches the saved image. Distortion-resampled stills retain
the numeric fallback until their sensor mask is resampled with the RGB; the
unaligned mask must not select channels at unrelated coordinates.

Regression validation uses both production apply shaders on neutral and colored
intensity edges, clipping each channel independently. It checks that an unrelated
brightness field in the guide cannot change local reconstructed color or detail.
It also applies 2.5x lens shading after sensor clipping to verify that surviving
channels keep their color, including valid RGB above the nominal WB ceiling:

```sh
cmake -S native/raw_highlight/tests -B tmp/highlight-fit
cmake --build tmp/highlight-fit
ctest --test-dir tmp/highlight-fit --output-on-failure
```

Preview requires these shaders: `RawPreview` refuses to start without them
rather than falling back to a different guide. (`raw_preview.comp`, the path
without CFA state used by validation and diagnostics, keeps a local inline
guide.)

## Inpaint Opposed (method 1)

`coloropp.comp` repairs clipped channels from the cube-root mean of the opposed
channels in a 3×3 neighbourhood. It runs on pre-WB RGB with lens shading. The
post-WB stage then runs `highlight_recover.comp` in passthrough mode (2).
`coloropp_tone.comp` applies RawTherapee's analytic highlight compression to
the SDR render only; the HDR gain-map tap is unaffected. Host code:
`raw_highlight/ColoroppProcessor.hpp`, driven by `native/post_demosaic`.

Preview, still and video all run the selected method. Preview compiles
`coloropp.comp` with `RAWR_COLOROPP_POST_WB=1` because its RGB is already white
balanced; the clip test and opposed mean are algebraically the same. It writes
the repaired image to its linear output (read by RAW overlays and scopes) and
the tone-compressed copy to a separate image that tonemap and film read, like
the still HDR and SDR taps. `raw_highlight/Coloropp.hpp` holds the parameter
mapping both hosts use.

## Related controls

The tonemap **Highlights** slider is separate. Negative values compress
scene-linear highlight luminance; positive values lift the localized display
highlight range. It never enables or tunes channel reconstruction.
