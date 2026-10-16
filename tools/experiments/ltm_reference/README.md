# Archived local tone mapping prototype

Experimental only; not linked into the app. Camera Settings and Renderer hide Local via
`LOCAL_TONE_CONTROLS_VISIBLE`. Camera preferences now load Adaptive mode; saved renderer
recipes keep their original processing for reproducibility.

This archive retains the full/half/quarter-resolution ACES inverse prototype, the
sky-preserving display-domain blend, and the comparison scripts. The approved look
used shadows 2 EV, highlights 0.5 EV, sigma 5, coarsest mip 7, contrast boost off,
full-resolution direct reconstruction and 24-step inverse mapping. The blend uses
global +1 EV in bright pixels and the local output in shadows, with a smooth
transition between linear display luminance .04 and .18. It is an SDR experiment,
not an integrated HDR implementation.

## Recreate

Run from the repository root. `setup.py` refuses to overwrite an existing isolated
source tree. The patch is based on the production source at the time of this archive;
future source changes may require resolving patch conflicts.

```sh
uv run python tools/experiments/ltm_reference/setup.py
cmake -S tmp/ltm-reference/src/tools/offline_rawr_pipeline -B tmp/ltm-reference/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build tmp/ltm-reference/build --target rawr_offline_pipeline -j 6
uv run python tmp/ltm-reference/run.py
uv run --with pillow python tmp/ltm-reference/global_ev1.py
uv run --with pillow python tmp/ltm-reference/gentle.py
uv run --with pillow --with numpy python tmp/ltm-reference/sky_preserve.py
uv run --with pillow --with numpy python tmp/ltm-reference/resolution_test.py
```

The scripts require the original packed Bayer input and replay configuration at
`tmp/ltm-texture/source/input.rgba16f` and `input.rgba16f.txt`, derived from
`RAWR_20261016_110146_703.dng`. Source photos and generated images are not included
in this code archive. Existing artifacts remain under `tmp/ltm-reference`.

See EXPERIMENT.md for the initial ablations and limitations. Later comparisons
favored corrected full-resolution reconstruction with gentler settings and global
highlight preservation. Half-resolution was close; quarter-resolution softened
some wall texture. No phone performance benchmark was completed. The prototype
uses substantial memory and a costly per-pixel inverse; it was not shipped.
