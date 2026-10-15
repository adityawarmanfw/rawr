#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

#include "color/AutoKeyExposure.h"

namespace rawrcam::color {

// HDR+ Bracketed highlight meter. The ZSL burst is exposed below Camera2 AE
// by a headroom (EV <= 0) so the scene's brightest meaningful highlights sit
// just under the sensor clip; the viewfinder and stills lift it back.
//
// Measured on the same scene-linear preview histogram as AutoKey
// (image_scopes SceneExposureStatsData: log2 luma bins, clipped samples
// counted separately at clipLuma). Closed loop: each frame reports the
// headroom it was exposed with, so the meter only needs the highlight
// distance to clip on that frame.
struct HdrHeadroomPolicy {
    // Highlight percentile kept below clip (top 0.2% of weighted samples may
    // clip: speculars and light sources are not worth darkening the burst).
    float highlightPercentile = 0.998f;
    // Kept between that percentile and clipLuma (itself 0.9 x white luma).
    // Luma under-reports single-channel clipping in saturated highlights.
    float marginEv = 0.3f;
    // Clipped weight above this fraction steps down without a percentile.
    float clippedFraction = 0.004f;
    float clipStepEv = 1.0f;
    // Smaller corrections are ignored, so the viewfinder does not pump.
    float deadbandEv = 0.5f;
    // Camera2 AE needs a few frames to settle after a compensation change.
    std::uint64_t holdNs = 500'000'000ull;
};

class HdrHeadroomMeter {
   public:
    // appliedEv: headroom (<= 0) the measured frame was exposed with.
    // floorEv: most underexposure allowed (< 0). Returns a new headroom
    // request when it should change.
    std::optional<float> update(const std::uint32_t* weightedBins, std::uint32_t totalWeight,
                                std::uint32_t clippedWeight, float clipLumaEv, float appliedEv, float floorEv,
                                std::uint64_t timestampNs, const HdrHeadroomPolicy& policy = {}) noexcept {
        if (!weightedBins || totalWeight == 0 || !std::isfinite(clipLumaEv) || !std::isfinite(appliedEv) ||
            !(floorEv < 0.0f))
            return std::nullopt;
        if (primed_ && timestampNs < lastChangeNs_ + policy.holdNs) return std::nullopt;
        const float clipped = float(clippedWeight) / float(totalWeight);
        float desired = appliedEv;
        if (clipped > policy.clippedFraction) {
            desired = appliedEv - policy.clipStepEv;
        } else {
            const auto high = scenePercentileEv(weightedBins, policy.highlightPercentile);
            if (!high) return std::nullopt;
            desired = appliedEv + (clipLumaEv - *high - policy.marginEv);
        }
        desired = std::clamp(desired, floorEv, 0.0f);
        if (primed_ && std::abs(desired - target_) < policy.deadbandEv &&
            !(clipped > policy.clippedFraction && desired < target_))
            return std::nullopt;
        primed_ = true;
        target_ = desired;
        lastChangeNs_ = timestampNs;
        return target_;
    }
    void reset() noexcept {
        target_ = 0.0f;
        lastChangeNs_ = 0;
        primed_ = false;
    }
    [[nodiscard]] float target() const noexcept { return target_; }

   private:
    float target_ = 0.0f;
    std::uint64_t lastChangeNs_ = 0;
    bool primed_ = false;
};

}  // namespace rawrcam::color
