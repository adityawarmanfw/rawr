#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace rawrcam::camera {

// Software shutter/ISO priority for cameras whose HAL has no Camera2 AE priority modes.
//
// The camera runs AE-off (manual). The user owns one axis; this controller owns the other and
// closes the loop on the brightness of the *rendered* frame (median luma plus highlight clipping,
// measured on the GPU). It is a pure function of its inputs so it can be tested on the host.
//
// Why a damped, settled-frame loop: the camera applies a request a few frames late and the
// rendered measurement lags it further. Acting on every frame would overshoot and hunt, so a new
// correction is made only from a frame whose exposure product has caught up with the last command.
class SoftwareAe {
   public:
    enum class FreeAxis { Sensitivity, Shutter };

    struct Limits {
        int64_t shutterMinNs = 0;
        // Longest shutter the loop may pick on its own (ISO priority). A user-held shutter is not capped here.
        int64_t autoShutterMaxNs = 0;
        int64_t shutterMaxNs = 0;
        int32_t sensitivityMin = 0;
        int32_t sensitivityMax = 0;
    };

    struct Sample {
        int64_t exposureTimeNs = 0;  // exposure the measured frame was actually taken with
        int32_t sensitivity = 0;
        float lumaP50 = 0.0f;  // rendered (display referred) 0..1
        float lumaP95 = 0.0f;
        float clippedFraction = 0.0f;
    };

    struct Command {
        int64_t exposureTimeNs = 0;
        int32_t sensitivity = 0;
    };

    // Seed from the exposure the camera is using right now so entering a mode never jumps brightness.
    // calibrateFromNextSample: take the first measured median as the target (entering from Auto, where the
    // HAL already chose an exposure the user was happy with) instead of the fixed default.
    void reset(int64_t exposureTimeNs, int32_t sensitivity, bool calibrateFromNextSample) {
        commandedLogProduct_ = logProduct(exposureTimeNs, sensitivity);
        calibrate_ = calibrateFromNextSample;
        target_ = kDefaultTarget;
        unsettledFrames_ = 0;
        active_ = exposureTimeNs > 0 && sensitivity > 0;
    }

    // The held axis changed on purpose: adopt the new exposure product as the baseline, keep the learned target.
    void rebase(int64_t exposureTimeNs, int32_t sensitivity) {
        commandedLogProduct_ = logProduct(exposureTimeNs, sensitivity);
        unsettledFrames_ = 0;
    }

    void deactivate() { active_ = false; }
    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] float target() const noexcept { return target_; }

    // Returns the new free-axis exposure when a correction is due, otherwise nullopt.
    // evStops: exposure compensation the user dialled in (positive is brighter).
    [[nodiscard]] std::optional<Command> update(FreeAxis axis, int64_t heldShutterNs, int32_t heldSensitivity,
                                                float evStops, const Limits& limits, const Sample& s) {
        if (!active_ || s.exposureTimeNs <= 0 || s.sensitivity <= 0) return std::nullopt;
        if (!(s.lumaP50 >= 0.0f) || !std::isfinite(s.lumaP50)) return std::nullopt;

        const double frameLog = logProduct(s.exposureTimeNs, s.sensitivity);
        // The frame has not caught up with the last command: wait, but never forever (a HAL that quantizes or
        // clamps would otherwise stall the loop), then trust what the camera really did.
        if (std::fabs(frameLog - commandedLogProduct_) > kSettledTolerance && unsettledFrames_ < kMaxUnsettled) {
            ++unsettledFrames_;
            return std::nullopt;
        }
        unsettledFrames_ = 0;

        if (calibrate_) {
            // Ignore a frame that is clearly mis-exposed rather than adopting it as the goal.
            target_ = std::clamp(s.lumaP50, kCalibrationMin, kCalibrationMax);
            calibrate_ = false;
        }

        const double effectiveTarget =
            std::clamp(static_cast<double>(target_) * std::exp2(kLumaPerStop * static_cast<double>(evStops)),
                       static_cast<double>(kTargetFloor), static_cast<double>(kTargetCeil));
        const double measured = std::max(static_cast<double>(s.lumaP50), static_cast<double>(kMeasureFloor));
        double errorStops = std::log2(effectiveTarget / measured) / kLumaPerStop;
        // Highlight guard: clipped highlights cannot be seen by the median, so pull exposure down regardless.
        if (s.clippedFraction > kClipTolerance) {
            const double guard = -(0.25 + std::min(1.0, static_cast<double>(s.clippedFraction) * 8.0));
            errorStops = std::min(errorStops, guard);
        }
        if (std::fabs(errorStops) < kDeadbandStops) return std::nullopt;

        const double stepStops = std::clamp(errorStops * kGain, -kMaxStepStops, kMaxStepStops);
        const double wantedLog = frameLog + stepStops;
        const Command next = split(axis, heldShutterNs, heldSensitivity, wantedLog, limits);
        const double achievedLog = logProduct(next.exposureTimeNs, next.sensitivity);
        // Record what was really commanded (after clamping) so a pinned axis cannot wind the loop up.
        commandedLogProduct_ = achievedLog;
        if (std::fabs(achievedLog - frameLog) < kMinChangeStops) return std::nullopt;
        return next;
    }

   private:
    // Display luma follows roughly exposure^0.45 (sRGB-like encoding). Only the sign and rough size matter: the
    // damped, settled-frame loop converges for slopes within about a factor of two of this.
    static constexpr double kLumaPerStop = 0.5;
    static constexpr double kGain = 0.7;
    static constexpr double kMaxStepStops = 1.5;
    static constexpr double kDeadbandStops = 0.12;
    static constexpr double kMinChangeStops = 0.02;
    static constexpr double kSettledTolerance = 0.06;
    static constexpr int kMaxUnsettled = 12;
    static constexpr float kDefaultTarget = 0.40f;
    static constexpr float kCalibrationMin = 0.28f;
    static constexpr float kCalibrationMax = 0.55f;
    static constexpr float kTargetFloor = 0.05f;
    static constexpr float kTargetCeil = 0.90f;
    static constexpr float kMeasureFloor = 0.01f;
    static constexpr float kClipTolerance = 0.02f;

    static double logProduct(int64_t exposureNs, int32_t sensitivity) {
        return std::log2(static_cast<double>(std::max<int64_t>(exposureNs, 1)) *
                         static_cast<double>(std::max<int32_t>(sensitivity, 1)));
    }

    static Command split(FreeAxis axis, int64_t heldShutterNs, int32_t heldSensitivity, double wantedLog,
                         const Limits& limits) {
        const double product = std::exp2(wantedLog);
        Command out;
        if (axis == FreeAxis::Shutter) {
            // ISO priority: sensitivity is held, the loop picks the shutter.
            const int32_t iso = std::max<int32_t>(heldSensitivity, 1);
            const int64_t cap = std::max(limits.shutterMinNs, std::min(limits.autoShutterMaxNs, limits.shutterMaxNs));
            out.sensitivity = iso;
            out.exposureTimeNs = std::clamp<int64_t>(std::llround(product / static_cast<double>(iso)),
                                                     limits.shutterMinNs, cap);
        } else {
            // Shutter priority: shutter is held, the loop picks the sensitivity.
            const int64_t t = std::max<int64_t>(heldShutterNs, 1);
            out.exposureTimeNs = t;
            out.sensitivity = static_cast<int32_t>(std::clamp<int64_t>(
                std::llround(product / static_cast<double>(t)), limits.sensitivityMin,
                std::max(limits.sensitivityMin, limits.sensitivityMax)));
        }
        return out;
    }

    bool active_ = false;
    bool calibrate_ = false;
    float target_ = kDefaultTarget;
    double commandedLogProduct_ = 0.0;
    int unsettledFrames_ = 0;
};

}  // namespace rawrcam::camera
