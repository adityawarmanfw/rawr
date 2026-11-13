#include "VulkanDispatch.h"

#include <adrenotools/driver.h>
#include <android/log.h>
#include <dlfcn.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

PFN_vkAcquireNextImageKHR vkAcquireNextImageKHR = nullptr;
PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers = nullptr;
PFN_vkAllocateDescriptorSets vkAllocateDescriptorSets = nullptr;
PFN_vkAllocateMemory vkAllocateMemory = nullptr;
PFN_vkBeginCommandBuffer vkBeginCommandBuffer = nullptr;
PFN_vkBindBufferMemory vkBindBufferMemory = nullptr;
PFN_vkBindImageMemory vkBindImageMemory = nullptr;
PFN_vkCmdBeginRenderPass vkCmdBeginRenderPass = nullptr;
PFN_vkCmdBindDescriptorSets vkCmdBindDescriptorSets = nullptr;
PFN_vkCmdBindPipeline vkCmdBindPipeline = nullptr;
PFN_vkCmdBlitImage vkCmdBlitImage = nullptr;
PFN_vkCmdClearColorImage vkCmdClearColorImage = nullptr;
PFN_vkCmdCopyBuffer vkCmdCopyBuffer = nullptr;
PFN_vkCmdCopyBufferToImage vkCmdCopyBufferToImage = nullptr;
PFN_vkCmdCopyImage vkCmdCopyImage = nullptr;
PFN_vkCmdCopyImageToBuffer vkCmdCopyImageToBuffer = nullptr;
PFN_vkCmdDispatch vkCmdDispatch = nullptr;
PFN_vkCmdDispatchIndirect vkCmdDispatchIndirect = nullptr;
PFN_vkCmdDispatchBase vkCmdDispatchBase = nullptr;
PFN_vkCmdDraw vkCmdDraw = nullptr;
PFN_vkCmdEndRenderPass vkCmdEndRenderPass = nullptr;
PFN_vkCmdFillBuffer vkCmdFillBuffer = nullptr;
PFN_vkCmdUpdateBuffer vkCmdUpdateBuffer = nullptr;
PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier = nullptr;
PFN_vkCmdPushConstants vkCmdPushConstants = nullptr;
PFN_vkCmdResetQueryPool vkCmdResetQueryPool = nullptr;
PFN_vkCmdSetScissor vkCmdSetScissor = nullptr;
PFN_vkCmdSetViewport vkCmdSetViewport = nullptr;
PFN_vkCmdWriteTimestamp vkCmdWriteTimestamp = nullptr;
PFN_vkCreateAndroidSurfaceKHR vkCreateAndroidSurfaceKHR = nullptr;
PFN_vkCreateBuffer vkCreateBuffer = nullptr;
PFN_vkCreateCommandPool vkCreateCommandPool = nullptr;
PFN_vkCreateComputePipelines vkCreateComputePipelines = nullptr;
PFN_vkCreateDescriptorPool vkCreateDescriptorPool = nullptr;
PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout = nullptr;
PFN_vkCreateDevice vkCreateDevice = nullptr;
PFN_vkCreateFence vkCreateFence = nullptr;
PFN_vkCreateFramebuffer vkCreateFramebuffer = nullptr;
PFN_vkCreateGraphicsPipelines vkCreateGraphicsPipelines = nullptr;
PFN_vkCreateImage vkCreateImage = nullptr;
PFN_vkCreateImageView vkCreateImageView = nullptr;
PFN_vkCreateInstance vkCreateInstance = nullptr;
PFN_vkCreatePipelineLayout vkCreatePipelineLayout = nullptr;
PFN_vkCreatePipelineCache vkCreatePipelineCache = nullptr;
PFN_vkDestroyPipelineCache vkDestroyPipelineCache = nullptr;
PFN_vkGetPipelineCacheData vkGetPipelineCacheData = nullptr;
PFN_vkCreateQueryPool vkCreateQueryPool = nullptr;
PFN_vkCreateRenderPass vkCreateRenderPass = nullptr;
PFN_vkCreateSampler vkCreateSampler = nullptr;
PFN_vkCreateSamplerYcbcrConversion vkCreateSamplerYcbcrConversion = nullptr;
PFN_vkCreateSemaphore vkCreateSemaphore = nullptr;
PFN_vkCreateShaderModule vkCreateShaderModule = nullptr;
PFN_vkCreateSwapchainKHR vkCreateSwapchainKHR = nullptr;
PFN_vkDestroyBuffer vkDestroyBuffer = nullptr;
PFN_vkDestroyCommandPool vkDestroyCommandPool = nullptr;
PFN_vkDestroyDescriptorPool vkDestroyDescriptorPool = nullptr;
PFN_vkDestroyDescriptorSetLayout vkDestroyDescriptorSetLayout = nullptr;
PFN_vkDestroyDevice vkDestroyDevice = nullptr;
PFN_vkDestroyFence vkDestroyFence = nullptr;
PFN_vkDestroyFramebuffer vkDestroyFramebuffer = nullptr;
PFN_vkDestroyImage vkDestroyImage = nullptr;
PFN_vkDestroyImageView vkDestroyImageView = nullptr;
PFN_vkDestroyInstance vkDestroyInstance = nullptr;
PFN_vkDestroyPipeline vkDestroyPipeline = nullptr;
PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout = nullptr;
PFN_vkDestroyQueryPool vkDestroyQueryPool = nullptr;
PFN_vkDestroyRenderPass vkDestroyRenderPass = nullptr;
PFN_vkDestroySampler vkDestroySampler = nullptr;
PFN_vkDestroySamplerYcbcrConversion vkDestroySamplerYcbcrConversion = nullptr;
PFN_vkDestroySemaphore vkDestroySemaphore = nullptr;
PFN_vkDestroyShaderModule vkDestroyShaderModule = nullptr;
PFN_vkDestroySurfaceKHR vkDestroySurfaceKHR = nullptr;
PFN_vkDestroySwapchainKHR vkDestroySwapchainKHR = nullptr;
PFN_vkDeviceWaitIdle vkDeviceWaitIdle = nullptr;
PFN_vkEndCommandBuffer vkEndCommandBuffer = nullptr;
PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties = nullptr;
PFN_vkEnumerateInstanceExtensionProperties vkEnumerateInstanceExtensionProperties = nullptr;
PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices = nullptr;
PFN_vkGetPhysicalDeviceExternalBufferProperties vkGetPhysicalDeviceExternalBufferProperties = nullptr;
PFN_vkFlushMappedMemoryRanges vkFlushMappedMemoryRanges = nullptr;
PFN_vkFreeCommandBuffers vkFreeCommandBuffers = nullptr;
PFN_vkFreeDescriptorSets vkFreeDescriptorSets = nullptr;
PFN_vkFreeMemory vkFreeMemory = nullptr;
PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements = nullptr;
PFN_vkGetBufferMemoryRequirements2 vkGetBufferMemoryRequirements2 = nullptr;
PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr = nullptr;
PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
PFN_vkGetDeviceQueue vkGetDeviceQueue = nullptr;
PFN_vkGetFenceStatus vkGetFenceStatus = nullptr;
PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements = nullptr;
PFN_vkGetImageMemoryRequirements2 vkGetImageMemoryRequirements2 = nullptr;
PFN_vkGetImageSubresourceLayout vkGetImageSubresourceLayout = nullptr;
PFN_vkGetPhysicalDeviceFeatures vkGetPhysicalDeviceFeatures = nullptr;
PFN_vkGetPhysicalDeviceFeatures2 vkGetPhysicalDeviceFeatures2 = nullptr;
PFN_vkGetPhysicalDeviceFormatProperties vkGetPhysicalDeviceFormatProperties = nullptr;
PFN_vkGetPhysicalDeviceFormatProperties2 vkGetPhysicalDeviceFormatProperties2 = nullptr;
PFN_vkGetPhysicalDeviceImageFormatProperties vkGetPhysicalDeviceImageFormatProperties = nullptr;
PFN_vkGetPhysicalDeviceImageFormatProperties2 vkGetPhysicalDeviceImageFormatProperties2 = nullptr;
PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties = nullptr;
PFN_vkGetPhysicalDeviceMemoryProperties2 vkGetPhysicalDeviceMemoryProperties2 = nullptr;
PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties = nullptr;
PFN_vkGetPhysicalDeviceProperties2 vkGetPhysicalDeviceProperties2 = nullptr;
PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties = nullptr;
PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR vkGetPhysicalDeviceSurfaceCapabilitiesKHR = nullptr;
PFN_vkGetPhysicalDeviceSurfaceFormatsKHR vkGetPhysicalDeviceSurfaceFormatsKHR = nullptr;
PFN_vkGetPhysicalDeviceSurfaceSupportKHR vkGetPhysicalDeviceSurfaceSupportKHR = nullptr;
PFN_vkGetQueryPoolResults vkGetQueryPoolResults = nullptr;
PFN_vkGetSwapchainImagesKHR vkGetSwapchainImagesKHR = nullptr;
PFN_vkInvalidateMappedMemoryRanges vkInvalidateMappedMemoryRanges = nullptr;
PFN_vkMapMemory vkMapMemory = nullptr;
PFN_vkQueuePresentKHR vkQueuePresentKHR = nullptr;
PFN_vkQueueSubmit vkQueueSubmit = nullptr;
PFN_vkQueueWaitIdle vkQueueWaitIdle = nullptr;
PFN_vkResetCommandBuffer vkResetCommandBuffer = nullptr;
PFN_vkResetDescriptorPool vkResetDescriptorPool = nullptr;
PFN_vkResetFences vkResetFences = nullptr;
PFN_vkUnmapMemory vkUnmapMemory = nullptr;
PFN_vkUpdateDescriptorSets vkUpdateDescriptorSets = nullptr;
PFN_vkWaitForFences vkWaitForFences = nullptr;

namespace rawrcam::vulkan::dispatch {
namespace {
std::mutex gMu;
void* gLib = nullptr;
PFN_vkGetInstanceProcAddr gGipa = nullptr;
PFN_vkGetDeviceProcAddr gGdpa = nullptr;
std::string gDesc = "unconfigured";
void clearFns() {
    vkAcquireNextImageKHR = nullptr;
    vkAllocateCommandBuffers = nullptr;
    vkAllocateDescriptorSets = nullptr;
    vkAllocateMemory = nullptr;
    vkBeginCommandBuffer = nullptr;
    vkBindBufferMemory = nullptr;
    vkBindImageMemory = nullptr;
    vkCmdBeginRenderPass = nullptr;
    vkCmdBindDescriptorSets = nullptr;
    vkCmdBindPipeline = nullptr;
    vkCmdClearColorImage = nullptr;
    vkCmdCopyBuffer = nullptr;
    vkCmdCopyBufferToImage = nullptr;
    vkCmdCopyImage = nullptr;
    vkCmdCopyImageToBuffer = nullptr;
    vkCmdDispatch = nullptr;
    vkCmdDispatchIndirect = nullptr;
    vkCmdDispatchBase = nullptr;
    vkCmdDraw = nullptr;
    vkCmdEndRenderPass = nullptr;
    vkCmdFillBuffer = nullptr;
    vkCmdUpdateBuffer = nullptr;
    vkCmdPipelineBarrier = nullptr;
    vkCmdPushConstants = nullptr;
    vkCmdResetQueryPool = nullptr;
    vkCmdSetScissor = nullptr;
    vkCmdSetViewport = nullptr;
    vkCmdWriteTimestamp = nullptr;
    vkCreateAndroidSurfaceKHR = nullptr;
    vkCreateBuffer = nullptr;
    vkCreateCommandPool = nullptr;
    vkCreateComputePipelines = nullptr;
    vkCreateDescriptorPool = nullptr;
    vkCreateDescriptorSetLayout = nullptr;
    vkCreateDevice = nullptr;
    vkCreateFence = nullptr;
    vkCreateFramebuffer = nullptr;
    vkCreateGraphicsPipelines = nullptr;
    vkCreateImage = nullptr;
    vkCreateImageView = nullptr;
    vkCreateInstance = nullptr;
    vkCreatePipelineLayout = nullptr;
    vkCreatePipelineCache = nullptr;
    vkDestroyPipelineCache = nullptr;
    vkGetPipelineCacheData = nullptr;
    vkCreateQueryPool = nullptr;
    vkCreateRenderPass = nullptr;
    vkCreateSampler = nullptr;
    vkCreateSamplerYcbcrConversion = nullptr;
    vkCreateSemaphore = nullptr;
    vkCreateShaderModule = nullptr;
    vkCreateSwapchainKHR = nullptr;
    vkDestroyBuffer = nullptr;
    vkDestroyCommandPool = nullptr;
    vkDestroyDescriptorPool = nullptr;
    vkDestroyDescriptorSetLayout = nullptr;
    vkDestroyDevice = nullptr;
    vkDestroyFence = nullptr;
    vkDestroyFramebuffer = nullptr;
    vkDestroyImage = nullptr;
    vkDestroyImageView = nullptr;
    vkDestroyInstance = nullptr;
    vkDestroyPipeline = nullptr;
    vkDestroyPipelineLayout = nullptr;
    vkDestroyQueryPool = nullptr;
    vkDestroyRenderPass = nullptr;
    vkDestroySampler = nullptr;
    vkDestroySamplerYcbcrConversion = nullptr;
    vkDestroySemaphore = nullptr;
    vkDestroyShaderModule = nullptr;
    vkDestroySurfaceKHR = nullptr;
    vkDestroySwapchainKHR = nullptr;
    vkDeviceWaitIdle = nullptr;
    vkEndCommandBuffer = nullptr;
    vkEnumerateDeviceExtensionProperties = nullptr;
    vkEnumerateInstanceExtensionProperties = nullptr;
    vkEnumeratePhysicalDevices = nullptr;
    vkGetPhysicalDeviceExternalBufferProperties = nullptr;
    vkFlushMappedMemoryRanges = nullptr;
    vkFreeCommandBuffers = nullptr;
    vkFreeDescriptorSets = nullptr;
    vkFreeMemory = nullptr;
    vkGetBufferMemoryRequirements = nullptr;
    vkGetBufferMemoryRequirements2 = nullptr;
    vkGetDeviceProcAddr = nullptr;
    vkGetInstanceProcAddr = nullptr;
    vkGetDeviceQueue = nullptr;
    vkGetFenceStatus = nullptr;
    vkGetImageMemoryRequirements = nullptr;
    vkGetImageMemoryRequirements2 = nullptr;
    vkGetImageSubresourceLayout = nullptr;
    vkGetPhysicalDeviceFeatures = nullptr;
    vkGetPhysicalDeviceFeatures2 = nullptr;
    vkGetPhysicalDeviceFormatProperties = nullptr;
    vkGetPhysicalDeviceFormatProperties2 = nullptr;
    vkGetPhysicalDeviceImageFormatProperties = nullptr;
    vkGetPhysicalDeviceImageFormatProperties2 = nullptr;
    vkGetPhysicalDeviceMemoryProperties = nullptr;
    vkGetPhysicalDeviceMemoryProperties2 = nullptr;
    vkGetPhysicalDeviceProperties = nullptr;
    vkGetPhysicalDeviceProperties2 = nullptr;
    vkGetPhysicalDeviceQueueFamilyProperties = nullptr;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR = nullptr;
    vkGetPhysicalDeviceSurfaceFormatsKHR = nullptr;
    vkGetPhysicalDeviceSurfaceSupportKHR = nullptr;
    vkGetQueryPoolResults = nullptr;
    vkGetSwapchainImagesKHR = nullptr;
    vkInvalidateMappedMemoryRanges = nullptr;
    vkMapMemory = nullptr;
    vkQueuePresentKHR = nullptr;
    vkQueueSubmit = nullptr;
    vkQueueWaitIdle = nullptr;
    vkResetCommandBuffer = nullptr;
    vkResetDescriptorPool = nullptr;
    vkResetFences = nullptr;
    vkUnmapMemory = nullptr;
    vkUpdateDescriptorSets = nullptr;
    vkWaitForFences = nullptr;
}
template <class T>
void setIf(T& slot, PFN_vkVoidFunction p) {
    if (p) slot = reinterpret_cast<T>(p);
}
PFN_vkCreateComputePipelines gDriverCreateComputePipelines = nullptr;
std::mutex gCacheMutex;
VkDevice gCacheDevice = VK_NULL_HANDLE;
VkPipelineCache gCache = VK_NULL_HANDLE;
std::string gCacheFile;
size_t gCacheSavedBytes = 0;
VKAPI_ATTR VkResult VKAPI_CALL cachedCreateComputePipelines(VkDevice device, VkPipelineCache cache,
                                                            uint32_t count,
                                                            const VkComputePipelineCreateInfo* infos,
                                                            const VkAllocationCallbacks* allocator,
                                                            VkPipeline* pipelines) {
    if (cache == VK_NULL_HANDLE) cache = pipelineCache(device);
    return gDriverCreateComputePipelines(device, cache, count, infos, allocator, pipelines);
}
}  // namespace
VkPipelineCache pipelineCache(VkDevice device) {
    std::lock_guard<std::mutex> lock(gCacheMutex);
    if (device == VK_NULL_HANDLE || !vkCreatePipelineCache) return VK_NULL_HANDLE;
    if (gCacheDevice != device) {
        if (gCacheDevice != VK_NULL_HANDLE) return VK_NULL_HANDLE;  // one device at a time
        std::vector<char> blob;
        if (!gCacheFile.empty()) {
            std::ifstream in(gCacheFile, std::ios::binary);
            blob.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        VkPipelineCacheCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
        info.initialDataSize = blob.size();
        info.pInitialData = blob.empty() ? nullptr : blob.data();
        VkResult result = vkCreatePipelineCache(device, &info, nullptr, &gCache);
        if (result != VK_SUCCESS && !blob.empty()) {
            // A driver may reject a corrupt blob outright: start empty.
            info.initialDataSize = 0;
            info.pInitialData = nullptr;
            blob.clear();
            result = vkCreatePipelineCache(device, &info, nullptr, &gCache);
        }
        if (result != VK_SUCCESS) return VK_NULL_HANDLE;
        gCacheDevice = device;
        gCacheSavedBytes = blob.size();
        __android_log_print(ANDROID_LOG_INFO, "RawrCamNative", "VK_PIPELINE_CACHE_LOADED bytes=%zu file=%s",
                            blob.size(), gCacheFile.empty() ? "none" : "yes");
    }
    return gCache;
}
void setPipelineCacheFile(const std::string& path) {
    std::lock_guard<std::mutex> lock(gCacheMutex);
    gCacheFile = path;
}
namespace {
void saveLocked() noexcept {
    if (gCache == VK_NULL_HANDLE || gCacheFile.empty() || !vkGetPipelineCacheData) return;
    size_t size = 0;
    if (vkGetPipelineCacheData(gCacheDevice, gCache, &size, nullptr) != VK_SUCCESS || size == 0) return;
    if (size == gCacheSavedBytes) return;  // the cache only grows: unchanged
    std::vector<char> blob(size);
    if (vkGetPipelineCacheData(gCacheDevice, gCache, &size, blob.data()) != VK_SUCCESS) return;
    const std::string temp = gCacheFile + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(blob.data(), static_cast<std::streamsize>(size));
        if (!out) return;
    }
    if (std::rename(temp.c_str(), gCacheFile.c_str()) != 0) return;
    gCacheSavedBytes = size;
    __android_log_print(ANDROID_LOG_INFO, "RawrCamNative", "VK_PIPELINE_CACHE_SAVED bytes=%zu", size);
}
}  // namespace
void savePipelineCache(VkDevice device) noexcept {
    std::lock_guard<std::mutex> lock(gCacheMutex);
    if (device != VK_NULL_HANDLE && device == gCacheDevice) saveLocked();
}
void releasePipelineCache(VkDevice device) {
    std::lock_guard<std::mutex> lock(gCacheMutex);
    if (device != VK_NULL_HANDLE && device == gCacheDevice) saveLocked();
    if (device != VK_NULL_HANDLE && device == gCacheDevice && gCache != VK_NULL_HANDLE && vkDestroyPipelineCache)
        vkDestroyPipelineCache(device, gCache, nullptr);
    if (device == gCacheDevice) {
        gCache = VK_NULL_HANDLE;
        gCacheDevice = VK_NULL_HANDLE;
        gCacheSavedBytes = 0;
    }
}
void configure(const std::string& nativeLibraryDir, const std::string& path) {
    std::lock_guard<std::mutex> lock(gMu);
    if (gLib) return;
    clearFns();
    const bool requested = !path.empty();
    bool customDriver = false;
    // RawPreviewCoordinator only supplies a custom path when GpuPlatform supports
    // it, using the same check as the settings UI. Do not gate on
    // ro.hardware.vulkan here: Android can resolve the ICD via ro.board.platform.
    if (requested) {
        const auto slash = path.find_last_of('/');
        if (slash == std::string::npos || slash + 1 >= path.size() || nativeLibraryDir.empty()) {
            gDesc = "system (custom bootstrap invalid path)";
        } else {
            const std::string driverDir = path.substr(0, slash + 1);
            const std::string driverName = path.substr(slash + 1);
            gLib = adrenotools_open_libvulkan(RTLD_NOW | RTLD_LOCAL, ADRENOTOOLS_DRIVER_CUSTOM, nullptr,
                                              nativeLibraryDir.c_str(), driverDir.c_str(), driverName.c_str(), nullptr);
            if (gLib) {
                customDriver = true;
                gDesc = std::string("adrenotools custom requested ") + path;
            } else {
                gDesc = "system (adrenotools custom loader failed)";
                __android_log_print(ANDROID_LOG_ERROR, "RawrCamNative",
                                    "GPU_DRIVER_LOAD_FAIL loader=adrenotools path=%s hookDir=%s", path.c_str(),
                                    nativeLibraryDir.c_str());
            }
        }
    }
    if (!gLib) {
        gLib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        if (!requested) gDesc = "system";
    }
    if (!gLib)
        throw std::runtime_error(std::string("Unable to load Vulkan library: ") + (dlerror() ? dlerror() : "unknown"));
    gGipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(gLib, "vkGetInstanceProcAddr"));
    if (!gGipa) throw std::runtime_error("Selected Vulkan library has no vkGetInstanceProcAddr");
    vkGetInstanceProcAddr = gGipa;
    // Before an instance exists only the global commands may be queried: Android's loader returns null for any other name
    // and logs "invalid vkGetInstanceProcAddr(VK_NULL_HANDLE, ...)" for each (about 120 lines per launch). Every other
    // pointer is set by loadInstance / loadDevice. A custom driver loaded directly may answer such queries, so it keeps
    // the full early table as before.
    const auto early = [&](const char* name) -> PFN_vkVoidFunction {
        static constexpr const char* kGlobal[] = {"vkCreateInstance", "vkEnumerateInstanceExtensionProperties",
                                                   "vkEnumerateInstanceLayerProperties", "vkEnumerateInstanceVersion"};
        bool global = false;
        for (const char* g : kGlobal) global = global || std::strcmp(g, name) == 0;
        return (global || customDriver) ? gGipa(VK_NULL_HANDLE, name) : nullptr;
    };
    setIf(vkAcquireNextImageKHR, early("vkAcquireNextImageKHR"));
    setIf(vkAllocateCommandBuffers, early("vkAllocateCommandBuffers"));
    setIf(vkAllocateDescriptorSets, early("vkAllocateDescriptorSets"));
    setIf(vkAllocateMemory, early("vkAllocateMemory"));
    setIf(vkBeginCommandBuffer, early("vkBeginCommandBuffer"));
    setIf(vkBindBufferMemory, early("vkBindBufferMemory"));
    setIf(vkBindImageMemory, early("vkBindImageMemory"));
    setIf(vkCmdBeginRenderPass, early("vkCmdBeginRenderPass"));
    setIf(vkCmdBindDescriptorSets, early("vkCmdBindDescriptorSets"));
    setIf(vkCmdBindPipeline, early("vkCmdBindPipeline"));
    setIf(vkCmdBlitImage, early("vkCmdBlitImage"));
    setIf(vkCmdClearColorImage, early("vkCmdClearColorImage"));
    setIf(vkCmdCopyBuffer, early("vkCmdCopyBuffer"));
    setIf(vkCmdCopyBufferToImage, early("vkCmdCopyBufferToImage"));
    setIf(vkCmdCopyImage, early("vkCmdCopyImage"));
    setIf(vkCmdCopyImageToBuffer, early("vkCmdCopyImageToBuffer"));
    setIf(vkCmdDispatch, early("vkCmdDispatch"));
    setIf(vkCmdDispatchIndirect, early("vkCmdDispatchIndirect"));
    setIf(vkCmdDispatchBase, early("vkCmdDispatchBase"));
    setIf(vkCmdDraw, early("vkCmdDraw"));
    setIf(vkCmdEndRenderPass, early("vkCmdEndRenderPass"));
    setIf(vkCmdFillBuffer, early("vkCmdFillBuffer"));
    setIf(vkCmdUpdateBuffer, early("vkCmdUpdateBuffer"));
    setIf(vkCmdPipelineBarrier, early("vkCmdPipelineBarrier"));
    setIf(vkCmdPushConstants, early("vkCmdPushConstants"));
    setIf(vkCmdResetQueryPool, early("vkCmdResetQueryPool"));
    setIf(vkCmdSetScissor, early("vkCmdSetScissor"));
    setIf(vkCmdSetViewport, early("vkCmdSetViewport"));
    setIf(vkCmdWriteTimestamp, early("vkCmdWriteTimestamp"));
    setIf(vkCreateAndroidSurfaceKHR, early("vkCreateAndroidSurfaceKHR"));
    setIf(vkCreateBuffer, early("vkCreateBuffer"));
    setIf(vkCreateCommandPool, early("vkCreateCommandPool"));
    setIf(vkCreateComputePipelines, early("vkCreateComputePipelines"));
    setIf(vkCreateDescriptorPool, early("vkCreateDescriptorPool"));
    setIf(vkCreateDescriptorSetLayout, early("vkCreateDescriptorSetLayout"));
    setIf(vkCreateDevice, early("vkCreateDevice"));
    setIf(vkCreateFence, early("vkCreateFence"));
    setIf(vkCreateFramebuffer, early("vkCreateFramebuffer"));
    setIf(vkCreateGraphicsPipelines, early("vkCreateGraphicsPipelines"));
    setIf(vkCreateImage, early("vkCreateImage"));
    setIf(vkCreateImageView, early("vkCreateImageView"));
    setIf(vkCreateInstance, early("vkCreateInstance"));
    setIf(vkCreatePipelineLayout, early("vkCreatePipelineLayout"));
    setIf(vkCreatePipelineCache, early("vkCreatePipelineCache"));
    setIf(vkDestroyPipelineCache, early("vkDestroyPipelineCache"));
    setIf(vkGetPipelineCacheData, early("vkGetPipelineCacheData"));
    setIf(vkCreateQueryPool, early("vkCreateQueryPool"));
    setIf(vkCreateRenderPass, early("vkCreateRenderPass"));
    setIf(vkCreateSampler, early("vkCreateSampler"));
    setIf(vkCreateSamplerYcbcrConversion, early("vkCreateSamplerYcbcrConversion"));
    setIf(vkCreateSemaphore, early("vkCreateSemaphore"));
    setIf(vkCreateShaderModule, early("vkCreateShaderModule"));
    setIf(vkCreateSwapchainKHR, early("vkCreateSwapchainKHR"));
    setIf(vkDestroyBuffer, early("vkDestroyBuffer"));
    setIf(vkDestroyCommandPool, early("vkDestroyCommandPool"));
    setIf(vkDestroyDescriptorPool, early("vkDestroyDescriptorPool"));
    setIf(vkDestroyDescriptorSetLayout, early("vkDestroyDescriptorSetLayout"));
    setIf(vkDestroyDevice, early("vkDestroyDevice"));
    setIf(vkDestroyFence, early("vkDestroyFence"));
    setIf(vkDestroyFramebuffer, early("vkDestroyFramebuffer"));
    setIf(vkDestroyImage, early("vkDestroyImage"));
    setIf(vkDestroyImageView, early("vkDestroyImageView"));
    setIf(vkDestroyInstance, early("vkDestroyInstance"));
    setIf(vkDestroyPipeline, early("vkDestroyPipeline"));
    setIf(vkDestroyPipelineLayout, early("vkDestroyPipelineLayout"));
    setIf(vkDestroyQueryPool, early("vkDestroyQueryPool"));
    setIf(vkDestroyRenderPass, early("vkDestroyRenderPass"));
    setIf(vkDestroySampler, early("vkDestroySampler"));
    setIf(vkDestroySamplerYcbcrConversion, early("vkDestroySamplerYcbcrConversion"));
    setIf(vkDestroySemaphore, early("vkDestroySemaphore"));
    setIf(vkDestroyShaderModule, early("vkDestroyShaderModule"));
    setIf(vkDestroySurfaceKHR, early("vkDestroySurfaceKHR"));
    setIf(vkDestroySwapchainKHR, early("vkDestroySwapchainKHR"));
    setIf(vkDeviceWaitIdle, early("vkDeviceWaitIdle"));
    setIf(vkEndCommandBuffer, early("vkEndCommandBuffer"));
    setIf(vkEnumerateDeviceExtensionProperties, early("vkEnumerateDeviceExtensionProperties"));
    setIf(vkEnumerateInstanceExtensionProperties, early("vkEnumerateInstanceExtensionProperties"));
    setIf(vkEnumeratePhysicalDevices, early("vkEnumeratePhysicalDevices"));
    setIf(vkGetPhysicalDeviceExternalBufferProperties,
          early("vkGetPhysicalDeviceExternalBufferProperties"));
    setIf(vkFlushMappedMemoryRanges, early("vkFlushMappedMemoryRanges"));
    setIf(vkFreeCommandBuffers, early("vkFreeCommandBuffers"));
    setIf(vkFreeDescriptorSets, early("vkFreeDescriptorSets"));
    setIf(vkFreeMemory, early("vkFreeMemory"));
    setIf(vkGetBufferMemoryRequirements, early("vkGetBufferMemoryRequirements"));
    setIf(vkGetBufferMemoryRequirements2, early("vkGetBufferMemoryRequirements2"));
    if (!vkGetBufferMemoryRequirements2)
        setIf(vkGetBufferMemoryRequirements2, early("vkGetBufferMemoryRequirements2KHR"));
    setIf(vkGetDeviceProcAddr, early("vkGetDeviceProcAddr"));
    setIf(vkGetDeviceQueue, early("vkGetDeviceQueue"));
    setIf(vkGetFenceStatus, early("vkGetFenceStatus"));
    setIf(vkGetImageMemoryRequirements, early("vkGetImageMemoryRequirements"));
    setIf(vkGetImageMemoryRequirements2, early("vkGetImageMemoryRequirements2"));
    if (!vkGetImageMemoryRequirements2)
        setIf(vkGetImageMemoryRequirements2, early("vkGetImageMemoryRequirements2KHR"));
    setIf(vkGetImageSubresourceLayout, early("vkGetImageSubresourceLayout"));
    setIf(vkGetPhysicalDeviceFeatures, early("vkGetPhysicalDeviceFeatures"));
    setIf(vkGetPhysicalDeviceFeatures2, early("vkGetPhysicalDeviceFeatures2"));
    setIf(vkGetPhysicalDeviceFormatProperties, early("vkGetPhysicalDeviceFormatProperties"));
    setIf(vkGetPhysicalDeviceFormatProperties2, early("vkGetPhysicalDeviceFormatProperties2"));
    setIf(vkGetPhysicalDeviceImageFormatProperties, early("vkGetPhysicalDeviceImageFormatProperties"));
    setIf(vkGetPhysicalDeviceImageFormatProperties2,
          early("vkGetPhysicalDeviceImageFormatProperties2"));
    setIf(vkGetPhysicalDeviceMemoryProperties, early("vkGetPhysicalDeviceMemoryProperties"));
    setIf(vkGetPhysicalDeviceMemoryProperties2, early("vkGetPhysicalDeviceMemoryProperties2"));
    setIf(vkGetPhysicalDeviceProperties, early("vkGetPhysicalDeviceProperties"));
    setIf(vkGetPhysicalDeviceProperties2, early("vkGetPhysicalDeviceProperties2"));
    if (!vkGetPhysicalDeviceProperties2)
        setIf(vkGetPhysicalDeviceProperties2, early("vkGetPhysicalDeviceProperties2KHR"));
    setIf(vkGetPhysicalDeviceQueueFamilyProperties, early("vkGetPhysicalDeviceQueueFamilyProperties"));
    setIf(vkGetPhysicalDeviceSurfaceCapabilitiesKHR,
          early("vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
    setIf(vkGetPhysicalDeviceSurfaceFormatsKHR, early("vkGetPhysicalDeviceSurfaceFormatsKHR"));
    setIf(vkGetPhysicalDeviceSurfaceSupportKHR, early("vkGetPhysicalDeviceSurfaceSupportKHR"));
    setIf(vkGetQueryPoolResults, early("vkGetQueryPoolResults"));
    setIf(vkGetSwapchainImagesKHR, early("vkGetSwapchainImagesKHR"));
    setIf(vkInvalidateMappedMemoryRanges, early("vkInvalidateMappedMemoryRanges"));
    setIf(vkMapMemory, early("vkMapMemory"));
    setIf(vkQueuePresentKHR, early("vkQueuePresentKHR"));
    setIf(vkQueueSubmit, early("vkQueueSubmit"));
    setIf(vkQueueWaitIdle, early("vkQueueWaitIdle"));
    setIf(vkResetCommandBuffer, early("vkResetCommandBuffer"));
    setIf(vkResetDescriptorPool, early("vkResetDescriptorPool"));
    setIf(vkResetFences, early("vkResetFences"));
    setIf(vkUnmapMemory, early("vkUnmapMemory"));
    setIf(vkUpdateDescriptorSets, early("vkUpdateDescriptorSets"));
    setIf(vkWaitForFences, early("vkWaitForFences"));

    __android_log_print(ANDROID_LOG_INFO, "RawrCamNative", "GPU_DRIVER_SELECTED backend=%s", gDesc.c_str());
}
void loadInstance(VkInstance instance) {
    std::lock_guard<std::mutex> lock(gMu);
    if (!gGipa) throw std::runtime_error("Vulkan dispatch not configured");
    setIf(vkAcquireNextImageKHR, gGipa(instance, "vkAcquireNextImageKHR"));
    setIf(vkAllocateCommandBuffers, gGipa(instance, "vkAllocateCommandBuffers"));
    setIf(vkAllocateDescriptorSets, gGipa(instance, "vkAllocateDescriptorSets"));
    setIf(vkAllocateMemory, gGipa(instance, "vkAllocateMemory"));
    setIf(vkBeginCommandBuffer, gGipa(instance, "vkBeginCommandBuffer"));
    setIf(vkBindBufferMemory, gGipa(instance, "vkBindBufferMemory"));
    setIf(vkBindImageMemory, gGipa(instance, "vkBindImageMemory"));
    setIf(vkCmdBeginRenderPass, gGipa(instance, "vkCmdBeginRenderPass"));
    setIf(vkCmdBindDescriptorSets, gGipa(instance, "vkCmdBindDescriptorSets"));
    setIf(vkCmdBindPipeline, gGipa(instance, "vkCmdBindPipeline"));
    setIf(vkCmdBlitImage, gGipa(instance, "vkCmdBlitImage"));
    setIf(vkCmdClearColorImage, gGipa(instance, "vkCmdClearColorImage"));
    setIf(vkCmdCopyBuffer, gGipa(instance, "vkCmdCopyBuffer"));
    setIf(vkCmdCopyBufferToImage, gGipa(instance, "vkCmdCopyBufferToImage"));
    setIf(vkCmdCopyImage, gGipa(instance, "vkCmdCopyImage"));
    setIf(vkCmdCopyImageToBuffer, gGipa(instance, "vkCmdCopyImageToBuffer"));
    setIf(vkCmdDispatch, gGipa(instance, "vkCmdDispatch"));
    setIf(vkCmdDispatchIndirect, gGipa(instance, "vkCmdDispatchIndirect"));
    setIf(vkCmdDispatchBase, gGipa(instance, "vkCmdDispatchBase"));
    setIf(vkCmdDraw, gGipa(instance, "vkCmdDraw"));
    setIf(vkCmdEndRenderPass, gGipa(instance, "vkCmdEndRenderPass"));
    setIf(vkCmdFillBuffer, gGipa(instance, "vkCmdFillBuffer"));
    setIf(vkCmdUpdateBuffer, gGipa(instance, "vkCmdUpdateBuffer"));
    setIf(vkCmdPipelineBarrier, gGipa(instance, "vkCmdPipelineBarrier"));
    setIf(vkCmdPushConstants, gGipa(instance, "vkCmdPushConstants"));
    setIf(vkCmdResetQueryPool, gGipa(instance, "vkCmdResetQueryPool"));
    setIf(vkCmdSetScissor, gGipa(instance, "vkCmdSetScissor"));
    setIf(vkCmdSetViewport, gGipa(instance, "vkCmdSetViewport"));
    setIf(vkCmdWriteTimestamp, gGipa(instance, "vkCmdWriteTimestamp"));
    setIf(vkCreateAndroidSurfaceKHR, gGipa(instance, "vkCreateAndroidSurfaceKHR"));
    setIf(vkCreateBuffer, gGipa(instance, "vkCreateBuffer"));
    setIf(vkCreateCommandPool, gGipa(instance, "vkCreateCommandPool"));
    setIf(vkCreateComputePipelines, gGipa(instance, "vkCreateComputePipelines"));
    setIf(vkCreateDescriptorPool, gGipa(instance, "vkCreateDescriptorPool"));
    setIf(vkCreateDescriptorSetLayout, gGipa(instance, "vkCreateDescriptorSetLayout"));
    setIf(vkCreateDevice, gGipa(instance, "vkCreateDevice"));
    setIf(vkCreateFence, gGipa(instance, "vkCreateFence"));
    setIf(vkCreateFramebuffer, gGipa(instance, "vkCreateFramebuffer"));
    setIf(vkCreateGraphicsPipelines, gGipa(instance, "vkCreateGraphicsPipelines"));
    setIf(vkCreateImage, gGipa(instance, "vkCreateImage"));
    setIf(vkCreateImageView, gGipa(instance, "vkCreateImageView"));
    setIf(vkCreateInstance, gGipa(instance, "vkCreateInstance"));
    setIf(vkCreatePipelineLayout, gGipa(instance, "vkCreatePipelineLayout"));
    setIf(vkCreatePipelineCache, gGipa(instance, "vkCreatePipelineCache"));
    setIf(vkDestroyPipelineCache, gGipa(instance, "vkDestroyPipelineCache"));
    setIf(vkGetPipelineCacheData, gGipa(instance, "vkGetPipelineCacheData"));
    setIf(vkCreateQueryPool, gGipa(instance, "vkCreateQueryPool"));
    setIf(vkCreateRenderPass, gGipa(instance, "vkCreateRenderPass"));
    setIf(vkCreateSampler, gGipa(instance, "vkCreateSampler"));
    setIf(vkCreateSamplerYcbcrConversion, gGipa(instance, "vkCreateSamplerYcbcrConversion"));
    setIf(vkCreateSemaphore, gGipa(instance, "vkCreateSemaphore"));
    setIf(vkCreateShaderModule, gGipa(instance, "vkCreateShaderModule"));
    setIf(vkCreateSwapchainKHR, gGipa(instance, "vkCreateSwapchainKHR"));
    setIf(vkDestroyBuffer, gGipa(instance, "vkDestroyBuffer"));
    setIf(vkDestroyCommandPool, gGipa(instance, "vkDestroyCommandPool"));
    setIf(vkDestroyDescriptorPool, gGipa(instance, "vkDestroyDescriptorPool"));
    setIf(vkDestroyDescriptorSetLayout, gGipa(instance, "vkDestroyDescriptorSetLayout"));
    setIf(vkDestroyDevice, gGipa(instance, "vkDestroyDevice"));
    setIf(vkDestroyFence, gGipa(instance, "vkDestroyFence"));
    setIf(vkDestroyFramebuffer, gGipa(instance, "vkDestroyFramebuffer"));
    setIf(vkDestroyImage, gGipa(instance, "vkDestroyImage"));
    setIf(vkDestroyImageView, gGipa(instance, "vkDestroyImageView"));
    setIf(vkDestroyInstance, gGipa(instance, "vkDestroyInstance"));
    setIf(vkDestroyPipeline, gGipa(instance, "vkDestroyPipeline"));
    setIf(vkDestroyPipelineLayout, gGipa(instance, "vkDestroyPipelineLayout"));
    setIf(vkDestroyQueryPool, gGipa(instance, "vkDestroyQueryPool"));
    setIf(vkDestroyRenderPass, gGipa(instance, "vkDestroyRenderPass"));
    setIf(vkDestroySampler, gGipa(instance, "vkDestroySampler"));
    setIf(vkDestroySamplerYcbcrConversion, gGipa(instance, "vkDestroySamplerYcbcrConversion"));
    setIf(vkDestroySemaphore, gGipa(instance, "vkDestroySemaphore"));
    setIf(vkDestroyShaderModule, gGipa(instance, "vkDestroyShaderModule"));
    setIf(vkDestroySurfaceKHR, gGipa(instance, "vkDestroySurfaceKHR"));
    setIf(vkDestroySwapchainKHR, gGipa(instance, "vkDestroySwapchainKHR"));
    setIf(vkDeviceWaitIdle, gGipa(instance, "vkDeviceWaitIdle"));
    setIf(vkEndCommandBuffer, gGipa(instance, "vkEndCommandBuffer"));
    setIf(vkEnumerateDeviceExtensionProperties, gGipa(instance, "vkEnumerateDeviceExtensionProperties"));
    setIf(vkEnumerateInstanceExtensionProperties, gGipa(instance, "vkEnumerateInstanceExtensionProperties"));
    setIf(vkEnumeratePhysicalDevices, gGipa(instance, "vkEnumeratePhysicalDevices"));
    setIf(vkFlushMappedMemoryRanges, gGipa(instance, "vkFlushMappedMemoryRanges"));
    setIf(vkFreeCommandBuffers, gGipa(instance, "vkFreeCommandBuffers"));
    setIf(vkFreeDescriptorSets, gGipa(instance, "vkFreeDescriptorSets"));
    setIf(vkFreeMemory, gGipa(instance, "vkFreeMemory"));
    setIf(vkGetBufferMemoryRequirements, gGipa(instance, "vkGetBufferMemoryRequirements"));
    setIf(vkGetBufferMemoryRequirements2, gGipa(instance, "vkGetBufferMemoryRequirements2"));
    if (!vkGetBufferMemoryRequirements2)
        setIf(vkGetBufferMemoryRequirements2, gGipa(instance, "vkGetBufferMemoryRequirements2KHR"));
    setIf(vkGetDeviceProcAddr, gGipa(instance, "vkGetDeviceProcAddr"));
    setIf(vkGetDeviceQueue, gGipa(instance, "vkGetDeviceQueue"));
    setIf(vkGetFenceStatus, gGipa(instance, "vkGetFenceStatus"));
    setIf(vkGetImageMemoryRequirements, gGipa(instance, "vkGetImageMemoryRequirements"));
    setIf(vkGetImageMemoryRequirements2, gGipa(instance, "vkGetImageMemoryRequirements2"));
    if (!vkGetImageMemoryRequirements2)
        setIf(vkGetImageMemoryRequirements2, gGipa(instance, "vkGetImageMemoryRequirements2KHR"));
    setIf(vkGetImageSubresourceLayout, gGipa(instance, "vkGetImageSubresourceLayout"));
    setIf(vkGetPhysicalDeviceFeatures, gGipa(instance, "vkGetPhysicalDeviceFeatures"));
    setIf(vkGetPhysicalDeviceFeatures2, gGipa(instance, "vkGetPhysicalDeviceFeatures2"));
    setIf(vkGetPhysicalDeviceFormatProperties, gGipa(instance, "vkGetPhysicalDeviceFormatProperties"));
    setIf(vkGetPhysicalDeviceFormatProperties2, gGipa(instance, "vkGetPhysicalDeviceFormatProperties2"));
    setIf(vkGetPhysicalDeviceImageFormatProperties, gGipa(instance, "vkGetPhysicalDeviceImageFormatProperties"));
    setIf(vkGetPhysicalDeviceImageFormatProperties2, gGipa(instance, "vkGetPhysicalDeviceImageFormatProperties2"));
    setIf(vkGetPhysicalDeviceMemoryProperties, gGipa(instance, "vkGetPhysicalDeviceMemoryProperties"));
    setIf(vkGetPhysicalDeviceProperties, gGipa(instance, "vkGetPhysicalDeviceProperties"));
    setIf(vkGetPhysicalDeviceProperties2, gGipa(instance, "vkGetPhysicalDeviceProperties2"));
    if (!vkGetPhysicalDeviceProperties2)
        setIf(vkGetPhysicalDeviceProperties2, gGipa(instance, "vkGetPhysicalDeviceProperties2KHR"));
    setIf(vkGetPhysicalDeviceQueueFamilyProperties, gGipa(instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
    setIf(vkGetPhysicalDeviceSurfaceCapabilitiesKHR, gGipa(instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
    setIf(vkGetPhysicalDeviceSurfaceFormatsKHR, gGipa(instance, "vkGetPhysicalDeviceSurfaceFormatsKHR"));
    setIf(vkGetPhysicalDeviceSurfaceSupportKHR, gGipa(instance, "vkGetPhysicalDeviceSurfaceSupportKHR"));
    setIf(vkGetQueryPoolResults, gGipa(instance, "vkGetQueryPoolResults"));
    setIf(vkGetSwapchainImagesKHR, gGipa(instance, "vkGetSwapchainImagesKHR"));
    setIf(vkInvalidateMappedMemoryRanges, gGipa(instance, "vkInvalidateMappedMemoryRanges"));
    setIf(vkMapMemory, gGipa(instance, "vkMapMemory"));
    setIf(vkQueuePresentKHR, gGipa(instance, "vkQueuePresentKHR"));
    setIf(vkQueueSubmit, gGipa(instance, "vkQueueSubmit"));
    setIf(vkQueueWaitIdle, gGipa(instance, "vkQueueWaitIdle"));
    setIf(vkResetCommandBuffer, gGipa(instance, "vkResetCommandBuffer"));
    setIf(vkResetDescriptorPool, gGipa(instance, "vkResetDescriptorPool"));
    setIf(vkResetFences, gGipa(instance, "vkResetFences"));
    setIf(vkUnmapMemory, gGipa(instance, "vkUnmapMemory"));
    setIf(vkUpdateDescriptorSets, gGipa(instance, "vkUpdateDescriptorSets"));
    setIf(vkWaitForFences, gGipa(instance, "vkWaitForFences"));
    gGdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gGipa(instance, "vkGetDeviceProcAddr"));
    if (gGdpa) vkGetDeviceProcAddr = gGdpa;
}
void loadDevice(VkDevice device) {
    std::lock_guard<std::mutex> lock(gMu);
    if (!gGdpa) throw std::runtime_error("Vulkan device dispatch unavailable");
    setIf(vkAcquireNextImageKHR, gGdpa(device, "vkAcquireNextImageKHR"));
    setIf(vkAllocateCommandBuffers, gGdpa(device, "vkAllocateCommandBuffers"));
    setIf(vkAllocateDescriptorSets, gGdpa(device, "vkAllocateDescriptorSets"));
    setIf(vkAllocateMemory, gGdpa(device, "vkAllocateMemory"));
    setIf(vkBeginCommandBuffer, gGdpa(device, "vkBeginCommandBuffer"));
    setIf(vkBindBufferMemory, gGdpa(device, "vkBindBufferMemory"));
    setIf(vkBindImageMemory, gGdpa(device, "vkBindImageMemory"));
    setIf(vkCmdBeginRenderPass, gGdpa(device, "vkCmdBeginRenderPass"));
    setIf(vkCmdBindDescriptorSets, gGdpa(device, "vkCmdBindDescriptorSets"));
    setIf(vkCmdBindPipeline, gGdpa(device, "vkCmdBindPipeline"));
    setIf(vkCmdBlitImage, gGdpa(device, "vkCmdBlitImage"));
    setIf(vkCmdClearColorImage, gGdpa(device, "vkCmdClearColorImage"));
    setIf(vkCmdCopyBuffer, gGdpa(device, "vkCmdCopyBuffer"));
    setIf(vkCmdCopyBufferToImage, gGdpa(device, "vkCmdCopyBufferToImage"));
    setIf(vkCmdCopyImage, gGdpa(device, "vkCmdCopyImage"));
    setIf(vkCmdCopyImageToBuffer, gGdpa(device, "vkCmdCopyImageToBuffer"));
    setIf(vkCmdDispatch, gGdpa(device, "vkCmdDispatch"));
    setIf(vkCmdDispatchIndirect, gGdpa(device, "vkCmdDispatchIndirect"));
    setIf(vkCmdDispatchBase, gGdpa(device, "vkCmdDispatchBase"));
    setIf(vkCmdDraw, gGdpa(device, "vkCmdDraw"));
    setIf(vkCmdEndRenderPass, gGdpa(device, "vkCmdEndRenderPass"));
    setIf(vkCmdFillBuffer, gGdpa(device, "vkCmdFillBuffer"));
    setIf(vkCmdUpdateBuffer, gGdpa(device, "vkCmdUpdateBuffer"));
    setIf(vkCmdPipelineBarrier, gGdpa(device, "vkCmdPipelineBarrier"));
    setIf(vkCmdPushConstants, gGdpa(device, "vkCmdPushConstants"));
    setIf(vkCmdResetQueryPool, gGdpa(device, "vkCmdResetQueryPool"));
    setIf(vkCmdSetScissor, gGdpa(device, "vkCmdSetScissor"));
    setIf(vkCmdSetViewport, gGdpa(device, "vkCmdSetViewport"));
    setIf(vkCmdWriteTimestamp, gGdpa(device, "vkCmdWriteTimestamp"));
    setIf(vkCreateAndroidSurfaceKHR, gGdpa(device, "vkCreateAndroidSurfaceKHR"));
    setIf(vkCreateBuffer, gGdpa(device, "vkCreateBuffer"));
    setIf(vkCreateCommandPool, gGdpa(device, "vkCreateCommandPool"));
    setIf(vkCreateComputePipelines, gGdpa(device, "vkCreateComputePipelines"));
    setIf(vkCreateDescriptorPool, gGdpa(device, "vkCreateDescriptorPool"));
    setIf(vkCreateDescriptorSetLayout, gGdpa(device, "vkCreateDescriptorSetLayout"));
    setIf(vkCreateDevice, gGdpa(device, "vkCreateDevice"));
    setIf(vkCreateFence, gGdpa(device, "vkCreateFence"));
    setIf(vkCreateFramebuffer, gGdpa(device, "vkCreateFramebuffer"));
    setIf(vkCreateGraphicsPipelines, gGdpa(device, "vkCreateGraphicsPipelines"));
    setIf(vkCreateImage, gGdpa(device, "vkCreateImage"));
    setIf(vkCreateImageView, gGdpa(device, "vkCreateImageView"));
    setIf(vkCreateInstance, gGdpa(device, "vkCreateInstance"));
    setIf(vkCreatePipelineLayout, gGdpa(device, "vkCreatePipelineLayout"));
    setIf(vkCreatePipelineCache, gGdpa(device, "vkCreatePipelineCache"));
    setIf(vkDestroyPipelineCache, gGdpa(device, "vkDestroyPipelineCache"));
    setIf(vkGetPipelineCacheData, gGdpa(device, "vkGetPipelineCacheData"));
    setIf(vkCreateQueryPool, gGdpa(device, "vkCreateQueryPool"));
    setIf(vkCreateRenderPass, gGdpa(device, "vkCreateRenderPass"));
    setIf(vkCreateSampler, gGdpa(device, "vkCreateSampler"));
    setIf(vkCreateSamplerYcbcrConversion, gGdpa(device, "vkCreateSamplerYcbcrConversion"));
    setIf(vkCreateSemaphore, gGdpa(device, "vkCreateSemaphore"));
    setIf(vkCreateShaderModule, gGdpa(device, "vkCreateShaderModule"));
    setIf(vkCreateSwapchainKHR, gGdpa(device, "vkCreateSwapchainKHR"));
    setIf(vkDestroyBuffer, gGdpa(device, "vkDestroyBuffer"));
    setIf(vkDestroyCommandPool, gGdpa(device, "vkDestroyCommandPool"));
    setIf(vkDestroyDescriptorPool, gGdpa(device, "vkDestroyDescriptorPool"));
    setIf(vkDestroyDescriptorSetLayout, gGdpa(device, "vkDestroyDescriptorSetLayout"));
    setIf(vkDestroyDevice, gGdpa(device, "vkDestroyDevice"));
    setIf(vkDestroyFence, gGdpa(device, "vkDestroyFence"));
    setIf(vkDestroyFramebuffer, gGdpa(device, "vkDestroyFramebuffer"));
    setIf(vkDestroyImage, gGdpa(device, "vkDestroyImage"));
    setIf(vkDestroyImageView, gGdpa(device, "vkDestroyImageView"));
    setIf(vkDestroyInstance, gGdpa(device, "vkDestroyInstance"));
    setIf(vkDestroyPipeline, gGdpa(device, "vkDestroyPipeline"));
    setIf(vkDestroyPipelineLayout, gGdpa(device, "vkDestroyPipelineLayout"));
    setIf(vkDestroyQueryPool, gGdpa(device, "vkDestroyQueryPool"));
    setIf(vkDestroyRenderPass, gGdpa(device, "vkDestroyRenderPass"));
    setIf(vkDestroySampler, gGdpa(device, "vkDestroySampler"));
    setIf(vkDestroySamplerYcbcrConversion, gGdpa(device, "vkDestroySamplerYcbcrConversion"));
    setIf(vkDestroySemaphore, gGdpa(device, "vkDestroySemaphore"));
    setIf(vkDestroyShaderModule, gGdpa(device, "vkDestroyShaderModule"));
    setIf(vkDestroySurfaceKHR, gGdpa(device, "vkDestroySurfaceKHR"));
    setIf(vkDestroySwapchainKHR, gGdpa(device, "vkDestroySwapchainKHR"));
    setIf(vkDeviceWaitIdle, gGdpa(device, "vkDeviceWaitIdle"));
    setIf(vkEndCommandBuffer, gGdpa(device, "vkEndCommandBuffer"));
    setIf(vkEnumerateDeviceExtensionProperties, gGdpa(device, "vkEnumerateDeviceExtensionProperties"));
    setIf(vkEnumerateInstanceExtensionProperties, gGdpa(device, "vkEnumerateInstanceExtensionProperties"));
    setIf(vkEnumeratePhysicalDevices, gGdpa(device, "vkEnumeratePhysicalDevices"));
    setIf(vkFlushMappedMemoryRanges, gGdpa(device, "vkFlushMappedMemoryRanges"));
    setIf(vkFreeCommandBuffers, gGdpa(device, "vkFreeCommandBuffers"));
    setIf(vkFreeDescriptorSets, gGdpa(device, "vkFreeDescriptorSets"));
    setIf(vkFreeMemory, gGdpa(device, "vkFreeMemory"));
    setIf(vkGetBufferMemoryRequirements, gGdpa(device, "vkGetBufferMemoryRequirements"));
    setIf(vkGetBufferMemoryRequirements2, gGdpa(device, "vkGetBufferMemoryRequirements2"));
    if (!vkGetBufferMemoryRequirements2)
        setIf(vkGetBufferMemoryRequirements2, gGdpa(device, "vkGetBufferMemoryRequirements2KHR"));
    setIf(vkGetDeviceProcAddr, gGdpa(device, "vkGetDeviceProcAddr"));
    setIf(vkGetDeviceQueue, gGdpa(device, "vkGetDeviceQueue"));
    setIf(vkGetFenceStatus, gGdpa(device, "vkGetFenceStatus"));
    setIf(vkGetImageMemoryRequirements, gGdpa(device, "vkGetImageMemoryRequirements"));
    setIf(vkGetImageMemoryRequirements2, gGdpa(device, "vkGetImageMemoryRequirements2"));
    if (!vkGetImageMemoryRequirements2)
        setIf(vkGetImageMemoryRequirements2, gGdpa(device, "vkGetImageMemoryRequirements2KHR"));
    setIf(vkGetImageSubresourceLayout, gGdpa(device, "vkGetImageSubresourceLayout"));
    setIf(vkGetPhysicalDeviceFeatures, gGdpa(device, "vkGetPhysicalDeviceFeatures"));
    setIf(vkGetPhysicalDeviceFeatures2, gGdpa(device, "vkGetPhysicalDeviceFeatures2"));
    setIf(vkGetPhysicalDeviceFormatProperties, gGdpa(device, "vkGetPhysicalDeviceFormatProperties"));
    setIf(vkGetPhysicalDeviceFormatProperties2, gGdpa(device, "vkGetPhysicalDeviceFormatProperties2"));
    setIf(vkGetPhysicalDeviceImageFormatProperties, gGdpa(device, "vkGetPhysicalDeviceImageFormatProperties"));
    setIf(vkGetPhysicalDeviceImageFormatProperties2, gGdpa(device, "vkGetPhysicalDeviceImageFormatProperties2"));
    setIf(vkGetPhysicalDeviceMemoryProperties, gGdpa(device, "vkGetPhysicalDeviceMemoryProperties"));
    setIf(vkGetPhysicalDeviceProperties, gGdpa(device, "vkGetPhysicalDeviceProperties"));
    setIf(vkGetPhysicalDeviceQueueFamilyProperties, gGdpa(device, "vkGetPhysicalDeviceQueueFamilyProperties"));
    setIf(vkGetPhysicalDeviceSurfaceCapabilitiesKHR, gGdpa(device, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
    setIf(vkGetPhysicalDeviceSurfaceFormatsKHR, gGdpa(device, "vkGetPhysicalDeviceSurfaceFormatsKHR"));
    setIf(vkGetPhysicalDeviceSurfaceSupportKHR, gGdpa(device, "vkGetPhysicalDeviceSurfaceSupportKHR"));
    setIf(vkGetQueryPoolResults, gGdpa(device, "vkGetQueryPoolResults"));
    setIf(vkGetSwapchainImagesKHR, gGdpa(device, "vkGetSwapchainImagesKHR"));
    setIf(vkInvalidateMappedMemoryRanges, gGdpa(device, "vkInvalidateMappedMemoryRanges"));
    setIf(vkMapMemory, gGdpa(device, "vkMapMemory"));
    setIf(vkQueuePresentKHR, gGdpa(device, "vkQueuePresentKHR"));
    setIf(vkQueueSubmit, gGdpa(device, "vkQueueSubmit"));
    setIf(vkQueueWaitIdle, gGdpa(device, "vkQueueWaitIdle"));
    setIf(vkResetCommandBuffer, gGdpa(device, "vkResetCommandBuffer"));
    setIf(vkResetDescriptorPool, gGdpa(device, "vkResetDescriptorPool"));
    setIf(vkResetFences, gGdpa(device, "vkResetFences"));
    setIf(vkUnmapMemory, gGdpa(device, "vkUnmapMemory"));
    setIf(vkUpdateDescriptorSets, gGdpa(device, "vkUpdateDescriptorSets"));
    setIf(vkWaitForFences, gGdpa(device, "vkWaitForFences"));
    // Route compute pipeline creation through the shared cache (see header).
    if (vkCreateComputePipelines && vkCreateComputePipelines != cachedCreateComputePipelines) {
        gDriverCreateComputePipelines = vkCreateComputePipelines;
        vkCreateComputePipelines = cachedCreateComputePipelines;
    }
}
const std::string& backendDescription() { return gDesc; }
}  // namespace rawrcam::vulkan::dispatch
