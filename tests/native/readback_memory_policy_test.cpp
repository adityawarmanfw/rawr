#include "vulkan/HostMemory.h"
#include <cassert>
int main() {
    VkPhysicalDeviceMemoryProperties p{};
    p.memoryTypeCount = 3;
    p.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    p.memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    p.memoryTypes[2].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    using rawrcam::vulkan::selectReadbackMemoryType;
    const auto cached = selectReadbackMemoryType(p, 7);
    assert(cached.index == 1 && !cached.coherent);
    assert(selectReadbackMemoryType(p, 7, false).index == 0);
    const auto fallback = selectReadbackMemoryType(p, 1);
    assert(fallback.index == 0 && fallback.coherent);
    p.memoryTypes[1].propertyFlags |= VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    assert(selectReadbackMemoryType(p, 7).coherent);
    bool rejected = false;
    try { (void)selectReadbackMemoryType(p, 4); }
    catch (const std::runtime_error&) { rejected = true; }
    assert(rejected);
}
