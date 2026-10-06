#include "VulkanContext.h"

#include <cstring>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "VulkanDispatch.h"

namespace rawrcam::vulkan {
namespace {

void vkCheck(VkResult result, const char* what) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(what) + " VkResult=" + std::to_string(result));
    }
}

bool hasExtension(const std::vector<VkExtensionProperties>& properties, const char* name) {
    for (const auto& property : properties) {
        if (std::strcmp(property.extensionName, name) == 0) return true;
    }
    return false;
}

}  // namespace

VulkanContext::~VulkanContext() { shutdown(); }

void VulkanContext::createInstance() {
    if (instance_ != VK_NULL_HANDLE) return;
    const char* extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "RawrCam";
    appInfo.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = 2;
    createInfo.ppEnabledExtensionNames = extensions;
    vkCheck(vkCreateInstance(&createInfo, nullptr, &instance_), "vkCreateInstance");
    dispatch::loadInstance(instance_);
}

void VulkanContext::createDeviceForSurface(VkSurfaceKHR surface) {
    if (device_ != VK_NULL_HANDLE) return;
    if (instance_ == VK_NULL_HANDLE) {
        throw std::runtime_error("Vulkan instance/surface not ready");
    }

    uint32_t deviceCount = 0;
    vkCheck(vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr), "vkEnumeratePhysicalDevices count");
    if (!deviceCount) throw std::runtime_error("No Vulkan physical device");
    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkCheck(vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data()), "vkEnumeratePhysicalDevices");

    for (VkPhysicalDevice candidate : devices) {
        uint32_t queueCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, nullptr);
        std::vector<VkQueueFamilyProperties> queues(queueCount);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, queues.data());
        for (uint32_t i = 0; i < queueCount; ++i) {
            VkBool32 present = surface == VK_NULL_HANDLE ? VK_TRUE : VK_FALSE;
            if (surface != VK_NULL_HANDLE) vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface, &present);
            const auto required = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
            if ((queues[i].queueFlags & required) == required && present) {
                physical_ = candidate;
                queueFamily_ = i;
                familyQueueCount_ = queues[i].queueCount;
                break;
            }
        }
        if (physical_ != VK_NULL_HANDLE) break;
    }
    if (physical_ == VK_NULL_HANDLE) throw std::runtime_error("No graphics+compute+present queue family");

    uint32_t extensionCount = 0;
    vkEnumerateDeviceExtensionProperties(physical_, nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> extensions(extensionCount);
    vkEnumerateDeviceExtensionProperties(physical_, nullptr, &extensionCount, extensions.data());
    const char* requiredExtensions[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
        VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
    };
    for (const char* extension : requiredExtensions) {
        if (!hasExtension(extensions, extension)) {
            throw std::runtime_error(std::string("Missing Vulkan device extension ") + extension);
        }
    }

    drmFormatModifierEnabled_ = hasExtension(extensions, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
    conditionalRenderingEnabled_ = hasExtension(extensions, VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME);
    displayTimingEnabled_ = hasExtension(extensions, VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);

    // GALOSH f16 probe (spec: querying structs from unsupported extensions
    // returns zero, so this is safe on 1.1-only drivers).
    VkPhysicalDevice16BitStorageFeatures storage16Query{};
    storage16Query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES;
    VkPhysicalDeviceShaderFloat16Int8Features float16Query{};
    float16Query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES;
    float16Query.pNext = &storage16Query;
    VkPhysicalDeviceFeatures2 float16Probe{};
    float16Probe.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    float16Probe.pNext = &float16Query;
    vkGetPhysicalDeviceFeatures2(physical_, &float16Probe);
    float16ComputeEnabled_ =
        storage16Query.storageBuffer16BitAccess == VK_TRUE && float16Query.shaderFloat16 == VK_TRUE;

    {
        // Diagnostic-only probe for a byte-addressed buffer-view read path:
        // whether this driver allows importing an AHB as buffer memory
        // (bypasses sampler pitch granularity for odd camera strides).
        // Resolve via the instance (not the NULL-instance cache): some
        // loaders only expose physical-device entry points per-instance.
        const auto getBufferProps = reinterpret_cast<PFN_vkGetPhysicalDeviceExternalBufferProperties>(
            vkGetInstanceProcAddr(instance_, "vkGetPhysicalDeviceExternalBufferProperties"));
        if (getBufferProps == nullptr && vkGetPhysicalDeviceExternalBufferProperties != nullptr)
            ahbExternalBufferFeatures_ = 0xdead0000u;  // marker: fallback pointer exists
        if (getBufferProps != nullptr) {
            VkPhysicalDeviceExternalBufferInfo bufferInfo{};
            bufferInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO;
            bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            bufferInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
            VkExternalBufferProperties bufferProps{};
            bufferProps.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES;
            getBufferProps(physical_, &bufferInfo, &bufferProps);
            ahbExternalBufferFeatures_ = bufferProps.externalMemoryProperties.externalMemoryFeatures;
        }
    }

    VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcrSupported{};
    ycbcrSupported.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES;
    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &ycbcrSupported;
    vkGetPhysicalDeviceFeatures2(physical_, &features2);
    samplerYcbcrConversionEnabled_ = ycbcrSupported.samplerYcbcrConversion == VK_TRUE;
    storageImageExtendedFormats_ = features2.features.shaderStorageImageExtendedFormats == VK_TRUE;

    float priorities[2] = {1.0f, 1.0f};
    const uint32_t wantedQueues = familyQueueCount_ >= 2 ? 2u : 1u;
    std::vector<const char*> enabledExtensions(std::begin(requiredExtensions), std::end(requiredExtensions));
    if (float16ComputeEnabled_) {
        // Core Vulkan 1.2 carries the f16 structs; on 1.1 drivers they ride
        // the KHR_shader_float16_int8 extension, which must then be enabled.
        VkPhysicalDeviceProperties versionProps{};
        vkGetPhysicalDeviceProperties(physical_, &versionProps);
        if (versionProps.apiVersion < VK_API_VERSION_1_2 &&
            hasExtension(extensions, VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME)) {
            enabledExtensions.push_back(VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME);
        }
    }
    if (drmFormatModifierEnabled_) enabledExtensions.push_back(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
    if (conditionalRenderingEnabled_) enabledExtensions.push_back(VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME);
    if (displayTimingEnabled_) enabledExtensions.push_back(VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);
    // NOTE: LOW global priority for the merge queue (two same-family create
    // infos) froze the viewfinder on Adreno 840 -- likely a driver scheduler
    // issue with cross-queue semaphore + priority. Both queues stay MEDIUM;
    // queue separation alone still removes the single-FIFO stalls.

    VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcrEnable{};
    ycbcrEnable.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES;
    ycbcrEnable.samplerYcbcrConversion = samplerYcbcrConversionEnabled_ ? VK_TRUE : VK_FALSE;

    VkPhysicalDevice16BitStorageFeatures storage16Enable{};
    storage16Enable.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES;
    storage16Enable.storageBuffer16BitAccess = VK_TRUE;
    VkPhysicalDeviceShaderFloat16Int8Features float16Enable{};
    float16Enable.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES;
    float16Enable.pNext = &storage16Enable;
    float16Enable.shaderFloat16 = VK_TRUE;
    ycbcrEnable.pNext = float16ComputeEnabled_ ? static_cast<void*>(&float16Enable) : nullptr;

    VkPhysicalDeviceConditionalRenderingFeaturesEXT conditionalEnable{};
    conditionalEnable.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT;
    conditionalEnable.pNext = samplerYcbcrConversionEnabled_
                                  ? static_cast<void*>(&ycbcrEnable)
                                  : (float16ComputeEnabled_ ? static_cast<void*>(&float16Enable) : nullptr);
    conditionalEnable.conditionalRendering = conditionalRenderingEnabled_ ? VK_TRUE : VK_FALSE;

    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = queueFamily_;
    queueInfo.queueCount = wantedQueues;
    queueInfo.pQueuePriorities = priorities;

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.pNext = conditionalRenderingEnabled_
                           ? static_cast<void*>(&conditionalEnable)
                           : (samplerYcbcrConversionEnabled_
                                  ? static_cast<void*>(&ycbcrEnable)
                                  : (float16ComputeEnabled_ ? static_cast<void*>(&float16Enable) : nullptr));
    VkPhysicalDeviceFeatures coreFeatures{};
    coreFeatures.shaderStorageImageExtendedFormats = storageImageExtendedFormats_ ? VK_TRUE : VK_FALSE;
    createInfo.pEnabledFeatures = &coreFeatures;
    createInfo.queueCreateInfoCount = 1;
    createInfo.pQueueCreateInfos = &queueInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
    createInfo.ppEnabledExtensionNames = enabledExtensions.data();
    uint32_t createdQueues = 1;
    vkCheck(vkCreateDevice(physical_, &createInfo, nullptr, &device_), "vkCreateDevice");
    createdQueues = wantedQueues;
    dispatch::loadDevice(device_);
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
    primarySubmission_.bind(queue_);
    if (createdQueues >= 2) {
        vkGetDeviceQueue(device_, queueFamily_, 1, &mfQueue_);
        multiframeSubmission_.bind(mfQueue_);
        hasMultiframeQueue_ = mfQueue_ != VK_NULL_HANDLE;
    }
    mfLowPriority_ = false;

    getAhbProps_ = reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(
        vkGetDeviceProcAddr(device_, "vkGetAndroidHardwareBufferPropertiesANDROID"));
    importSemaphoreFd_ =
        reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(vkGetDeviceProcAddr(device_, "vkImportSemaphoreFdKHR"));
    if (!getAhbProps_ || !importSemaphoreFd_) {
        throw std::runtime_error("Required Vulkan extension functions unavailable");
    }

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical_, &properties);
    gpuName_ = properties.deviceName;
    timestampPeriod_ = properties.limits.timestampPeriod;

    driverName_.clear();
    driverInfo_.clear();
    if (hasExtension(extensions, VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME) && vkGetPhysicalDeviceProperties2) {
        VkPhysicalDeviceDriverProperties driverProperties{};
        driverProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
        VkPhysicalDeviceProperties2 properties2{};
        properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        properties2.pNext = &driverProperties;
        vkGetPhysicalDeviceProperties2(physical_, &properties2);
        driverName_ = driverProperties.driverName;
        driverInfo_ = driverProperties.driverInfo;
    }

    VkFormatProperties formatProperties{};
    vkGetPhysicalDeviceFormatProperties(physical_, VK_FORMAT_R16_UINT, &formatProperties);
    if (!(formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
        throw std::runtime_error("VK_FORMAT_R16_UINT lacks STORAGE_IMAGE support");
    }
}

VkPipelineCache VulkanContext::pipelineCache() const { return dispatch::pipelineCache(device_); }

VkResult VulkanContext::waitIdle() const {
    if (!device_) return VK_SUCCESS;
    if (hasMultiframeQueue_) {
        std::scoped_lock lock(primarySubmission_.mutex(), multiframeSubmission_.mutex());
        return vkDeviceWaitIdle(device_);
    }
    std::lock_guard<std::mutex> lock(primarySubmission_.mutex());
    return vkDeviceWaitIdle(device_);
}

void VulkanContext::destroyDevice() {
    dispatch::releasePipelineCache(device_);
    if (device_ != VK_NULL_HANDLE) vkDestroyDevice(device_, nullptr);
    device_ = VK_NULL_HANDLE;
    primarySubmission_.bind(VK_NULL_HANDLE);
    multiframeSubmission_.bind(VK_NULL_HANDLE);
    queue_ = VK_NULL_HANDLE;
    mfQueue_ = VK_NULL_HANDLE;
    hasMultiframeQueue_ = false;
    mfLowPriority_ = false;
    physical_ = VK_NULL_HANDLE;
    queueFamily_ = 0;
    familyQueueCount_ = 0;
    timestampPeriod_ = 1.0f;
    gpuName_.clear();
    driverName_.clear();
    driverInfo_.clear();
    getAhbProps_ = nullptr;
    importSemaphoreFd_ = nullptr;
    drmFormatModifierEnabled_ = false;
    samplerYcbcrConversionEnabled_ = false;
    conditionalRenderingEnabled_ = false;
}

void VulkanContext::destroyInstance() {
    if (instance_ != VK_NULL_HANDLE) vkDestroyInstance(instance_, nullptr);
    instance_ = VK_NULL_HANDLE;
}

void VulkanContext::shutdown() {
    destroyDevice();
    destroyInstance();
}

}  // namespace rawrcam::vulkan
