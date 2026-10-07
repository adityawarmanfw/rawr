#include <rawr/raw_merge_hdrplus_gpu/RawMergeHdrPlusGpu.h>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace hp = rawr::raw_merge_hdrplus_gpu;

int main() {
    // 12.5 MP sensor at defaults (tile 32, search 64): upstream builds 6 levels
    // (1536 -> 48 px short side) and pads 4080 -> 4096 (tile factor 8 * 2^6).
    const auto g = hp::makeGeometry(4080, 3072, hp::Config{});
    assert(g.paddedWidth == 4096u && g.paddedHeight == 3072u);
    assert(g.padLeft == 8u && g.padTop == 0u);
    assert(g.levels.size() == 6u);
    const unsigned tiles[] = {32, 16, 8, 8, 8, 8};
    for (std::size_t i = 0; i < g.levels.size(); ++i) {
        assert(g.levels[i].tileSize == tiles[i]);
        assert(g.levels[i].width == 4096u >> (i + 1u));
        assert(g.levels[i].tilesX == g.levels[i].width / (tiles[i] / 2u) - 1u);
    }
    assert(g.levels[0].tilesX == 127u && g.levels[0].tilesY == 95u);

    // 3064 rows pad to 3072 with an even split (Bayer phase preserved).
    const auto h = hp::makeGeometry(4080, 3064, hp::Config{});
    assert(h.paddedHeight == 3072u && h.padTop == 4u);
    // Odd half-padding rounds down to keep both sides even.
    const auto o = hp::makeGeometry(4092, 3072, hp::Config{});
    assert(o.paddedWidth == 4096u && o.padLeft == 2u && (o.padLeft & 1u) == 0u);

    // Robustness mapping (upstream: 0.12 * 1.3^(0.5 * (36 - nr)) - 0.4529822).
    assert(std::fabs(hp::robustness(13.f) - 1.99907f) < 1e-4f);
    assert(hp::robustness(22.f) < hp::robustness(13.f) && hp::robustness(13.f) < hp::robustness(1.f));

    // Bracketed frequency norms (upstream uniform_exposure == false). A
    // uniform burst falls back to the uniform constants exactly.
    {
        const auto uniform = hp::frequencyNorms(13.f);
        const auto same = hp::bracketedFrequencyNorms(13.f, {1.f, 1.f, 1.0005f});
        assert(hp::uniformExposure({1.f, 1.f, 1.0005f}) && !hp::uniformExposure({1.f, 4.f}));
        assert(same.robustnessNorm == uniform.robustnessNorm && same.maxMotionNorm == uniform.maxMotionNorm);
        const auto b = hp::bracketedFrequencyNorms(13.f, {1.f, 4.f});
        assert(std::fabs(b.robustnessNorm - 0.273291f) < 1e-5f);
        assert(std::fabs(b.readNoise - 12.12573f) < 1e-4f);
        assert(std::fabs(b.maxMotionNorm - 2.345935f) < 1e-5f);
        assert(std::fabs(hp::bracketedMaxMotionNorm(b, 4.f) - 6.126578f) < 1e-5f);
        // Brighter-than-4x frames get no further motion-norm boost.
        assert(hp::bracketedMaxMotionNorm(b, 16.f) == hp::bracketedMaxMotionNorm(b, 4.f));
    }

    bool threw = false;
    try {
        (void)hp::makeGeometry(4081, 3072, hp::Config{});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw);
    std::puts("hdrplus_geometry_test OK");
    return 0;
}
