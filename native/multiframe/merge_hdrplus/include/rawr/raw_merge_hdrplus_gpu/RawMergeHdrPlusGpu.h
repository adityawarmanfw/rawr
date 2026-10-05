#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

// HDR+ spatial-domain burst merge, ported from hdr-plus-swift (Burst Photo,
// "Fast" merging; see ../UPSTREAM.md). Tile-based coarse-to-fine alignment of
// raw Bayer frames, then a per-pixel robust average against the reference
// whose weight falls off with the blurred frame difference relative to noise.
namespace rawr::raw_merge_hdrplus_gpu {

struct Config {
    // Upstream "noise reduction" slider, 1..22 (23 = plain average upstream,
    // not exposed). Higher merges more aggressively (cleaner, more ghosting).
    float strength = 13.f;
    // Alignment tile size at the first (2x2-binned) pyramid level: 16 or 32
    // (upstream's 64 is not supported: the tile-cost shader stages tiles in
    // shared memory sized for 32).
    std::uint32_t tileSize = 32;
    // Pyramid stops once the coarsest level is <= this many pixels on the
    // short side: 128 (small motion) / 64 / 32 (large motion).
    std::uint32_t searchDistance = 64;
};

inline bool valid(const Config& c) {
    return std::isfinite(c.strength) && c.strength >= 1.f && c.strength <= 22.f &&
           (c.tileSize == 16u || c.tileSize == 32u) &&
           (c.searchDistance == 32u || c.searchDistance == 64u || c.searchDistance == 128u);
}

// Upstream align_merge_spatial_domain: four strength steps double the
// tolerated difference (shot noise sd grows sqrt(2) per ISO stop).
inline float robustness(float strength) {
    const double rev = 0.5 * (36.0 - double(int(strength + 0.5f)));
    return float(0.12 * std::pow(1.3, rev) - 0.4529822);
}

struct LevelGeometry {
    std::uint32_t width = 0, height = 0;  // pyramid level extent
    std::uint32_t tileSize = 0;           // tiles overlap by tileSize/2
    std::uint32_t tilesX = 0, tilesY = 0;
};

struct Geometry {
    std::uint32_t width = 0, height = 0;          // RAW extent
    std::uint32_t padLeft = 0, padTop = 0;        // zero padding so every level tiles exactly
    std::uint32_t paddedWidth = 0, paddedHeight = 0;
    std::vector<LevelGeometry> levels;            // fine (index 0, 2x2-binned) to coarse
};

// Mirrors upstream's pyramid/padding derivation. Each level is a 2x average
// pool of the previous; level 0 bins the Bayer 2x2 cells. Search radius is 2
// at every level. Padding is split so both sides stay even (Bayer phase).
inline Geometry makeGeometry(std::uint32_t width, std::uint32_t height, const Config& c) {
    if (width == 0u || height == 0u || (width & 1u) || (height & 1u))
        throw std::invalid_argument("hdrplus geometry: RAW dimensions must be nonzero/even");
    if (!valid(c)) throw std::invalid_argument("hdrplus geometry: invalid config");
    std::vector<std::uint32_t> tiles{c.tileSize};
    std::uint32_t res = std::min(width, height) / 2u;
    std::uint32_t factor = 2u;
    while (res > c.searchDistance) {
        tiles.push_back(std::max(tiles.back() / 2u, 8u));
        factor *= 2u;
        res /= 2u;
    }
    const std::uint32_t tileFactor = tiles.back() * factor;
    Geometry g{};
    g.width = width;
    g.height = height;
    g.paddedWidth = (width + tileFactor - 1u) / tileFactor * tileFactor;
    g.paddedHeight = (height + tileFactor - 1u) / tileFactor * tileFactor;
    g.padLeft = ((g.paddedWidth - width) / 2u) & ~1u;
    g.padTop = ((g.paddedHeight - height) / 2u) & ~1u;
    std::uint32_t w = g.paddedWidth, h = g.paddedHeight;
    for (const auto tile : tiles) {
        w /= 2u;
        h /= 2u;
        LevelGeometry l{};
        l.width = w;
        l.height = h;
        l.tileSize = tile;
        if (w < tile || h < tile) throw std::invalid_argument("hdrplus geometry: level smaller than a tile");
        l.tilesX = w / (tile / 2u) - 1u;
        l.tilesY = h / (tile / 2u) - 1u;
        g.levels.push_back(l);
    }
    return g;
}

}  // namespace rawr::raw_merge_hdrplus_gpu
