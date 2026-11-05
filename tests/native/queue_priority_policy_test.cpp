#include "vulkan/QueuePriorityPolicy.h"
#include <cassert>

int main() {
    using rawrcam::vulkan::queuePriorityPolicy;
    // Regression: Qualcomm's tested local priority/global priority behavior
    // must survive the Mali scheduling change, including the debug override.
    for (bool legacy : {false, true}) {
        const auto adreno = queuePriorityPolicy(0x5143, 2, legacy);
        assert(adreno.queueCount == 2 && adreno.priorities[0] == 1.0f && adreno.priorities[1] == 0.0f);
        assert(adreno.stillQueueLowPriority());
    }
    const auto mali = queuePriorityPolicy(0x13B5, 2);
    assert(mali.queueCount == 2 && mali.priorities[0] == mali.priorities[1]);
    assert(!mali.stillQueueLowPriority());
    assert(queuePriorityPolicy(0x13B5, 2, true).stillQueueLowPriority());
    // A driver exposing only one suitable queue keeps a single FIFO and must
    // never claim a nonexistent low-priority second queue.
    for (uint32_t vendor : {0x13B5u, 0x5143u, 0u}) {
        const auto single = queuePriorityPolicy(vendor, 1);
        assert(single.queueCount == 1 && single.priorities[0] == 1.0f && !single.stillQueueLowPriority());
    }
    const auto other = queuePriorityPolicy(0, 4);
    assert(other.queueCount == 2 && other.stillQueueLowPriority());
}
