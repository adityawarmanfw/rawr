# Local tone mapping reference experiment

Source: RAWR_20261016_110146_703.dng, packed input reused from ../ltm-texture/source.
Experiments are isolated under src. No production source changes or app installation in this experiment.

## Reproduction

Run setup.py to construct the isolated source tree (other tools directories must also be symlinked as in the current tree), build build/rawr_offline_pipeline with CMake/Ninja, then:
- uv run python tmp/ltm-reference/run.py
- uv run python tmp/ltm-reference/defaults.py
- uv run --with numpy --with pillow python tmp/ltm-reference/compare.py
- uv run --with numpy --with pillow --with scipy python tmp/ltm-reference/display.py
- uv run --with numpy --with pillow python tmp/ltm-reference/final_sheets.py

## Method

Same source, demosaic and downstream pipeline for all native variants. Host uses Dual RCD/VNG4, whereas the phone exports used AMaZE/VNG4; these are controlled host comparisons, not pixel-exact replicas of phone JPEGs. SDR output comparisons; no claim of Ultra HDR equivalence.

Native primary ablation: shadows 2 EV, highlights 2 EV, sigma 5, coarsest mip 7, contrast boost on. Full/half/quarter base resolutions preserve the same coarsest image scale. Full direct reconstruction bypasses guided regression. Inverse variant uses 24 bisection steps over +/-8 EV to invert exactly the ACES proxy used for fusion, along each original RGB ray. This does not invert the downstream RAWR display curve.

Default check: same exposure/sigma, mip 6, contrast boost off. Global uses adaptive lift .76 EV, key -4.49946928, highlight -.677802324.

Wall normalization: median linear Rec.709 Y in portrait region (1160,1750)-(1500,2160); includes some foliage/highlights, not a pure wall mask. A uniform linear RGB gain is applied for the matched crop only, with the amount labeled. Native outputs are also provided.

Additional display-domain candidate: fuse sqrt(linear display Y) from actual RAWR -2/0/+2 EV SDR exposures, Gaussian pyramid sigma 1 with seven downsamples, bilinear reconstruction, exposure weights sigma 5, optional Laplacian magnitude weighting. Reconstruct using neutral exposure chromaticity. This changes both transfer and pyramid filters, so it is a candidate rather than a single-variable ablation. Inputs are 8-bit PPM and output may clip; not a production HDR design.

## Findings

- Quarter ratio wall median Y .018517 vs global .013028: +.51 EV.
- Full direct ratio .018654: +.52 EV. Removing quarter resolution/guided reconstruction does not resolve the overall wall appearance.
- Full direct inverse .023923: +.88 EV. Correct proxy inversion lifts shadows more strongly and darkens bright regions; it is not a visual win by itself.
- Full guided/half guided/quarter guided inverse medians .023878/.023741/.023736, close to direct inverse.
- Matched crops retain considerable wall texture, suggesting raised shadow level/local contrast redistribution is a major cause of perceived haze. This is a visual assessment, not proof of no detail loss.
- Forward validation of inverse: for desired lightness in [0,1], maximum absolute error .000375, 99th percentile .000218; no nonfinite errors. Desired lightness overshoots [0,1] for 1.62% of full-res boosted pixels (range -.2474 to 1.1631), which cannot be reconstructed exactly. Default non-boosted full-res also overshoots for .261% (range -.1515 to 1.1152). These measurements describe the experimental full-resolution pyramids, not production overshoot rates.
- Display-domain fusion avoids proxy mismatch but still does not clearly improve the natural wall/sky balance over global. Boost gives stronger/harsh texture, not an automatic quality improvement.

Conclusion: the quarter-resolution shortcut is not the dominant cause of this scene's hazy wall. Proxy gain conversion is inconsistent, but correcting it does not solve the desired rendering. This exposure-fusion configuration itself redistributes contrast in an undesirable way. This single scene does not establish a fundamental limitation of all Mertens/local algorithms.

Recommendation: retain global as the quality baseline. A future alternative should constrain a modest local exposure correction around the global result, explicitly protecting deep-shadow contrast and limiting edge artifacts; validate on multiple scenes before replacing the current implementation. Do not ship this reference as a fix.
