#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace rawrcam::color {

// Auto scene key exposure: a render-side digital lift on top of the AE
// post-gain (AePostGainCurve.h). In high-contrast scenes the HAL underexposes
// the RAW to protect highlights and expects ISP local tone mapping to lift
// the rest; this places the scene's midtones instead. Sensor exposure stays
// with Camera2.
//
// The key is measured on the scene-linear preview image before any gain
// (image_scopes SceneExposureStatsData), so the lift never feeds back into
// its own measurement.
enum class AutoKeyMode : int { Off = 0, Natural = 1, Strong = 2 };

struct AutoKeyPolicy {
    float targetLuma;  // rendered scene-linear luma the key is placed at
    float maxLiftEv;   // largest lift; the policy only lifts, never darkens
};

constexpr AutoKeyPolicy autoKeyPolicy(AutoKeyMode mode) noexcept {
    switch (mode) {
        case AutoKeyMode::Natural: return {0.12f, 2.0f};
        case AutoKeyMode::Strong: return {0.18f, 3.0f};
        case AutoKeyMode::Off: break;
    }
    return {0.18f, 0.0f};
}

constexpr AutoKeyMode autoKeyModeFromId(int id) noexcept {
    return id == 1 ? AutoKeyMode::Natural : id == 2 ? AutoKeyMode::Strong : AutoKeyMode::Off;
}

// Histogram layout shared with image_scopes::SceneExposureStatsData.
inline constexpr std::size_t kSceneKeyBins = 256;
inline constexpr float kSceneKeyMinEv = -20.0f;
inline constexpr float kSceneKeyMaxEv = 4.0f;
// A large sky pulls a mean up; a trimmed weighted median does not.
inline constexpr float kSceneKeyTrimLow = 0.01f;
inline constexpr float kSceneKeyTrimHigh = 0.02f;

// Weighted median of log2 luma after trimming the bottom 1% and top 2% of
// weight, interpolated inside its bin. nullopt for an empty histogram.
inline std::optional<float> sceneKeyEv(const std::uint32_t* weightedBins, std::size_t binCount = kSceneKeyBins,
                                       float minEv = kSceneKeyMinEv, float maxEv = kSceneKeyMaxEv) noexcept {
    if (!weightedBins || binCount == 0 || !(maxEv > minEv)) return std::nullopt;
    double total = 0.0;
    for (std::size_t b = 0; b < binCount; ++b) total += weightedBins[b];
    if (!(total > 0.0)) return std::nullopt;
    const double lo = kSceneKeyTrimLow * total;
    const double hi = (1.0 - kSceneKeyTrimHigh) * total;
    const double target = lo + 0.5 * (hi - lo);
    const double binWidth = double(maxEv - minEv) / double(binCount);
    double before = 0.0;
    for (std::size_t b = 0; b < binCount; ++b) {
        const double w = weightedBins[b];
        if (w > 0.0 && before + w >= target) {
            const double within = std::clamp((target - before) / w, 0.0, 1.0);
            return static_cast<float>(minEv + (double(b) + within) * binWidth);
        }
        before += w;
    }
    return maxEv;
}

// Lift that brings the rendered key (scene key times the HAL post-gain) to
// the policy target, clamped to [0, maxLiftEv].
inline float autoKeyTargetEv(float keyEv, float halGain, const AutoKeyPolicy& policy) noexcept {
    if (!std::isfinite(keyEv) || !(halGain > 0.0f) || !(policy.maxLiftEv > 0.0f) || !(policy.targetLuma > 0.0f))
        return 0.0f;
    const float lift = std::log2(policy.targetLuma) - (keyEv + std::log2(halGain));
    return std::clamp(lift, 0.0f, policy.maxLiftEv);
}

// Exponential smoothing in the EV domain with a deadband so the lift does
// not shimmer on small key changes. The first update after reset() lands
// directly on the target so a fresh session does not fade in.
class AutoKeySmoother {
   public:
    static constexpr float kTimeConstantSec = 0.3f;
    static constexpr float kRecordingTimeConstantSec = 1.0f;
    static constexpr float kDeadbandEv = 0.1f;
    static constexpr float kMaxStepSec = 0.25f;

    float update(float targetEv, std::uint64_t timestampNs, bool recording) noexcept {
        if (!std::isfinite(targetEv)) return ev_;
        if (!primed_) {
            primed_ = true;
            lastNs_ = timestampNs;
            ev_ = targetEv;
            return ev_;
        }
        const float dt =
            timestampNs > lastNs_ ? std::min(static_cast<float>(timestampNs - lastNs_) * 1.0e-9f, kMaxStepSec) : 0.0f;
        lastNs_ = std::max(lastNs_, timestampNs);
        const float error = targetEv - ev_;
        if (std::abs(error) < kDeadbandEv) return ev_;
        const float tau = recording ? kRecordingTimeConstantSec : kTimeConstantSec;
        ev_ += error * (1.0f - std::exp(-dt / tau));
        return ev_;
    }
    void reset() noexcept {
        ev_ = 0.0f;
        lastNs_ = 0;
        primed_ = false;
    }
    [[nodiscard]] float value() const noexcept { return ev_; }

   private:
    float ev_ = 0.0f;
    std::uint64_t lastNs_ = 0;
    bool primed_ = false;
};

}  // namespace rawrcam::color
