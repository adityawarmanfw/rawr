#pragma once
#include <android/log.h>
#include <vulkan/vulkan.h>

#include <stdexcept>
#include <string>
namespace rawrcam::vulkan {
inline const char* rawImportUsageName(VkImageUsageFlags u) {
    if (u == VK_IMAGE_USAGE_STORAGE_BIT) return "STORAGE";
    if (u == (VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) return "COMPUTE";
    if (u == VK_IMAGE_USAGE_SAMPLED_BIT) return "SAMPLED";
    if (u == VK_IMAGE_USAGE_TRANSFER_SRC_BIT) return "TRANSFER_SRC";
    return "MIXED";
}
// The zero-copy diagnostic usages; production imports never request these.
inline bool isZeroCopyEvidenceUsage(VkImageUsageFlags u) {
    return u == (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) ||
           u == (VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
}
inline void logZeroCopyEvidence(VkImageUsageFlags u, const std::string& line) {
    if (isZeroCopyEvidenceUsage(u)) __android_log_print(ANDROID_LOG_INFO, "RawrCamNative", "%s", line.c_str());
}
// First memory type in bits with the wanted flags, else the first in bits.
inline uint32_t findImportMemoryType(VkPhysicalDevice p, uint32_t bits, VkMemoryPropertyFlags wanted) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(p, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & wanted) == wanted) return i;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if (bits & (1u << i)) return i;
    throw std::runtime_error("No compatible Vulkan memory type");
}
}  // namespace rawrcam::vulkan
