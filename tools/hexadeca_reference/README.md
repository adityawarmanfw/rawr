# Hexadeca (200 MP, 4x4 same-colour blocks) -> 50 MP Bayer: research harness

Standalone; nothing here is selected by capture or merge code yet. Library code:
`app/src/main/cpp/imaging/CfaRemosaic.h` (CPU), `imaging/PhaseFit.h` (CPU fit),
`native/raw_ingress/` (GPU, see its README).

## Pipeline under test

1. optional per-frame phase-response fit, divided out of every sample (PhaseFit.h);
2. exact 2x2 same-colour sums -> 50 MP quad mosaic, quarter-DN units (black 256, white 4092);
3. quad -> Bayer with `CfaRemosaicQuality::Detail`: cubic green + per-pixel colour difference
   (radius 3), own-colour samples kept exact.

## Results

Ground truth (`gt_compare.py`: synthetic 4x4 mosaic from sharp 50 MP crops, 6 crops, lens blur
0.7 px; Bayer PSNR clean / noisy):

| Method | clean | noisy |
|---|---:|---:|
| 200 MP High remosaic + Natural reduce (old route) | 43.8 | 42.7 |
| 2x2 sum + Quad High | 41.9 | 41.0 |
| 2x2 sum + Detail | 43.2 | 41.8 |
| pixel-level hybrid (rejected, see below) | 45.0 | 42.7 |

- Error is almost all at "foreign" Bayer sites; own-colour sites are exact after the sum.
- The hybrid (interpolate at the 200 MP pixel level, average the 2x2 footprint) wins on
  synthetic data but amplifies the real sensor's 4x4 per-position response (period-4 grid 5.3%
  vs 1.3% on the sky ROI of a real frame). Detail reads only sums and keeps the 1.3%.
- Earlier experiments that did not pan out: local spectral reconstruction (Singh-style
  augmented/directional filters) - worse than High on edges and colour grids; per-frame gains
  inferred from one natural patch are unreliable.

Phase fit (`phase_fit.py` prototype, native in PhaseFit.h): the position response is a curve
over brightness (steps near 30 DN), different per capture. Period-4 grid on smooth regions,
G/R/B %, without -> with fit: vivo frame 1.27/2.90/1.21 -> 0.25/0.80/0.25 (others 2-5x better);
Xiaomi 0.6-0.7/1.5-2.0/0.9-1.1 -> mostly unchanged. Statistics use an odd-stride block lattice
(3 matches full quality; even strides miss block parities).

Phone (Adreno 840, 16320x12288, GPU only, readback excluded, warm): Detail ~114 ms per frame
(sum 8.5, green 14.4, Bayer out 76, trailing 8), Detail + fit ~151 ms (fit adds block sums 12,
statistics 3, curve 1.5, corrected sum +19). Mac CPU, 8 threads: Detail 0.26 s, fit + corrected
sum 0.07 s. GPU vs CPU: <= 2 codes on real frames, <= 4 with the fit.
Phone CPU (Adreno 840 phone, 8 threads, same frame, cool): plain sum ~14 ms, fit + corrected sum
~107 ms, Detail ~240 ms, so ~350 ms with the fit (was ~625 ms before the CPU Detail got the
per-phase tap table, fused green + diff pass and per-block fit apply). Quad High ~280 ms.

## Files

- `gt_compare.py` ground-truth comparison; `hybrid_proto.py` numba prototype of the sweeps.
- `phase_fit.py` prototype fit; `visual_compare.py`, `render_50mp.py` real-DNG renders
  (WIP vs Detail vs Detail + fit); `quad_validate.py` earlier Quad render check.
- `baseline.cpp`, `high_benchmark.cpp`, `detail_wrapper.cpp` C++ wrappers (High remosaic, tiled
  NaturalBayer reduce, Detail + native fit).
- Older scripts (`reference.py`, `augmented.py`, `directional.py`, `artifact_check.py`, ...) are
  the retained negative-result experiments above.

Run (repo root, outputs in tmp/): build the wrapper libraries with
`c++ -O3 -std=c++17 -shared -fPIC -Iapp/src/main/cpp <file>.cpp -o tmp/hexadeca-gt/<name>.dylib`
(`lib.dylib` from `high_benchmark.cpp`; `lib2.dylib` from `detail_wrapper.cpp`: Detail and the native fit),
then
`uv run --with numpy --with numba --with scipy --with pillow --with rawpy --with tifffile python -I tools/hexadeca_reference/gt_compare.py --crops 2 --blur 0.7 [--noise] SCENE.jpg ...`.
The DNGs used are in ~/Downloads (read only).
