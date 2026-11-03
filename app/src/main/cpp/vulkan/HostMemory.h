#pragma once
#include <vulkan/vulkan.h>

#include <stdexcept>
namespace rawrcam::vulkan {
struct ReadbackMemoryType { uint32_t index; bool coherent; };
inline ReadbackMemoryType selectReadbackMemoryType(const VkPhysicalDeviceMemoryProperties& properties,
                                                  uint32_t bits, bool preferCached = true) {
    if (preferCached) {
        for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
            const auto flags = properties.memoryTypes[i].propertyFlags;
            if ((bits & (1u << i)) && (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
                (flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
                return {i, (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0};
        }
    }
    constexpr auto required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & required) == required) return {i, true};
    throw std::runtime_error("no compatible CPU readback memory");
}
inline uint32_t findCoherentHostMemoryType(VkPhysicalDevice physical, uint32_t bits) {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    constexpr auto required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & required) == required) return i;
    }
    throw std::runtime_error("rendered still requires HOST_VISIBLE|HOST_COHERENT readback memory");
}
}  // namespace rawrcam::vulkan
