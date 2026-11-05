#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <limits>
#include <optional>
#include <string>

namespace rawrcam::camera {

// A scene with no shadows cannot prove its black floor. Fail closed rather
// than letting a changed sensor pedestal drive an automatic full-size still.
inline bool blackFloorAgrees(const std::optional<std::array<float, 4>>& measured,
                            const std::array<float, 4>& black, float white) {
    if (!measured || !std::isfinite(white)) return false;
    for (size_t c = 0; c < 4; ++c) {
        if (!std::isfinite(black[c]) || !std::isfinite((*measured)[c]) || white <= black[c]) return false;
        const float tolerance = std::max(16.0f, (white - black[c]) * 0.06f);
        if (std::abs((*measured)[c] - black[c]) > tolerance) return false;
    }
    return true;
}

class StillExposureGuard {
public:
    struct Exposure {
        int64_t exposureTimeNs = 0;
        int32_t sensitivity = 0;
        bool reportedIso = false;
        uint64_t timestampNs = 0;
    };
    // levels: what the frame was judged on, for the refusal log ("floor=... black=... white=...").
    void observe(uint64_t generation, Exposure exposure, int64_t nowMs, bool agrees, std::string levels = {}) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation < generation_ || (generation == generation_ && exposure.timestampNs <= exposure_.timestampNs)) return;
        generation_ = generation;
        exposure_ = exposure;
        observedMs_ = nowMs;
        agrees_ = agrees;
        levels_ = std::move(levels);
    }
    // The last observation as log fields, so a refused still says why.
    std::string describe(uint64_t generation, int64_t nowMs) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!observedMs_) return "observed=none";
        return std::string("agrees=") + (agrees_ ? "1" : "0") + " sameSession=" + (generation == generation_ ? "1" : "0") +
               " ageMs=" + std::to_string(nowMs - observedMs_) + (levels_.empty() ? "" : " " + levels_);
    }
    std::optional<Exposure> checked(uint64_t generation, int64_t nowMs) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto maxAge = std::max<int64_t>(2000, exposure_.exposureTimeNs / 1000000 + 1000);
        if (!agrees_ || generation != generation_ || nowMs < observedMs_ || nowMs - observedMs_ > maxAge ||
            exposure_.exposureTimeNs <= 0 || exposure_.sensitivity <= 0 || !exposure_.timestampNs) return std::nullopt;
        return exposure_;
    }
    // Preserve a user-owned shutter or ISO using only the checked frame's
    // exposure product; a pending AE command may have come from a bad frame.
    static std::optional<Exposure> requestExposure(Exposure sample, double reportedPerRequest,
                                                    int64_t shutterNs = 0, int32_t iso = 0) {
        const double ratio = sample.reportedIso ? reportedPerRequest : 1.0;
        if (!(ratio > 0) || !std::isfinite(ratio) || sample.exposureTimeNs <= 0 || sample.sensitivity <= 0)
            return std::nullopt;
        double sensitivity = sample.sensitivity / ratio;
        double exposure = double(sample.exposureTimeNs);
        const double product = exposure * sensitivity;
        if (shutterNs > 0) { exposure = double(shutterNs); sensitivity = product / exposure; }
        else if (iso > 0) { sensitivity = iso; exposure = product / sensitivity; }
        if (!std::isfinite(exposure) || !std::isfinite(sensitivity) ||
            exposure >= double(std::numeric_limits<int64_t>::max()) || sensitivity > INT32_MAX) return std::nullopt;
        sample.exposureTimeNs = int64_t(std::max(1.0, std::round(exposure)));
        sample.sensitivity = int32_t(std::max(1.0, std::round(sensitivity)));
        sample.reportedIso = false;
        return sample;
    }
private:
    mutable std::mutex mutex_;
    uint64_t generation_ = 0;
    int64_t observedMs_ = 0;
    Exposure exposure_;
    bool agrees_ = false;
    std::string levels_;
};
}  // namespace rawrcam::camera
