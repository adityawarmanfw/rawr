#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

namespace rawrcam::camera {

// Deterministic camera policy. The adapter supplies monotonic time and executes
// decisions on the same serial lane as surface/configuration changes. Capture
// callbacks only publish frame time; they never start or retire a session.
class CameraSessionPolicy final {
   public:
    enum class Action { None, Start, Stop, Restart };

    void setForeground(bool value, int64_t now) {
        if (value && !foreground_ && !configReady_) configDeadline_ = now + 1500;
        if (value && !foreground_) runningSince_ = now;
        foreground_ = value;
        if (!value) restartDeadline_.reset();
    }
    void setSurfaceReady(bool value) {
        surfaceReady_ = value;
        if (!value) restartDeadline_.reset();
    }
    void setConfigReady() { configReady_ = true; }
    void beginStill() { ++stillLeases_; }
    void endStill() {
        if (stillLeases_ > 0) --stillLeases_;
    }
    void setStillLeases(unsigned count) { stillLeases_ = count; }
    bool canRecoverStills() const { return foreground_ && surfaceReady_; }

    void setZeroCopy(bool value, int64_t now) {
        const bool changed = !zeroCopy_ || *zeroCopy_ != value;
        zeroCopy_ = value;
        if (changed && running_ && foreground_ && surfaceReady_) {
            if (!restartDeadline_) restartDeadline_ = now + 400;
            restartForRecovery_ = false;
        }
    }

    // Rebuilds the running session after a short settle, e.g. so a new RAW
    // reader picks up different buffer usage. No-op while a restart is pending.
    void requestRestart(int64_t now) {
        if (!running_ || !foreground_ || !surfaceReady_ || restartDeadline_) return;
        restartDeadline_ = now + 400;
        restartForRecovery_ = false;
    }

    Action advance(int64_t now, int64_t lastFrameTime) {
        if (!configReady_ && configDeadline_ && now >= *configDeadline_) configReady_ = true;
        const bool requested = foreground_ && surfaceReady_ && configReady_;
        if (!requested) {
            if (!running_ || stillLeases_ != 0) return Action::None;
            running_ = false;
            restartDeadline_.reset();
            return Action::Stop;
        }
        if (!running_) {
            running_ = true;
            runningSince_ = now;
            return Action::Start;
        }
        // A still owns the current session until its final lease is released.
        if (stillLeases_ != 0) return Action::None;
        if (restartDeadline_ && restartForRecovery_ && lastRecovery_ && lastFrameTime >= *lastRecovery_)
            restartDeadline_.reset();
        if (restartDeadline_ && now >= *restartDeadline_) {
            restartDeadline_.reset();
            runningSince_ = now;
            return Action::Restart;
        }
        // Unlike a retained exposure snapshot, a monotonic result timestamp
        // detects both failed startup and a session that later goes silent.
        const auto lastActivity = std::max(runningSince_, lastFrameTime);
        if (!restartDeadline_ && now - lastActivity >= 8000 && (!lastRecovery_ || now - *lastRecovery_ >= 5000)) {
            lastRecovery_ = now;
            restartDeadline_ = now + 400;
            restartForRecovery_ = true;
        }
        return Action::None;
    }

    // Explicit teardown/offline replay bypasses leases. Keep attachment and
    // configuration facts so a later foreground input can resume normally.
    void forceStopped() {
        foreground_ = false;
        running_ = false;
        stillLeases_ = 0;
        restartDeadline_.reset();
    }
    void activationCompleted(int64_t now) { runningSince_ = now; }

   private:
    bool foreground_ = false;
    bool surfaceReady_ = false;
    bool configReady_ = false;
    bool running_ = false;
    bool restartForRecovery_ = false;
    unsigned stillLeases_ = 0;
    int64_t runningSince_ = 0;
    std::optional<int64_t> configDeadline_;
    std::optional<int64_t> restartDeadline_;
    std::optional<int64_t> lastRecovery_;
    std::optional<bool> zeroCopy_;
};

}  // namespace rawrcam::camera
