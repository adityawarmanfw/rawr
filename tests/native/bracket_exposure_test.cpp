#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

#include "capture/multiframe/BracketExposure.h"

using namespace rawrcam::capture::multiframe;
using rawrcam::metadata::FrameMetadataSnapshot;

namespace {
FrameMetadataSnapshot frame(long long exposureNs, int iso, int boost = 100) {
    FrameMetadataSnapshot m{};
    m.exposureTimeNs = exposureNs;
    m.sensitivity = iso;
    m.postRawSensitivityBoost = boost;
    return m;
}
}  // namespace

int main() {
    // The post-RAW boost is not part of the RAW signal.
    assert(linearExposure(frame(10'000'000, 100, 400)) == linearExposure(frame(10'000'000, 100)));
    // Request-copy fallback when the result is missing.
    FrameMetadataSnapshot requested{};
    requested.requestedExposureTimeNs = 5'000'000;
    requested.requestedSensitivity = 200;
    assert(linearExposure(requested) == 5'000'000.0 * 200.0);
    assert(linearExposure(FrameMetadataSnapshot{}) == 0.0);

    std::vector<FrameMetadataSnapshot> uniform(5, frame(33'333'333, 400));
    // Merged noise: a uniform burst of N averages both terms by 1/N.
    {
        const auto scale = mergedNoiseVarianceScale(uniform, 2u);
        assert(std::fabs(scale[0] - 0.2) < 1e-9 && std::fabs(scale[1] - 0.2) < 1e-9);
    }
    // Reference + one frame 4x longer at the same ISO: weights 1 and 4,
    // shot (1 + 4) / 25, read (1 + 1) / 25.
    {
        std::vector<FrameMetadataSnapshot> pair{frame(10'000'000, 100), frame(40'000'000, 100)};
        const auto scale = mergedNoiseVarianceScale(pair, 0u);
        assert(std::fabs(scale[0] - 5.0 / 25.0) < 1e-9 && std::fabs(scale[1] - 2.0 / 25.0) < 1e-9);
    }
    // Same 4x exposure reached with 4x gain: shot 1+16, read 1+16, over 25.
    {
        std::vector<FrameMetadataSnapshot> pair{frame(10'000'000, 100), frame(10'000'000, 400)};
        const auto scale = mergedNoiseVarianceScale(pair, 0u);
        assert(std::fabs(scale[0] - 17.0 / 25.0) < 1e-9 && std::fabs(scale[1] - 17.0 / 25.0) < 1e-9);
    }
    // Unknown metadata keeps the single-frame profile.
    {
        const auto scale = mergedNoiseVarianceScale(std::vector<FrameMetadataSnapshot>(3), 1u);
        assert(scale[0] == 1.0 && scale[1] == 1.0);
    }
    std::puts("bracket_exposure_test OK");
    return 0;
}
