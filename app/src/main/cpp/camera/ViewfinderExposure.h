#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "camera/CameraControlTypes.h"

namespace rawrcam::camera {

// Longest shutter the repeating (viewfinder) request runs. Longer M and rawr S
// shutters run only as a one-shot capture when the shutter is pressed; the
// viewfinder holds this and raises ISO for the difference (render gain covers
// whatever ISO cannot), so it previews the capture's brightness.
inline constexpr int64_t kViewfinderShutterMaxNs = 200'000'000;  // 1/5 s

// Longest shutter multiframe still merges: past the viewfinder cap the burst
// is taken after the press at that shutter; longer shutters take one frame.
// A little over 1 s: shutters the controls show as "1" (e.g. 1.03 s) still stack.
inline constexpr int64_t kStackShutterMaxNs = 1'100'000'000;  // 1 s

struct SensorExposurePair {
    int64_t exposureTimeNs = 0;
    int32_t sensitivity = 0;
};

// M's repeating-request pair for the requested (capture) pair.
inline SensorExposurePair viewfinderManualPair(const CameraControlState& s) {
    SensorExposurePair pair{s.requestedExposureTimeNs, s.requestedSensitivity};
    if (pair.exposureTimeNs <= kViewfinderShutterMaxNs) return pair;
    const auto& caps = s.capabilities;
    const double boosted = double(pair.sensitivity) * double(pair.exposureTimeNs) / double(kViewfinderShutterMaxNs);
    pair.sensitivity = int32_t(std::clamp<double>(std::llround(boosted), std::max(1, caps.sensitivityMin),
                                                  std::max(caps.sensitivityMin, caps.sensitivityMax)));
    pair.exposureTimeNs = kViewfinderShutterMaxNs;
    return pair;
}

// True when the requested shutter is longer than the viewfinder runs: a
// capture needs its own one-shot exposure.
inline bool deferredLongExposure(int64_t exposureTimeNs) { return exposureTimeNs > kViewfinderShutterMaxNs; }

}  // namespace rawrcam::camera
