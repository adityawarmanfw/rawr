#pragma once
#include <android/hardware_buffer.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>

#include <cstdint>
#include <functional>
#include <string>
namespace rawrcam::vulkan {
struct ImportedRaw;
class DeviceObjects;
// What the importer learned about one camera AHB, for diagnostic probes.
struct RawImportFacts {
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    AHardwareBuffer* ahb = nullptr;
    uint32_t width = 0, height = 0;
    VkImageUsageFlags usage = 0;
    const AHardwareBuffer_Desc* desc = nullptr;
    const VkAndroidHardwareBufferPropertiesANDROID* properties = nullptr;
    const VkAndroidHardwareBufferFormatPropertiesANDROID* format = nullptr;
    const std::function<void(const std::string&)>* diagnostic = nullptr;
    bool advancedCandidates = false;
};
// Diagnostic hooks around the primary import. beforeImport runs before the
// import is validated, so its evidence survives a rejection. afterImport may
// add images bound to the same AHB; it adopts them into objects, which live
// as long as the cache entry. Throwing from it rejects the import.
struct RawImportOptions {
    void (*beforeImport)(const RawImportFacts&) = nullptr;
    void (*afterImport)(const RawImportFacts&, ImportedRaw&, DeviceObjects&) = nullptr;
    bool advancedCandidates = false;
};
}  // namespace rawrcam::vulkan
