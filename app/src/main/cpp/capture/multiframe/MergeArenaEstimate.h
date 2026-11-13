#pragma once
// Memory estimate for the multiframe merge arena. The arena is laid out for the whole merge window
// (native/multiframe/pipeline: RextureRecorder.cpp, HdrPlusRecorder.cpp), so it grows with the RAW geometry, not with the
// frame count. Used to keep a camera session from reserving an arena that the device cannot hold, and to log the planned
// footprint before a merge.
#include <algorithm>
#include <cstdint>

#include "rawr/raw_gpu_pipeline/HdrPlusRecorder.h"
#include "rawr/raw_gpu_pipeline/RextureRecorder.h"
#include "rawr/raw_gpu_pipeline/ResourceGeometry.h"
#include "rawr/raw_gpu_pipeline/ScratchLayout.h"

namespace rawrcam::capture::multiframe {

// Logical image+buffer bytes of the merge arena of `algorithm` (MultiframeTuning::mergeAlgorithm: 4 = Rexture, 2 = HDR+
// Quality) at this geometry; any other id gives the larger of the two. Returns 0 for a geometry the layout rejects; callers
// treat 0 as "unknown" and do not gate on it.
inline std::uint64_t estimateMergeArenaBytes(std::uint32_t width, std::uint32_t height,
                                             std::uint32_t algorithm = 0u) noexcept {
    if (width == 0 || height == 0 || (width & 1u) != 0 || (height & 1u) != 0) return 0;
    try {
        const auto rexture = [&] {
            const auto layout = rawr::raw_gpu_pipeline::makeRextureScratchLayout(
                rawr::raw_gpu_pipeline::makeMultiframeGeometry(width, height, 1.0f));
            return layout.logicalImageBytes + layout.logicalBufferBytes;
        };
        const auto quality = [&] {
            const auto layout = rawr::raw_gpu_pipeline::makeHdrPlusFrequencyScratchLayout(
                rawr::raw_merge_hdrplus_gpu::makeFrequencyGeometry(width, height, rawr::raw_merge_hdrplus_gpu::Config{}),
                width, height);
            return layout.logicalImageBytes + layout.logicalBufferBytes;
        };
        if (algorithm == 4u) return rexture();
        if (algorithm == 2u) return quality();
        return std::max(rexture(), quality());
    } catch (...) {
        return 0;
    }
}

}  // namespace rawrcam::capture::multiframe
