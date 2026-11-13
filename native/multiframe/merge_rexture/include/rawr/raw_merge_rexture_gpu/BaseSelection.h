#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// Rexture base-frame choice at the shutter press. Each candidate frame gets a cheap score (rx_frame_score: sharpness above the
// noise expectation, clipped share); the base is the best frame of the last kWindowSeconds, penalised for clipping and age.
namespace rawr::raw_merge_rexture_gpu {

inline constexpr double kBaseWindowSeconds = 0.25;
inline constexpr double kBaseSaturationPenalty = 2.0;  // value x (1 - penalty x clipped share)
inline constexpr double kBaseTimePenalty = 0.5;        // value x (1 - penalty x age / window)

struct FrameScore {
    double sharp = 0.0;  // >= 0
    double sat = 0.0;    // clipped share, 0..1
};

// times: capture times (s) of the frames; press: press time (s). Candidates: frames in [press - window, press]; none: the
// latest frame before the press; none at all: the nearest. Ties go to the newest frame. Returns an index into `scores`.
[[nodiscard]] inline std::size_t chooseBaseAtPress(const std::vector<FrameScore>& scores, const std::vector<double>& times,
                                                   double press) {
    const std::size_t n = std::min(scores.size(), times.size());
    std::vector<std::size_t> idx;
    for (std::size_t i = 0; i < n; ++i)
        if (times[i] >= press - kBaseWindowSeconds && times[i] <= press) idx.push_back(i);
    if (idx.empty()) {
        bool found = false;
        std::size_t best = 0;
        for (std::size_t i = 0; i < n; ++i)
            if (times[i] <= press && (!found || times[i] > times[best])) best = i, found = true;
        if (!found)
            for (std::size_t i = 1; i < n; ++i)
                if (std::abs(times[i] - press) < std::abs(times[best] - press)) best = i;
        return best;
    }
    auto value = [&](std::size_t i) {
        const double age = std::max(press - times[i], 0.0);
        return scores[i].sharp * std::max(1.0 - kBaseSaturationPenalty * scores[i].sat, 0.0) *
               std::max(1.0 - kBaseTimePenalty * age / kBaseWindowSeconds, 0.0);
    };
    std::size_t best = idx.front();
    for (const std::size_t i : idx)
        if (value(i) > value(best) || (value(i) == value(best) && times[i] >= times[best])) best = i;
    return best;
}

}  // namespace rawr::raw_merge_rexture_gpu
