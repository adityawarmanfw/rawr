#pragma once
#include <array>
#include <cstdint>

namespace rawrcam::vulkan {
struct QueuePriorityPolicy {
    uint32_t queueCount;
    std::array<float, 2> priorities;
    bool stillQueueLowPriority() const noexcept {
        return queueCount > 1 && priorities[1] < priorities[0];
    }
};

inline QueuePriorityPolicy queuePriorityPolicy(uint32_t vendorId, uint32_t familyQueues,
                                               bool legacyMaliPriority = false) noexcept {
    // A continuously busy preview at 1.0 can starve still compute at 0.0 on
    // Mali. Equal local priorities allow still work to progress; existing
    // bounded submissions limit preview stalls. Preserve Adreno/other policy.
    const bool fairStill = vendorId == 0x13B5u && !legacyMaliPriority;
    return {familyQueues >= 2 ? 2u : 1u, {1.0f, fairStill ? 1.0f : 0.0f}};
}
}  // namespace rawrcam::vulkan
