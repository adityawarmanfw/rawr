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

    // ZSL frames at 1/30 s, then two post-shutter frames 2 EV darker.
    std::vector<FrameMetadataSnapshot> burst(6, frame(33'333'333, 400));
    burst.push_back(frame(8'333'333, 400));
    burst.push_back(frame(8'400'000, 400));  // within 1/8 EV of the darkest
    assert(isExposureBracketed(burst));
    assert((darkestFrames(burst) == std::vector<std::uint32_t>{6u, 7u}));
    assert(bracketReference(burst, {}, 3u) == 7u);  // middle of the dark group
    std::vector<float> scores(burst.size(), 1.f);
    scores[6] = 2.f;
    assert(bracketReference(burst, scores, 3u) == 6u);  // sharpest dark frame
    assert(std::fabs(bracketLiftEv(burst, 6u) - 2.0f) < 1e-3f);

    // Uniform burst: not bracketed, no lift.
    std::vector<FrameMetadataSnapshot> uniform(5, frame(33'333'333, 400));
    assert(!isExposureBracketed(uniform));
    assert(bracketLiftEv(uniform, 2u) == 0.0f);
    // Unknown exposures keep the fallback reference.
    assert(bracketReference(std::vector<FrameMetadataSnapshot>(3), {}, 1u) == 1u);
    // Headroom lift: reference at 1/4 of the Camera2 AE exposure -> 2 EV.
    assert(std::fabs(headroomLiftEv(float(33'333'333.0 * 400.0), frame(8'333'333, 400)) - 2.0f) < 1e-3f);
    assert(headroomLiftEv(0.0f, frame(8'333'333, 400)) == 0.0f);
    assert(headroomLiftEv(1.0f, frame(8'333'333, 400)) == 0.0f);  // never darkens
    // Underexposed burst plus brighter long frames: the burst is the reference.
    {
        std::vector<FrameMetadataSnapshot> burst2(6, frame(8'333'333, 400));
        burst2.push_back(frame(33'333'333, 400));
        burst2.push_back(frame(66'666'667, 400));
        assert(isExposureBracketed(burst2));
        assert(bracketReference(burst2, {}, 3u) == 3u);
    }
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
