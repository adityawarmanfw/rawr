#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <utility>

namespace rawrcam::diagnostics {

// Largest sample within a trailing time window, plus the all-time maximum.
// Averages hide the single slow frame that causes a drop; this keeps it visible.
class RecentPeak {
   public:
    explicit RecentPeak(int64_t windowNs = 2'000'000'000) : windowNs_(windowNs) {}
    void add(int64_t nowNs, double value) {
        samples_.emplace_back(nowNs, value);
        while (!samples_.empty() && samples_.front().first < nowNs - windowNs_) samples_.pop_front();
        max_ = std::max(max_, value);
    }
    double peak(int64_t nowNs) const {
        double peak = 0.0;
        for (const auto& [at, value] : samples_)
            if (at >= nowNs - windowNs_) peak = std::max(peak, value);
        return peak;
    }
    double max() const noexcept { return max_; }
    void reset() noexcept {
        samples_.clear();
        max_ = 0.0;
    }

   private:
    int64_t windowNs_;
    std::deque<std::pair<int64_t, double>> samples_;
    double max_ = 0.0;
};

}  // namespace rawrcam::diagnostics
