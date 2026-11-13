#pragma once
// Memory estimate for the multiframe merge arena. The arena is laid out for the
// whole merge window (native/multiframe/pipeline/src/ScratchLayout.cpp and
// HdrPlusRecorder.cpp), so it grows with the RAW geometry, not with the frame
// count. Wronski is the largest layout at a given geometry; the HDR+ layouts
// are smaller. Used to keep a camera session from reserving an arena that the
// device cannot hold, and to log the planned footprint before a merge.
#include <cstdint>

#include "rawr/raw_gpu_pipeline/RextureRecorder.h"
#include "rawr/raw_gpu_pipeline/ResourceGeometry.h"
#include "rawr/raw_gpu_pipeline/ScratchLayout.h"

namespace rawrcam::capture::multiframe {

// Logical image+buffer bytes of the merge arena of `algorithm` (MergeAlgorithm id) at this geometry. Rexture has its
// own layout (about 0.65x of Wronski's); every other id, and the default, gives the largest layout (Wronski, outputScale
// 1x) that the geometry can need. Returns 0 for a geometry the layout rejects;
// callers treat 0 as "unknown" and do not gate on it.
inline std::uint64_t estimateMergeArenaBytes(std::uint32_t width, std::uint32_t height,
                                                 std::uint32_t algorithm = 0u) noexcept {
    if (width == 0 || height == 0 || (width & 1u) != 0 || (height & 1u) != 0) return 0;
    try {
        const auto geometry = rawr::raw_gpu_pipeline::makeMultiframeGeometry(width, height, 1.0f);
        const auto layout = algorithm == 4u ? rawr::raw_gpu_pipeline::makeRextureScratchLayout(geometry)
                                            : rawr::raw_gpu_pipeline::makeMultiframeScratchLayout(geometry);
        return layout.logicalImageBytes + layout.logicalBufferBytes;
    } catch (...) {
        return 0;
    }
}

}  // namespace rawrcam::capture::multiframe
