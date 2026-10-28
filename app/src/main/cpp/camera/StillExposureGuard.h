#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>

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
    void observe(uint64_t generation, Exposure exposure, int64_t nowMs, bool agrees) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation < generation_ || (generation == generation_ && exposure.timestampNs <= exposure_.timestampNs)) return;
        generation_ = generation;
        exposure_ = exposure;
        observedMs_ = nowMs;
        agrees_ = agrees;
    }
    std::optional<Exposure> checked(uint64_t generation, int64_t nowMs) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto maxAge = std::max<int64_t>(2000, exposure_.exposureTimeNs / 1000000 + 1000);
        if (!agrees_ || generation != generation_ || nowMs < observedMs_ || nowMs - observedMs_ > maxAge ||
            exposure_.exposureTimeNs <= 0 || exposure_.sensitivity <= 0 || !exposure_.timestampNs) return std::nullopt;
        return exposure_;
    }
private:
    mutable std::mutex mutex_;
    uint64_t generation_ = 0;
    int64_t observedMs_ = 0;
    Exposure exposure_;
    bool agrees_ = false;
};
}  // namespace rawrcam::camera
