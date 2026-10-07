#include "VulkanDispatch.h"

#include <adrenotools/driver.h>
#include <android/log.h>
#include <dlfcn.h>

#include <mutex>
#include <stdexcept>

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
        VkPipelineCacheCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
        if (vkCreatePipelineCache(device, &info, nullptr, &gCache) != VK_SUCCESS) return VK_NULL_HANDLE;
        gCacheDevice = device;
    }
    return gCache;
}
void releasePipelineCache(VkDevice device) {
    std::lock_guard<std::mutex> lock(gCacheMutex);
    if (device != VK_NULL_HANDLE && device == gCacheDevice && gCache != VK_NULL_HANDLE && vkDestroyPipelineCache)
        vkDestroyPipelineCache(device, gCache, nullptr);
    if (device == gCacheDevice) {
        gCache = VK_NULL_HANDLE;
        gCacheDevice = VK_NULL_HANDLE;
    }
}
void configure(const std::string& nativeLibraryDir, const std::string& path) {
    std::lock_guard<std::mutex> lock(gMu);
    if (gLib) return;
    clearFns();
    const bool requested = !path.empty();
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
    setIf(vkAcquireNextImageKHR, gGipa(VK_NULL_HANDLE, "vkAcquireNextImageKHR"));
    setIf(vkAllocateCommandBuffers, gGipa(VK_NULL_HANDLE, "vkAllocateCommandBuffers"));
    setIf(vkAllocateDescriptorSets, gGipa(VK_NULL_HANDLE, "vkAllocateDescriptorSets"));
    setIf(vkAllocateMemory, gGipa(VK_NULL_HANDLE, "vkAllocateMemory"));
    setIf(vkBeginCommandBuffer, gGipa(VK_NULL_HANDLE, "vkBeginCommandBuffer"));
    setIf(vkBindBufferMemory, gGipa(VK_NULL_HANDLE, "vkBindBufferMemory"));
    setIf(vkBindImageMemory, gGipa(VK_NULL_HANDLE, "vkBindImageMemory"));
    setIf(vkCmdBeginRenderPass, gGipa(VK_NULL_HANDLE, "vkCmdBeginRenderPass"));
    setIf(vkCmdBindDescriptorSets, gGipa(VK_NULL_HANDLE, "vkCmdBindDescriptorSets"));
    setIf(vkCmdBindPipeline, gGipa(VK_NULL_HANDLE, "vkCmdBindPipeline"));
    setIf(vkCmdBlitImage, gGipa(VK_NULL_HANDLE, "vkCmdBlitImage"));
    setIf(vkCmdClearColorImage, gGipa(VK_NULL_HANDLE, "vkCmdClearColorImage"));
    setIf(vkCmdCopyBuffer, gGipa(VK_NULL_HANDLE, "vkCmdCopyBuffer"));
    setIf(vkCmdCopyBufferToImage, gGipa(VK_NULL_HANDLE, "vkCmdCopyBufferToImage"));
    setIf(vkCmdCopyImage, gGipa(VK_NULL_HANDLE, "vkCmdCopyImage"));
    setIf(vkCmdCopyImageToBuffer, gGipa(VK_NULL_HANDLE, "vkCmdCopyImageToBuffer"));
    setIf(vkCmdDispatch, gGipa(VK_NULL_HANDLE, "vkCmdDispatch"));
    setIf(vkCmdDispatchBase, gGipa(VK_NULL_HANDLE, "vkCmdDispatchBase"));
    setIf(vkCmdDraw, gGipa(VK_NULL_HANDLE, "vkCmdDraw"));
    setIf(vkCmdEndRenderPass, gGipa(VK_NULL_HANDLE, "vkCmdEndRenderPass"));
    setIf(vkCmdFillBuffer, gGipa(VK_NULL_HANDLE, "vkCmdFillBuffer"));
    setIf(vkCmdUpdateBuffer, gGipa(VK_NULL_HANDLE, "vkCmdUpdateBuffer"));
    setIf(vkCmdPipelineBarrier, gGipa(VK_NULL_HANDLE, "vkCmdPipelineBarrier"));
    setIf(vkCmdPushConstants, gGipa(VK_NULL_HANDLE, "vkCmdPushConstants"));
    setIf(vkCmdResetQueryPool, gGipa(VK_NULL_HANDLE, "vkCmdResetQueryPool"));
    setIf(vkCmdSetScissor, gGipa(VK_NULL_HANDLE, "vkCmdSetScissor"));
    setIf(vkCmdSetViewport, gGipa(VK_NULL_HANDLE, "vkCmdSetViewport"));
    setIf(vkCmdWriteTimestamp, gGipa(VK_NULL_HANDLE, "vkCmdWriteTimestamp"));
    setIf(vkCreateAndroidSurfaceKHR, gGipa(VK_NULL_HANDLE, "vkCreateAndroidSurfaceKHR"));
    setIf(vkCreateBuffer, gGipa(VK_NULL_HANDLE, "vkCreateBuffer"));
    setIf(vkCreateCommandPool, gGipa(VK_NULL_HANDLE, "vkCreateCommandPool"));
    setIf(vkCreateComputePipelines, gGipa(VK_NULL_HANDLE, "vkCreateComputePipelines"));
    setIf(vkCreateDescriptorPool, gGipa(VK_NULL_HANDLE, "vkCreateDescriptorPool"));
    setIf(vkCreateDescriptorSetLayout, gGipa(VK_NULL_HANDLE, "vkCreateDescriptorSetLayout"));
    setIf(vkCreateDevice, gGipa(VK_NULL_HANDLE, "vkCreateDevice"));
    setIf(vkCreateFence, gGipa(VK_NULL_HANDLE, "vkCreateFence"));
    setIf(vkCreateFramebuffer, gGipa(VK_NULL_HANDLE, "vkCreateFramebuffer"));
    setIf(vkCreateGraphicsPipelines, gGipa(VK_NULL_HANDLE, "vkCreateGraphicsPipelines"));
    setIf(vkCreateImage, gGipa(VK_NULL_HANDLE, "vkCreateImage"));
    setIf(vkCreateImageView, gGipa(VK_NULL_HANDLE, "vkCreateImageView"));
    setIf(vkCreateInstance, gGipa(VK_NULL_HANDLE, "vkCreateInstance"));
    setIf(vkCreatePipelineLayout, gGipa(VK_NULL_HANDLE, "vkCreatePipelineLayout"));
    setIf(vkCreatePipelineCache, gGipa(VK_NULL_HANDLE, "vkCreatePipelineCache"));
    setIf(vkDestroyPipelineCache, gGipa(VK_NULL_HANDLE, "vkDestroyPipelineCache"));
    setIf(vkCreateQueryPool, gGipa(VK_NULL_HANDLE, "vkCreateQueryPool"));
    setIf(vkCreateRenderPass, gGipa(VK_NULL_HANDLE, "vkCreateRenderPass"));
    setIf(vkCreateSampler, gGipa(VK_NULL_HANDLE, "vkCreateSampler"));
    setIf(vkCreateSamplerYcbcrConversion, gGipa(VK_NULL_HANDLE, "vkCreateSamplerYcbcrConversion"));
    setIf(vkCreateSemaphore, gGipa(VK_NULL_HANDLE, "vkCreateSemaphore"));
    setIf(vkCreateShaderModule, gGipa(VK_NULL_HANDLE, "vkCreateShaderModule"));
    setIf(vkCreateSwapchainKHR, gGipa(VK_NULL_HANDLE, "vkCreateSwapchainKHR"));
    setIf(vkDestroyBuffer, gGipa(VK_NULL_HANDLE, "vkDestroyBuffer"));
    setIf(vkDestroyCommandPool, gGipa(VK_NULL_HANDLE, "vkDestroyCommandPool"));
    setIf(vkDestroyDescriptorPool, gGipa(VK_NULL_HANDLE, "vkDestroyDescriptorPool"));
    setIf(vkDestroyDescriptorSetLayout, gGipa(VK_NULL_HANDLE, "vkDestroyDescriptorSetLayout"));
    setIf(vkDestroyDevice, gGipa(VK_NULL_HANDLE, "vkDestroyDevice"));
    setIf(vkDestroyFence, gGipa(VK_NULL_HANDLE, "vkDestroyFence"));
    setIf(vkDestroyFramebuffer, gGipa(VK_NULL_HANDLE, "vkDestroyFramebuffer"));
    setIf(vkDestroyImage, gGipa(VK_NULL_HANDLE, "vkDestroyImage"));
    setIf(vkDestroyImageView, gGipa(VK_NULL_HANDLE, "vkDestroyImageView"));
    setIf(vkDestroyInstance, gGipa(VK_NULL_HANDLE, "vkDestroyInstance"));
    setIf(vkDestroyPipeline, gGipa(VK_NULL_HANDLE, "vkDestroyPipeline"));
    setIf(vkDestroyPipelineLayout, gGipa(VK_NULL_HANDLE, "vkDestroyPipelineLayout"));
    setIf(vkDestroyQueryPool, gGipa(VK_NULL_HANDLE, "vkDestroyQueryPool"));
    setIf(vkDestroyRenderPass, gGipa(VK_NULL_HANDLE, "vkDestroyRenderPass"));
    setIf(vkDestroySampler, gGipa(VK_NULL_HANDLE, "vkDestroySampler"));
    setIf(vkDestroySamplerYcbcrConversion, gGipa(VK_NULL_HANDLE, "vkDestroySamplerYcbcrConversion"));
    setIf(vkDestroySemaphore, gGipa(VK_NULL_HANDLE, "vkDestroySemaphore"));
    setIf(vkDestroyShaderModule, gGipa(VK_NULL_HANDLE, "vkDestroyShaderModule"));
    setIf(vkDestroySurfaceKHR, gGipa(VK_NULL_HANDLE, "vkDestroySurfaceKHR"));
    setIf(vkDestroySwapchainKHR, gGipa(VK_NULL_HANDLE, "vkDestroySwapchainKHR"));
    setIf(vkDeviceWaitIdle, gGipa(VK_NULL_HANDLE, "vkDeviceWaitIdle"));
    setIf(vkEndCommandBuffer, gGipa(VK_NULL_HANDLE, "vkEndCommandBuffer"));
    setIf(vkEnumerateDeviceExtensionProperties, gGipa(VK_NULL_HANDLE, "vkEnumerateDeviceExtensionProperties"));
    setIf(vkEnumerateInstanceExtensionProperties, gGipa(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties"));
    setIf(vkEnumeratePhysicalDevices, gGipa(VK_NULL_HANDLE, "vkEnumeratePhysicalDevices"));
    setIf(vkGetPhysicalDeviceExternalBufferProperties,
          gGipa(VK_NULL_HANDLE, "vkGetPhysicalDeviceExternalBufferProperties"));
    setIf(vkFlushMappedMemoryRanges, gGipa(VK_NULL_HANDLE, "vkFlushMappedMemoryRanges"));
    setIf(vkFreeCommandBuffers, gGipa(VK_NULL_HANDLE, "vkFreeCommandBuffers"));
    setIf(vkFreeDescriptorSets, gGipa(VK_NULL_HANDLE, "vkFreeDescriptorSets"));
    setIf(vkFreeMemory, gGipa(VK_NULL_HANDLE, "vkFreeMemory"));
    setIf(vkGetBufferMemoryRequirements, gGipa(VK_NULL_HANDLE, "vkGetBufferMemoryRequirements"));
    setIf(vkGetBufferMemoryRequirements2, gGipa(VK_NULL_HANDLE, "vkGetBufferMemoryRequirements2"));
    if (!vkGetBufferMemoryRequirements2)
        setIf(vkGetBufferMemoryRequirements2, gGipa(VK_NULL_HANDLE, "vkGetBufferMemoryRequirements2KHR"));
    setIf(vkGetDeviceProcAddr, gGipa(VK_NULL_HANDLE, "vkGetDeviceProcAddr"));
    setIf(vkGetDeviceQueue, gGipa(VK_NULL_HANDLE, "vkGetDeviceQueue"));
    setIf(vkGetFenceStatus, gGipa(VK_NULL_HANDLE, "vkGetFenceStatus"));
    setIf(vkGetImageMemoryRequirements, gGipa(VK_NULL_HANDLE, "vkGetImageMemoryRequirements"));
    setIf(vkGetImageMemoryRequirements2, gGipa(VK_NULL_HANDLE, "vkGetImageMemoryRequirements2"));
    if (!vkGetImageMemoryRequirements2)
        setIf(vkGetImageMemoryRequirements2, gGipa(VK_NULL_HANDLE, "vkGetImageMemoryRequirements2KHR"));
    setIf(vkGetImageSubresourceLayout, gGipa(VK_NULL_HANDLE, "vkGetImageSubresourceLayout"));
    setIf(vkGetPhysicalDeviceFeatures, gGipa(VK_NULL_HANDLE, "vkGetPhysicalDeviceFeatures"));
    setIf(vkGetPhysicalDeviceFeatures2, gGipa(VK_NULL_HANDLE, "vkGetPhysicalDeviceFeatures2"));
    setIf(vkGetPhysicalDeviceFormatProperties, gGipa(VK_NULL_HANDLE, "vkGetPhysicalDeviceFormatProperties"));
    setIf(vkGetPhysicalDeviceFormatProperties2, gGipa(VK_NULL_HANDLE, "vkGetPhysicalDeviceFormatProperties2"));
    setIf(vkGetPhysicalDeviceImageFormatProperties, gGipa(VK_NULL_HANDLE, "vkGetPhysicalDeviceImageFormatProperties"));
    setIf(vkGetPhysicalDeviceImageFormatProperties2,
          gGipa(VK_NULL_HANDLE, "vkGetPhysicalDeviceImageFormatProperties2"));
    setIf(vkGetPhysicalDeviceMemoryProperties, gGipa(VK_NULL_HANDLE, "vkGetPhysicalDeviceMemoryProperties"));
    setIf(vkGetPhysicalDeviceMemoryProperties2, gGipa(VK_NULL_HANDLE, "vkGetPhysicalDeviceMemoryProperties2"));
    setIf(vkGetPhysicalDeviceProperties, gGipa(VK_NULL_HANDLE, "vkGetPhysicalDeviceProperties"));
    setIf(vkGetPhysicalDeviceProperties2, gGipa(VK_NULL_HANDLE, "vkGetPhysicalDeviceProperties2"));
    if (!vkGetPhysicalDeviceProperties2)
        setIf(vkGetPhysicalDeviceProperties2, gGipa(VK_NULL_HANDLE, "vkGetPhysicalDeviceProperties2KHR"));
    setIf(vkGetPhysicalDeviceQueueFamilyProperties, gGipa(VK_NULL_HANDLE, "vkGetPhysicalDeviceQueueFamilyProperties"));
    setIf(vkGetPhysicalDeviceSurfaceCapabilitiesKHR,
          gGipa(VK_NULL_HANDLE, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
    setIf(vkGetPhysicalDeviceSurfaceFormatsKHR, gGipa(VK_NULL_HANDLE, "vkGetPhysicalDeviceSurfaceFormatsKHR"));
    setIf(vkGetPhysicalDeviceSurfaceSupportKHR, gGipa(VK_NULL_HANDLE, "vkGetPhysicalDeviceSurfaceSupportKHR"));
    setIf(vkGetQueryPoolResults, gGipa(VK_NULL_HANDLE, "vkGetQueryPoolResults"));
    setIf(vkGetSwapchainImagesKHR, gGipa(VK_NULL_HANDLE, "vkGetSwapchainImagesKHR"));
    setIf(vkInvalidateMappedMemoryRanges, gGipa(VK_NULL_HANDLE, "vkInvalidateMappedMemoryRanges"));
    setIf(vkMapMemory, gGipa(VK_NULL_HANDLE, "vkMapMemory"));
    setIf(vkQueuePresentKHR, gGipa(VK_NULL_HANDLE, "vkQueuePresentKHR"));
    setIf(vkQueueSubmit, gGipa(VK_NULL_HANDLE, "vkQueueSubmit"));
    setIf(vkQueueWaitIdle, gGipa(VK_NULL_HANDLE, "vkQueueWaitIdle"));
    setIf(vkResetCommandBuffer, gGipa(VK_NULL_HANDLE, "vkResetCommandBuffer"));
    setIf(vkResetDescriptorPool, gGipa(VK_NULL_HANDLE, "vkResetDescriptorPool"));
    setIf(vkResetFences, gGipa(VK_NULL_HANDLE, "vkResetFences"));
    setIf(vkUnmapMemory, gGipa(VK_NULL_HANDLE, "vkUnmapMemory"));
    setIf(vkUpdateDescriptorSets, gGipa(VK_NULL_HANDLE, "vkUpdateDescriptorSets"));
    setIf(vkWaitForFences, gGipa(VK_NULL_HANDLE, "vkWaitForFences"));

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
