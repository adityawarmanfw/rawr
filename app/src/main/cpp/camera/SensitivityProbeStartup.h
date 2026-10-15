#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>

#include "metadata/FrameMetadataSnapshot.h"

namespace rawrcam::camera {

// Startup ISO calibration probe for forced (DCG) sensor modes. The viewfinder
// shows from the first frame; only frames that would look wrong are held back
// (the viewfinder keeps its last image): the probe frame itself, and frames
// after it whose exposure jumped away from the pre-probe exposure while AE
// recovers (at most kRecoveryLimit). All decisions use CaptureResult
// metadata; RAW images are still paired and released promptly.
class SensitivityProbeStartup {
   public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto kTimeout = std::chrono::seconds(3);
    static constexpr auto kRecoveryLimit = std::chrono::milliseconds(500);
    static constexpr double kRecoveredEv = 0.125;
    struct Decision {
        bool suppressPreview = false;
        bool requestProbe = false;
    };
    void reset() { *this = SensitivityProbeStartup{}; }
    bool finished() const { return phase_ == Phase::Ready; }
    bool timedOut() const { return timedOut_; }

    Decision observe(const metadata::FrameMetadataSnapshot& frame, bool eligible, bool calibrated,
                     bool autoSettled, Clock::time_point now) {
        (void)autoSettled;
        const bool probe = frame.optimizedStillRequestId == metadata::kSensitivityProbeRequestId;
        if (phase_ == Phase::Ready)
            return {probe || (probeTimestamp_ != 0 && frame.timestampNs > probeTimestamp_ &&
                              frame.timestampNs < visibleFrom_),
                    false};
        if (!started_) {
            started_ = true;
            began_ = now;
        }
        // Once a probe is in flight, keep draining its transition even if the
        // setting was switched off. It must never leak into preview/stills.
        if (phase_ == Phase::Warmup && (!eligible || calibrated)) return reveal(frame, probe);
        if (now - began_ >= kTimeout) {
            timedOut_ = true;
            return reveal(frame, probe);
        }
        if (probe) {
            phase_ = Phase::Recovering;
            probeTimestamp_ = frame.timestampNs;
            recoveryBegan_ = now;
            return {true, false};
        }
        if (frame.optimizedStillRequestId.value_or(0) != 0) return {true, false};
        if (frame.timestampNs <= lastTimestamp_) return {true, false};
        lastTimestamp_ = frame.timestampNs;
        const double exposure = double(frame.exposureTimeNs) * double(frame.sensitivity);
        if (phase_ == Phase::Warmup) {
            if (!(exposure > 0.0)) return {};
            if (++warmupFrames_ < 3) return {};
            phase_ = Phase::AwaitingProbe;
            reference_ = exposure;
            return {false, true};
        }
        if (phase_ == Phase::AwaitingProbe || frame.timestampNs <= probeTimestamp_) {
            if (exposure > 0.0) reference_ = exposure;
            return {};
        }
        // Recovering: the probe's manual request can disturb AE for a few frames.
        const bool jumped = !(exposure > 0.0) || !(reference_ > 0.0) ||
                            std::abs(std::log2(exposure / reference_)) > kRecoveredEv;
        if (!jumped || now - recoveryBegan_ >= kRecoveryLimit) return reveal(frame, false);
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
    Clock::time_point began_{}, recoveryBegan_{};
    uint64_t lastTimestamp_ = 0, probeTimestamp_ = 0, visibleFrom_ = 0;
    unsigned warmupFrames_ = 0;
    double reference_ = 0.0;
};
}  // namespace rawrcam::camera
