#pragma once
#include <rawr/raw_gpu_pipeline/ResourceGeometry.h>

#include <cstdint>
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

}  // namespace rawr::raw_gpu_pipeline
