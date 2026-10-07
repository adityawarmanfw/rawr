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
    std::puts("bracket_exposure_test OK");
    return 0;
}
