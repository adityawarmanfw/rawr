#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "tonemap/AdaptiveTone.h"

namespace rawrcam::color {

// Auto scene key exposure: an adaptive luminance curve after the AE
// post-gain (AePostGainCurve.h), with highlights anchored at their original EV.
// In high-contrast scenes the HAL underexposes
// the RAW to protect highlights and expects ISP local tone mapping to lift
// the rest; this places the scene's midtones instead. Sensor exposure stays
// with Camera2.
//
// The key is measured on the scene-linear preview image before any gain
// (image_scopes SceneExposureStatsData), so the lift never feeds back into
// its own measurement.
// Ids are persisted/JNI values; Medium was added later, hence id 3.
enum class AutoKeyMode : int { Off = 0, Natural = 1, Strong = 2, Medium = 3 };

struct AutoKeyPolicy {
    float targetLuma;  // rendered scene-linear luma the key is placed at
    float maxLiftEv;   // largest lift; the policy only lifts, never darkens
};

constexpr AutoKeyPolicy autoKeyPolicy(AutoKeyMode mode) noexcept {
    switch (mode) {
        case AutoKeyMode::Natural: return {0.075f, 1.0f};
        // Geometric midpoint of Natural and Strong targets.
        case AutoKeyMode::Medium: return {0.116f, 2.0f};
        case AutoKeyMode::Strong: return {0.18f, 3.0f};
        case AutoKeyMode::Off: break;
    }
    return {0.18f, 0.0f};
}

constexpr AutoKeyMode autoKeyModeFromId(int id) noexcept {
    switch (id) {
        case 1: return AutoKeyMode::Natural;
        case 2: return AutoKeyMode::Strong;
        case 3: return AutoKeyMode::Medium;
        default: return AutoKeyMode::Off;
    }
}

// App enum names (AutoKeyExposure in Kotlin), as recipes store them.
inline std::optional<AutoKeyMode> autoKeyModeFromName(std::string_view name) noexcept {
    if (name == "Off") return AutoKeyMode::Off;
    if (name == "Natural") return AutoKeyMode::Natural;
    if (name == "Medium") return AutoKeyMode::Medium;
    if (name == "Strong") return AutoKeyMode::Strong;
    return std::nullopt;
}

// Histogram layout shared with image_scopes::SceneExposureStatsData.
inline constexpr std::size_t kSceneKeyBins = 256;
inline constexpr float kSceneKeyMinEv = -20.0f;
inline constexpr float kSceneKeyMaxEv = 4.0f;
// A large sky pulls a mean up; a trimmed weighted median does not.
inline constexpr float kSceneKeyTrimLow = 0.01f;
inline constexpr float kSceneKeyTrimHigh = 0.02f;

// Weighted percentile of log2 luma, interpolated inside its bin.
// nullopt for an empty histogram.
inline std::optional<float> scenePercentileEv(const std::uint32_t* weightedBins, float percentile,
                                              std::size_t binCount = kSceneKeyBins, float minEv = kSceneKeyMinEv,
                                              float maxEv = kSceneKeyMaxEv) noexcept {
    if (!weightedBins || binCount == 0 || !(maxEv > minEv)) return std::nullopt;
    double total = 0.0;
    for (std::size_t b = 0; b < binCount; ++b) total += weightedBins[b];
    if (!(total > 0.0)) return std::nullopt;
    const double target = std::clamp(percentile, 0.0f, 1.0f) * total;
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

// Median after trimming the bottom 1% and top 2% of weight.
inline std::optional<float> sceneKeyEv(const std::uint32_t* bins, std::size_t count = kSceneKeyBins,
                                       float minEv = kSceneKeyMinEv, float maxEv = kSceneKeyMaxEv) noexcept {
    return scenePercentileEv(bins, kSceneKeyTrimLow + 0.5f * (1.0f - kSceneKeyTrimHigh - kSceneKeyTrimLow), count,
                             minEv, maxEv);
}

// Lift that brings the rendered key (scene key times the HAL post-gain) to
// the policy target, clamped to [0, maxLiftEv].
inline float autoKeyTargetEv(float keyEv, float halGain, const AutoKeyPolicy& policy) noexcept {
    if (!std::isfinite(keyEv) || !(halGain > 0.0f) || !(policy.maxLiftEv > 0.0f) || !(policy.targetLuma > 0.0f))
        return 0.0f;
    const float lift = std::log2(policy.targetLuma) - (keyEv + std::log2(halGain));
    return std::clamp(lift, 0.0f, policy.maxLiftEv);
}

// Protect the upper scene (P95), bounded to diffuse white / upper midtones.
// The floor avoids pinning an entirely dark scene at its dark P95; the ceiling
// ensures HDR/specular values never push the protected range beyond 1.0.
inline tonemap::AdaptiveToneParams autoKeyCurve(float keyEv, float highEv, float halGain,
                                                const AutoKeyPolicy& policy) noexcept {
    if (!std::isfinite(keyEv) || !std::isfinite(highEv) || !std::isfinite(halGain) || !(halGain > 0.0f)) return {};
    const float gainEv = std::log2(halGain);
    tonemap::AdaptiveToneParams p;
    p.keyEv = keyEv + gainEv;
    p.highlightEv = std::clamp(highEv + gainEv, std::log2(0.36f), 0.0f);
    p.liftEv = std::min(autoKeyTargetEv(keyEv, halGain, policy), tonemap::adaptiveToneMaxLift(p));
    return p;
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
        // No lift needed: keep decaying so the deadband cannot strand a residual lift.
        if (std::abs(error) < kDeadbandEv && targetEv > 0.0f) return ev_;
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
