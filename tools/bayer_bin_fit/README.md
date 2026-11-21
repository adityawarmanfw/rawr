# Linear Bayer bin weights

Fits and exports the weights of the GPU 2x Bayer bin (`native/raw_ingress`, `BayerBinner`), used when a
200 MP Bayer lens reduces its photos to 50 MP (2x bin) or 22 MP (direct 3x bin).

1. `fit_linear_bin.py` fits four 12x12 kernels (one per output CFA site) by least squares to an anti-aliased
   2x downscale of clean photos (high-resolution JPEGs, downscaled 4x first so their own processing
   disappears). The inputs carry the 200 MP sensors' measured grid (`f85_grid_phase16.npy`, a 16x16 relative
   pattern from a flat sky of a vivo F85 frame) at random offset and strength, plus noise. Constraints:
   exact flat colours, zero response on the 4-pixel grid lines and the 8x8 diagonal grid frequency; a
   penalty on any response beyond the 50 MP Nyquist, and (`chroma=0.2,0.12`) a penalty that makes each
   colour reproduce itself and nothing of the others below 0.12 cycles per source pixel. Without that last
   term the fit borrows the neighbourhood's luma for every colour, because the training photos are mostly
   low-saturation, and coloured detail a few pixels wide (a thin orange mast against the sky) turns grey;
   the fit prints how much saturation a thin bar keeps.
2. The shipped kernels: `fit_linear_bin.py 12 1e-4 notch8 beta=0.02 chroma=0.2,0.12 tag=plain images=DIR` (2x)
   and `fit_linear_bin.py 17 1e-4 ratio=3 notch8 beta=0.02 chroma=0.2,0.1 tag=bin3 images=DIR` (3x: output
   centred on source 3X+1, window -8..8, stopband beyond 0.18 cycles/px).
   The GPU clamps its result to the nearby same-colour source samples (the kernel rings about 5% at a hard
   edge). No sharpening is applied in the bin: the renderer's capture sharpening owns that, and a bin that
   sharpens stacks with it into halos.
3. `export_kernels.py k_plain.npy k_bin3.npy ../../native/raw_ingress` writes
   `include/rawr/raw_ingress/BayerBinKernels.h` and `BayerBinNoise.h`.

Check a new fit with `tests/native/bayer_bin_vulkan_test` (GPU vs CPU reference) before shipping.
