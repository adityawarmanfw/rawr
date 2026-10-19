#pragma once
#include <atomic>
#include <cstdint>

namespace rawrcam::capture {
// Monotonic, process-wide counts of full-size captures the HAL returned binned
// (see imaging/BinnedCorner.h); the UI tells the user when one goes up.
struct FullSizeFallbacks {
    std::atomic<uint64_t> binnedStills{0};     // single stills saved at the binned size
    std::atomic<uint64_t> burstsToSingle{0};   // merged bursts that fell back to a single still
};
inline FullSizeFallbacks& fullSizeFallbacks() {
    static FullSizeFallbacks counts;
    return counts;
}
}  // namespace rawrcam::capture
