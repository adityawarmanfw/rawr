# Linear Bayer bin weights

Fits and exports the weights of the GPU 2x Bayer bin (`native/raw_ingress`, `BayerBinner`), used when a
200 MP Bayer lens reduces its photos to 50 or 25 MP.

1. `fit_linear_bin.py` fits four 12x12 kernels (one per output CFA site) by least squares to an anti-aliased
   2x downscale of clean photos (high-resolution JPEGs, downscaled 4x first so their own processing
   disappears). The inputs carry the 200 MP sensors' measured grid (`f85_grid_phase16.npy`, a 16x16 relative
   pattern from a flat sky of a vivo F85 frame) at random offset and strength, plus noise. Constraints:
   exact flat colours, zero response on the 4-pixel grid lines and the 8x8 diagonal grid frequency; a
   penalty on any response beyond the 50 MP Nyquist.
2. Two fits ship: the plain kernel (`tag=plain`) and a sharp one fitted to the same target with an unsharp
   mask (`usm=0.6 tag=sharp`). The GPU uses the sharp one only where detail stands clearly above the
   frame's noise floor.
3. `export_kernels.py k_plain.npy k_sharp.npy ../../native/raw_ingress` writes
   `include/rawr/raw_ingress/BayerBinKernels.h` and `BayerBinNoise.h`.

Check a new fit with `tests/native/bayer_bin_vulkan_test` (GPU vs CPU reference) before shipping.
