#pragma once
#include <algorithm>
#include <cstdint>

namespace rawrcam::capture {
inline constexpr uint32_t kMaxBurstFrames = 30;
// A profile can lower the application limit; zero means the application default.
inline constexpr uint32_t fullSizeBurstCount(uint32_t requested, uint32_t profileLimit) {
    return std::min(requested, profileLimit ? std::min(profileLimit, kMaxBurstFrames) : kMaxBurstFrames);
}
}  // namespace rawrcam::capture
