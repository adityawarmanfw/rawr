#include <cassert>
#include <cstdint>

#include "rawr/raw_gpu_pipeline/ScratchLayout.h"

using namespace rawr::raw_gpu_pipeline;

int main() {
    ScratchLayout l;
    auto img = [&](const char* n, std::uint32_t w, std::uint32_t h, PixelStorage s, const char* pool = "", const char* at = "") {
        l.images.push_back({n, Extent2D{w, h}, s, Lifetime::Companion});
        if (*pool) l.placements.push_back({n, pool, at});
    };
    // 100 B + 200 B unpooled.
    img("a", 5, 5, PixelStorage::RGBA32Float);  // 400 B
    // Pool: p (800 B) at 0, q (800 B) aliasing p, r (400 B) after both, s (1600 B) starting where r starts.
    img("p", 10, 10, PixelStorage::RG32Float, "pool", "");
    img("q", 10, 10, PixelStorage::RG32Float, "pool", "start:p");
    img("r", 5, 5, PixelStorage::RGBA32Float, "pool", "end:p,q");
    img("s", 10, 10, PixelStorage::RGBA32Float, "pool", "start:r");
    // A second pool counts on its own.
    img("t", 4, 4, PixelStorage::RGBA16Float, "other", "");  // 128 B
    assert(aliasedImageBytes(l) == 400u + (800u + 1600u) + 128u);
    // No placements: every image counts once.
    l.placements.clear();
    assert(aliasedImageBytes(l) == 400u + 800u + 800u + 400u + 1600u + 128u);
    return 0;
}
