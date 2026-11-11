#include <rawr/raw_gpu_pipeline/ScratchLayout.h>

#include <algorithm>
#include <array>
#include <stdexcept>

namespace rawr::raw_gpu_pipeline {
std::uint32_t bytesPerPixel(PixelStorage s) noexcept {
    switch (s) {
        case PixelStorage::R8Uint:
            return 1;
        case PixelStorage::R16Uint:
        case PixelStorage::R16Float:
            return 2;
        case PixelStorage::R32Float:
            return 4;
        case PixelStorage::RG32Float:
            return 8;
        case PixelStorage::RGBA16Float:
            return 8;
        case PixelStorage::RGBA32Float:
            return 16;
    }
    return 0;
}
namespace {
std::uint64_t imageBytes(const ImageSpec& s) {
    return static_cast<std::uint64_t>(s.extent.width) * s.extent.height * bytesPerPixel(s.storage);
}
Extent2D validAxis(Extent2D src, std::uint32_t factor, bool y) {
    if (factor == 1u) return src;
    const float sigma = static_cast<float>(factor) * 0.5f;
    const int radius = static_cast<int>(4.0f * sigma + 0.5f);
    const std::uint32_t k = static_cast<std::uint32_t>(2 * radius + 1);
    if (y)
        src.height -= k - 1u;
    else
        src.width -= k - 1u;
    return src;
}
}  // namespace
ScratchLayout makeMultiframeScratchLayout(const MultiframeGeometry& g) {
    return makeMultiframeScratchLayout(g, g.output.height);
}
ScratchLayout makeMultiframeScratchLayout(const MultiframeGeometry& g, std::uint32_t reconstructionStripeHeight) {
    ScratchLayout r{};
    r.geometry = g;
    auto img = [&](std::string n, Extent2D e, PixelStorage s, Lifetime l) {
        if (!e.width || !e.height) throw std::invalid_argument("scratch layout: empty image " + n);
        r.images.push_back({std::move(n), e, s, l});
    };
    auto buf = [&](std::string n, std::uint64_t nbytes, Lifetime l) { r.buffers.push_back({std::move(n), nbytes, l}); };

    // RAW16 burst inputs are owned by zsl_ring::RawImageRing; arena begins at normalized R32F.
    img("reference_raw_f32", g.raw, PixelStorage::R32Float, Lifetime::Burst);
    img("companion_raw_f32", g.raw, PixelStorage::R32Float, Lifetime::Companion);
    // The frozen CPU aligns a separable local5 CFA-suppressed view while merge
    // consumes the original normalized RAW. robust_a is the separable-pass
    // temporary and robust_b holds the companion view until alignment finishes.
    img("reference_alignment_f32", g.raw, PixelStorage::R32Float, Lifetime::Burst);

    // Coarse-to-fine level 3 is the fine normalized RAW alias in the eventual allocator.
    // List only non-fine pyramid images as physical logical resources here.
    for (std::size_t l = 0; l < 3u; ++l) {
        img("reference_pyramid_l" + std::to_string(l), g.coarseToFine[l].image, PixelStorage::R32Float,
            Lifetime::Burst);
        img("companion_pyramid_l" + std::to_string(l), g.coarseToFine[l].image, PixelStorage::R32Float,
            Lifetime::Companion);
    }

    // Exact VALID Gaussian intermediates for fine-index factors 2,4,4. They are reused
    // for reference then for every companion, so only one Y/X pair per size is needed.
    Extent2D cur = g.raw;
    static constexpr std::array<std::uint32_t, 4> factors{1u, 2u, 4u, 4u};
    for (std::size_t fi = 1; fi < 4u; ++fi) {
        Extent2D y = validAxis(cur, factors[fi], true);
        Extent2D x = validAxis(y, factors[fi], false);
        img("pyramid_tmp_y_f" + std::to_string(fi), y, PixelStorage::R32Float, Lifetime::PyramidTemp);
        img("pyramid_tmp_x_f" + std::to_string(fi), x, PixelStorage::R32Float, Lifetime::PyramidTemp);
        cur = g.coarseToFine[3u - fi].image;
    }

    for (std::size_t l = 0; l < 4u; ++l) {
        const Extent2D t{g.coarseToFine[l].tilesX, g.coarseToFine[l].tilesY};
        img("tile_flow_l" + std::to_string(l), t, PixelStorage::RG32Float, Lifetime::Companion);
    }
    const Extent2D fineTiles{g.coarseToFine[3].tilesX, g.coarseToFine[3].tilesY};
    img("tile_valid", fineTiles, PixelStorage::R8Uint, Lifetime::Companion);
    // Reference structure tensor per fine tile (A00, A01, A11, pixel count),
    // written by lk_refine and read by the affine gate.
    img("tile_tensor_l3", fineTiles, PixelStorage::RGBA32Float, Lifetime::Companion);

    // Steerable kernel and robustness live at half RAW resolution where possible.
    img("reference_kernel", g.guide, PixelStorage::RGBA32Float, Lifetime::Burst);
    img("reference_guide", g.guide, PixelStorage::RGBA32Float, Lifetime::PyramidTemp);
    img("reference_mean_lr", g.guide, PixelStorage::RGBA32Float, Lifetime::Burst);
    img("reference_var_lr", g.guide, PixelStorage::RGBA32Float, Lifetime::Burst);
    img("companion_guide", g.guide, PixelStorage::RGBA32Float, Lifetime::Companion);
    img("companion_mean_lr", g.guide, PixelStorage::RGBA32Float, Lifetime::Companion);
    img("companion_var_scratch", g.guide, PixelStorage::RGBA32Float, Lifetime::Companion);
    img("robust_scale_tiles", fineTiles, PixelStorage::R32Float, Lifetime::Companion);
    img("robust_a", g.raw, PixelStorage::R32Float, Lifetime::Companion);
    img("robust_b", g.raw, PixelStorage::R32Float, Lifetime::Companion);
    img("support", g.raw, PixelStorage::R32Float, Lifetime::Burst);

    if (reconstructionStripeHeight == 0u)
        throw std::invalid_argument("scratch layout: reconstruction stripe height must be nonzero");
    const Extent2D accumulatorExtent{g.output.width, std::min(g.output.height, reconstructionStripeHeight)};
    // Accumulation mirrors the reference binary32 state. Keep FP16 only at
    // the final output boundary; repeated half stores between companion and
    // reference passes introduce quantization that the reference algorithm does not.
    img("rgb_sum", accumulatorExtent, PixelStorage::RGBA32Float, Lifetime::Output);
    img("rgb_weight", accumulatorExtent, PixelStorage::RGBA32Float, Lifetime::Output);
    // Red/green squared per-frame weights occupy the unused accumulator alpha
    // lanes; blue needs one scalar. This measures independent frame support.
    img("rgb_weight_square_b", accumulatorExtent, PixelStorage::R32Float, Lifetime::Output);
    img("linear_output", g.output, PixelStorage::RGBA16Float, Lifetime::Output);

    const std::uint64_t tileN = static_cast<std::uint64_t>(fineTiles.width) * fineTiles.height;
    buf("affine_weights", tileN * sizeof(float), Lifetime::Companion);
    buf("affine_residuals", tileN * sizeof(float), Lifetime::Companion);
    // IRLS scratch requires sorted residual work + normal-equation workspace; keep conservative 2*tileN+64 floats.
    buf("affine_scratch", (2u * tileN + 64u) * sizeof(float), Lifetime::Companion);
    buf("affine_model", 16u * sizeof(float), Lifetime::Companion);
    // R, averaged-G, and B curves are stored contiguously. The aggregate
    // brightness-indexed model replicates the same curve into all three planes.
    buf("noise_std_curve", 3u * 1001u * sizeof(float), Lifetime::Burst);
    buf("noise_diff_curve", 3u * 1001u * sizeof(float), Lifetime::Burst);

    for (const auto& q : r.images) r.logicalImageBytes += imageBytes(q);
    for (const auto& q : r.buffers) r.logicalBufferBytes += q.bytes;
    return r;
}
}  // namespace rawr::raw_gpu_pipeline
