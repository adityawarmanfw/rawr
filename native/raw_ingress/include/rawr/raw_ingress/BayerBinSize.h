#pragma once
// Output size of the linear Bayer bins (BayerBinner::bin), header-only for callers without Vulkan.
#include <cstdint>

namespace rawr::raw_ingress {
struct BayerBinSize {
    std::uint32_t width = 0, height = 0, offsetX = 0, offsetY = 0;  // offset: source crop, even
};
// Ratio 2: half the source per side. Ratio 3: a third, rounded down to a multiple of 8, taken from the
// centred source area (an even offset, so the colour phase holds).
inline BayerBinSize bayerBinOutputSize(std::uint32_t width, std::uint32_t height, std::uint32_t ratio) {
    if (ratio != 3) return {width / 2, height / 2, 0, 0};
    const std::uint32_t w = (width / 3) & ~7u, h = (height / 3) & ~7u;
    return {w, h, ((width - 3 * w) / 2) & ~1u, ((height - 3 * h) / 2) & ~1u};
}
}  // namespace rawr::raw_ingress
