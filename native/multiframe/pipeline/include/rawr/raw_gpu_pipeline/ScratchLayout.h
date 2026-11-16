#pragma once
#include <rawr/raw_gpu_pipeline/ResourceGeometry.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace rawr::raw_gpu_pipeline {

enum class PixelStorage : std::uint32_t { R8Uint, R16Uint, R32Float, RG32Float, RGBA16Float, RGBA32Float, R16Float };

enum class Lifetime : std::uint32_t {
    Burst,        // retained across all companions
    Companion,    // reused for each companion
    PyramidTemp,  // reused while building reference/companion pyramids
    Output        // final reconstruction/accumulators
};

struct ImageSpec {
    std::string name;
    Extent2D extent{};
    PixelStorage storage = PixelStorage::R32Float;
    Lifetime lifetime = Lifetime::Companion;
};

struct BufferSpec {
    std::string name;
    std::uint64_t bytes = 0;
    Lifetime lifetime = Lifetime::Companion;
};

// Optional memory pool placement of a named image: images of one pool share a single allocation and are placed explicitly,
// so images whose lifetimes never overlap can alias. placeAt: "" = offset 0, "start:A" = the offset of A (A earlier in the
// pool's member order), "end:A[,B...]" = right after the furthest end among the named images. The caller guarantees the
// lifetimes. Members are processed in layout image order.
struct ImagePlacement {
    std::string name, pool, placeAt;
};

struct ScratchLayout {
    std::vector<ImagePlacement> placements;
    MultiframeGeometry geometry{};
    std::vector<ImageSpec> images;
    std::vector<BufferSpec> buffers;
    std::uint64_t logicalImageBytes = 0;
    std::uint64_t logicalBufferBytes = 0;
};

std::uint32_t bytesPerPixel(PixelStorage) noexcept;

// Image bytes the arena really allocates: images of one pool share a block laid out by their placement rules, so
// ScratchLayout::logicalImageBytes (every image counted once) overstates it (Rexture at 50 MP: 3.8 against ~3.1 GiB).
// Mirrors ResourceArena's pool layout without the driver's size/alignment rounding, so it is an estimate within a few
// MiB. Memory planning reads this one; `logicalImageBytes` stays what it was.
inline std::uint64_t aliasedImageBytes(const ScratchLayout& layout) {
    std::map<std::string, const ImagePlacement*> placement;
    for (const auto& p : layout.placements) placement[p.name] = &p;
    std::map<std::string, std::uint64_t> offset, end, poolBytes;
    std::uint64_t total = 0;
    for (const auto& image : layout.images) {
        const std::uint64_t bytes = std::uint64_t(image.extent.width) * image.extent.height * bytesPerPixel(image.storage);
        const auto it = placement.find(image.name);
        if (it == placement.end() || it->second->pool.empty()) {
            total += bytes;
            continue;
        }
        const std::string& at = it->second->placeAt;
        std::uint64_t o = 0;
        if (at.rfind("start:", 0) == 0) {
            o = offset[at.substr(6)];
        } else if (at.rfind("end:", 0) == 0) {
            std::stringstream names(at.substr(4));
            for (std::string n; std::getline(names, n, ',');) o = std::max(o, end[n]);
        }
        offset[image.name] = o;
        end[image.name] = o + bytes;
        auto& pool = poolBytes[it->second->pool];
        pool = std::max(pool, o + bytes);
    }
    for (const auto& pool : poolBytes) total += pool.second;
    return total;
}

}  // namespace rawr::raw_gpu_pipeline
