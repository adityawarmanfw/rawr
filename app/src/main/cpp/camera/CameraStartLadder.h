#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace rawrcam::camera {

// Recovery for a lens whose session configures but never delivers a RAW frame
// (observed with vendor sensor-mode keys: the session runs, then frames stop
// or the device errors). Applies only to starts whose companion YUV may be
// dropped (the same starts the configure-time RAW-only fallback covers).
//
// Steps, per lens, counted only before the lens's first frame:
//   1st failure: retry as configured. The RAW reader usage ladder can demote
//                the reader on this retry, so existing recovery goes first.
//   2nd failure: retry RAW-only (no companion).
//   RAW-only failure: give up. The lens reverts to its configured session for
//                the next user retry (lens or profile switch) instead of
//                staying demoted.
// A failure is a device error, kFailedCapturesToCount failed captures, or no
// frame by the deadline. A frame proves the lens: later failures go to the
// regular restart paths, and the lens is never demoted.
// Pure state: the caller supplies time and owns synchronization.
class CameraStartLadder final {
   public:
    static constexpr uint32_t kFailedCapturesToCount = 3;
    static constexpr int64_t kMinFirstFrameMs = 5000;  // After the reader-usage ladder's 4 s deadline.
    static constexpr int64_t kExposureMarginMs = 1000;

    enum class Plan { AsConfigured, RawOnly, GaveUp };
    enum class Outcome { None, Retry, RetryRawOnly, GaveUp };

    // A profile companion is part of the sensor mode, never a recovery option.
    static bool eligible(bool companionByRule, bool photoStream, bool profileCompanion, bool forced) {
        return companionByRule && !photoStream && !profileCompanion && !forced;
    }

    // Time a start gets for its first frame: never less than kMinFirstFrameMs,
    // and a long viewfinder exposure gets its full frame time plus a margin.
    static int64_t firstFrameDeadlineMs(int64_t exposureNs) {
        return std::max(kMinFirstFrameMs, exposureNs / 1000000 + kExposureMarginMs);
    }

    // A session start for lensKey. eligible = the start carries a companion
    // the ladder may drop; ineligible starts are left alone.
    Plan begin(const std::string& lensKey, uint64_t generation, int64_t nowMs, int64_t exposureNs, bool eligible) {
        current_ = {};
        auto& lens = lenses_[lensKey];
        if (lens.gaveUp) return Plan::GaveUp;
        if (!eligible && !lens.rawOnly) return Plan::AsConfigured;
        current_ = {lensKey, generation, nowMs + firstFrameDeadlineMs(exposureNs), 0, !lens.proven};
        return lens.rawOnly ? Plan::RawOnly : Plan::AsConfigured;
    }

    // The configure itself was rejected with the companion: RAW-only next.
    void configureRejected(const std::string& lensKey) {
        auto& lens = lenses_[lensKey];
        lens.rawOnly = true;
        lens.failures = 0;
    }

    void firstFrame(uint64_t generation) {
        if (!watching(generation)) return;
        lenses_[current_.lensKey].proven = true;
        lenses_[current_.lensKey].failures = 0;
        current_.watching = false;
    }

    Outcome deviceFailed(uint64_t generation) { return watching(generation) ? fail() : Outcome::None; }

    Outcome captureFailed(uint64_t generation) {
        if (!watching(generation)) return Outcome::None;
        return ++current_.failedCaptures >= kFailedCapturesToCount ? fail() : Outcome::None;
    }

    Outcome tick(uint64_t generation, int64_t nowMs) {
        return watching(generation) && nowMs >= current_.deadlineMs ? fail() : Outcome::None;
    }

    // A user action (lens or profile switch) gives every given-up lens another
    // try. Restarts can't tell a user reopen from a watchdog restart, so only
    // explicit switches reset.
    void resetGivenUp() {
        for (auto& [key, lens] : lenses_)
            if (lens.gaveUp) lens = {};
    }

    [[nodiscard]] bool watching(uint64_t generation) const {
        return current_.watching && generation != 0 && generation == current_.generation;
    }
    [[nodiscard]] bool rawOnly(const std::string& lensKey) const {
        auto it = lenses_.find(lensKey);
        return it != lenses_.end() && it->second.rawOnly;
    }

   private:
    struct Lens {
        bool proven = false;
        bool rawOnly = false;
        bool gaveUp = false;
        uint32_t failures = 0;  // At the current step.
    };
    struct Start {
        std::string lensKey;
        uint64_t generation = 0;
        int64_t deadlineMs = 0;
        uint32_t failedCaptures = 0;
        bool watching = false;
    };

    // One failure per start: the start stops watching once it has failed.
    Outcome fail() {
        current_.watching = false;
        auto& lens = lenses_[current_.lensKey];
        ++lens.failures;
        if (lens.rawOnly) {
            lens = {};
            lens.gaveUp = true;
            return Outcome::GaveUp;
        }
        if (lens.failures < 2) return Outcome::Retry;
        lens.rawOnly = true;
        lens.failures = 0;
        return Outcome::RetryRawOnly;
    }

    std::unordered_map<std::string, Lens> lenses_;
    Start current_;
};

}  // namespace rawrcam::camera
