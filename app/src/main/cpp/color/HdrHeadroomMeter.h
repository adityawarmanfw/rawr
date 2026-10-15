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
// (image_scopes SceneExposureStatsData: log2 luma bins, samples at or above
// clipLuma = 0.9 x white luma land in the top bin). Closed loop: each frame
// reports the headroom it was exposed with, so the meter only needs the
// highlight distance to clip on that frame.
//
// One measurement drives both directions (two disagreeing tests made the
// headroom cycle and the viewfinder pump). Moving toward clip is slow and
// remembers the last level that clipped; moving away from clip is quick.
struct HdrHeadroomPolicy {
    // Highlight percentile kept below clip: the top 0.05% (speculars, light
    // sources) may clip.
    float highlightPercentile = 0.9995f;
    // Luma under-reports single-channel clipping (a blue sky clips blue and
    // green at ~70% of white luma): keep the percentile this far below clipLuma.
    float channelMarginEv = 0.6f;
    // Only move toward clip with at least this much room, and keep this
    // cushion when doing so.
    float raiseDeadbandEv = 0.7f;
    float raiseCushionEv = 0.3f;
    // Over clip by less than this is tolerated (meter noise).
    float lowerToleranceEv = 0.1f;
    // A clipped percentile does not say by how much: step at least this far.
    float clippedStepEv = 1.0f;
    // Camera2 AE needs a few frames to settle after a compensation change.
    std::uint64_t lowerHoldNs = 600'000'000ull;
    std::uint64_t raiseHoldNs = 2'000'000'000ull;
    // After clipping, stay this far below the level that clipped for a while.
    float clipMemoryMarginEv = 0.3f;
    std::uint64_t clipMemoryNs = 6'000'000'000ull;
};

class HdrHeadroomMeter {
   public:
    // appliedEv: headroom (<= 0) the measured frame was exposed with.
    // floorEv: most underexposure allowed (< 0). Returns a new headroom
    // request when it should change.
    std::optional<float> update(const std::uint32_t* weightedBins, std::uint32_t totalWeight, float clipLumaEv,
                                float appliedEv, float floorEv, std::uint64_t timestampNs,
                                const HdrHeadroomPolicy& policy = {}) noexcept {
        if (!weightedBins || totalWeight == 0 || !std::isfinite(clipLumaEv) || !std::isfinite(appliedEv) ||
            !(floorEv < 0.0f))
            return std::nullopt;
        const auto high = scenePercentileEv(weightedBins, policy.highlightPercentile);
        if (!high) return std::nullopt;
        // Room between the highlight percentile and the effective clip; <= 0
        // when it is clipped (clipped samples sit in the clipLuma bin).
        const float room = (clipLumaEv - policy.channelMarginEv) - *high;
        const bool clipped = *high >= clipLumaEv - 0.05f;
        const std::uint64_t since = primed_ && timestampNs > lastChangeNs_ ? timestampNs - lastChangeNs_ : 0;
        float desired = target_;
        if (room < -policy.lowerToleranceEv) {
            if (primed_ && since < policy.lowerHoldNs) return std::nullopt;
            desired = appliedEv + (clipped ? std::min(room, -policy.clippedStepEv) : room);
            clipCeilingEv_ = appliedEv;
            clipCeilingNs_ = timestampNs;
            hasClipCeiling_ = true;
        } else if (room > policy.raiseDeadbandEv) {
            if (primed_ && since < policy.raiseHoldNs) return std::nullopt;
            desired = appliedEv + room - policy.raiseCushionEv;
            if (hasClipCeiling_ && timestampNs < clipCeilingNs_ + policy.clipMemoryNs)
                desired = std::min(desired, clipCeilingEv_ - policy.clipMemoryMarginEv);
        } else {
            return std::nullopt;
        }
        desired = std::clamp(desired, floorEv, 0.0f);
        if (std::abs(desired - target_) < 0.05f && std::abs(desired - appliedEv) < 0.05f) return std::nullopt;
        primed_ = true;
        target_ = desired;
        lastChangeNs_ = timestampNs;
        return target_;
    }
    void reset() noexcept { *this = HdrHeadroomMeter{}; }
    [[nodiscard]] float target() const noexcept { return target_; }

   private:
    float target_ = 0.0f;
    std::uint64_t lastChangeNs_ = 0;
    bool primed_ = false;
    float clipCeilingEv_ = 0.0f;
    std::uint64_t clipCeilingNs_ = 0;
    bool hasClipCeiling_ = false;
};

}  // namespace rawrcam::color
