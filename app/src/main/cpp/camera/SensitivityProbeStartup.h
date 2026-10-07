#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>

#include "metadata/FrameMetadataSnapshot.h"

namespace rawrcam::camera {

// Holds the first visible frame until calibration and the Auto -> manual ->
// Auto transition have finished. All decisions use CaptureResult metadata;
// RAW images are still paired and released promptly by the preview pipeline.
class SensitivityProbeStartup {
   public:
    using Clock = std::chrono::steady_clock;
    struct Decision {
        bool suppressPreview = false;
        bool requestProbe = false;
    };
    void reset() { *this = SensitivityProbeStartup{}; }
    bool finished() const { return phase_ == Phase::Ready; }
    bool timedOut() const { return timedOut_; }

    Decision observe(const metadata::FrameMetadataSnapshot& frame, bool eligible, bool calibrated,
                     bool autoSettled, Clock::time_point now) {
        const bool probe = frame.optimizedStillRequestId == metadata::kSensitivityProbeRequestId;
        if (phase_ == Phase::Ready)
            return {probe || (visibleFrom_ != 0 && frame.timestampNs < visibleFrom_), false};
        if (!started_) {
            started_ = true;
            began_ = now;
        }
        // Once a probe is in flight, keep draining its transition even if the
        // setting was switched off. It must never leak into preview/stills.
        if (phase_ == Phase::Warmup && (!eligible || calibrated)) return reveal(frame, probe);
        if (now - began_ >= std::chrono::seconds(3)) {
            timedOut_ = true;
            return reveal(frame, probe);
        }
        if (probe) {
            phase_ = Phase::Recovering;
            probeTimestamp_ = frame.timestampNs;
            stableFrames_ = 0;
            previousExposure_ = 0.0;
            return {true, false};
        }
        if (frame.optimizedStillRequestId.value_or(0) != 0) return {true, false};
        if (frame.timestampNs <= lastTimestamp_) return {true, false};
        lastTimestamp_ = frame.timestampNs;
        if (phase_ == Phase::Warmup) {
            if (frame.exposureTimeNs <= 0 || frame.sensitivity <= 0) return {true, false};
            if (++warmupFrames_ < 3) return {true, false};
            phase_ = Phase::AwaitingProbe;
            return {true, true};
        }
        if (phase_ == Phase::AwaitingProbe || frame.timestampNs <= probeTimestamp_) return {true, false};
        const double exposure = double(frame.exposureTimeNs) * double(frame.sensitivity);
        const bool stable = autoSettled && exposure > 0.0 &&
                            (previousExposure_ == 0.0 || std::abs(std::log2(exposure / previousExposure_)) <= 0.125);
        stableFrames_ = stable ? stableFrames_ + 1 : 0;
        previousExposure_ = exposure > 0.0 ? exposure : 0.0;
        if (stableFrames_ >= 3) return reveal(frame, false);
        return {true, false};
    }
    void submissionFailed() { phase_ = Phase::Ready; }

   private:
    enum class Phase { Warmup, AwaitingProbe, Recovering, Ready };
    Decision reveal(const metadata::FrameMetadataSnapshot& frame, bool probe) {
        phase_ = Phase::Ready;
        visibleFrom_ = frame.timestampNs;
        return {probe, false};
    }
    Phase phase_ = Phase::Warmup;
    bool started_ = false, timedOut_ = false;
    Clock::time_point began_{};
    uint64_t lastTimestamp_ = 0, probeTimestamp_ = 0, visibleFrom_ = 0;
    unsigned warmupFrames_ = 0, stableFrames_ = 0;
    double previousExposure_ = 0.0;
};
}  // namespace rawrcam::camera
