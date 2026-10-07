#pragma once
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>

#include <cstdint>
#include <mutex>
#include <string>

#include "rawr/vk/GpuContext.h"
#include "vulkan/QueueSubmission.h"

namespace rawrcam::vulkan {

// Coarse GPU family, from VkPhysicalDeviceProperties::vendorID. Used to pick vendor
// qualified fast paths; anything unrecognised must take the portable reference path.
enum class GpuVendor { Other, Adreno, Mali };


class VulkanContext {
   public:
    VulkanContext() = default;
    ~VulkanContext();
    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    void createInstance();
    void createDeviceForSurface(VkSurfaceKHR surface);
    void destroyDevice();
    void destroyInstance();
    void shutdown();
    // Serializes a whole-device wait with every actual queue. Call without queue locks held.
    VkResult waitIdle() const;

    VkInstance instance() const noexcept { return instance_; }
    VkPhysicalDevice physicalDevice() const noexcept { return physical_; }
    VkDevice device() const noexcept { return device_; }
    VkQueue queue() const noexcept { return queue_; }
    // Borrowed handles for native libraries (rawr::vk).
    rawr::vk::GpuContext gpuContext() const noexcept { return {physical_, device_, queueFamily_}; }
    // Background multiframe queue: second queue of the same family when the
    // driver exposes one (Adreno fam0 has 4), otherwise aliases queue().
    // Same family => no queue-ownership transfers; preview keeps queue 0 so
    // its submits no longer sit behind whole multiframe chunks in one FIFO.
    VkQueue multiframeQueue() const noexcept { return hasMultiframeQueue_ ? mfQueue_ : queue_; }
    bool hasMultiframeQueue() const noexcept { return hasMultiframeQueue_; }
    // Submit mutex for the multiframe queue, shared by every user of that
    // queue (merge worker, multiframe + single-frame still chains) so
    // vkQueueSubmit on it is externally synchronized. Mutable so const
    // contexts (still coordinators hold const refs) can submit through it.
    std::mutex& mfSubmitMutex() const noexcept {
        return hasMultiframeQueue_ ? multiframeSubmission_.mutex() : primarySubmission_.mutex();
    }
    std::mutex& stillProcessingMutex() const noexcept { return stillProcessingMutex_; }
    // Device-lifetime pipeline cache for still engines that are rebuilt per
    // capture (demosaicers): later builds reuse compiled pipelines instead of
    // recompiling every shader. Created on first use; VK_NULL_HANDLE if the
    // driver refuses (callers then compile uncached, as before). Pipeline
    // caches are internally synchronized, so concurrent builds may share it.
    VkPipelineCache pipelineCache() const;
    bool multiframeQueueLowPriority() const noexcept { return mfLowPriority_; }
    uint32_t queueFamily() const noexcept { return queueFamily_; }
    float timestampPeriod() const noexcept { return timestampPeriod_; }
    const std::string& gpuName() const noexcept { return gpuName_; }
    GpuVendor gpuVendor() const noexcept { return gpuVendor_; }
    uint32_t vendorId() const noexcept { return vendorId_; }
    uint32_t deviceId() const noexcept { return deviceId_; }
    const std::string& driverName() const noexcept { return driverName_; }
    const std::string& driverInfo() const noexcept { return driverInfo_; }
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID getAhbProperties() const noexcept { return getAhbProps_; }
    PFN_vkImportSemaphoreFdKHR importSemaphoreFd() const noexcept { return importSemaphoreFd_; }
    // Diagnostic-only: external-memory features reported for the
    // ANDROID_HARDWARE_BUFFER handle type on buffers (0 if unqueried).
    // Used to gate a byte-addressed buffer-view read path that bypasses
    // sampler pitch granularity.
    uint32_t ahbExternalBufferFeatures() const noexcept { return ahbExternalBufferFeatures_; }
    // Optional: f16 SSBO storage + float16 arithmetic (core Vulkan 1.2, or
    // KHR_shader_float16_int8 on 1.1 drivers). Enabled only when the driver
    // exposes both; required by the GALOSH compute stages, which stay off
    // otherwise (profiled wavelet serves as fallback).
    bool float16ComputeEnabled() const noexcept { return float16ComputeEnabled_; }
    // Storage images in formats such as rgb10_a2 (the packed 10-bit encoder Surface).
    bool storageImageExtendedFormats() const noexcept { return storageImageExtendedFormats_; }
    // VK_GOOGLE_display_timing: Android maps desiredPresentTime to the buffer
    // timestamp, which the video encoder uses as the frame's PTS.
    bool displayTimingEnabled() const noexcept { return displayTimingEnabled_; }

    const QueueSubmission& primaryQueue() const noexcept { return primarySubmission_; }
    const QueueSubmission& multiframeSubmission() const noexcept {
        return hasMultiframeQueue_ ? multiframeSubmission_ : primarySubmission_;
    }

   private:
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkQueue mfQueue_ = VK_NULL_HANDLE;
    bool hasMultiframeQueue_ = false;
    QueueSubmission primarySubmission_;
    QueueSubmission multiframeSubmission_;
    mutable std::mutex stillProcessingMutex_;
    bool mfLowPriority_ = false;
    uint32_t familyQueueCount_ = 0;
    uint32_t queueFamily_ = 0;
    float timestampPeriod_ = 1.0f;
    std::string gpuName_;
    GpuVendor gpuVendor_ = GpuVendor::Other;
    uint32_t vendorId_ = 0;
    uint32_t deviceId_ = 0;
    std::string driverName_;
    std::string driverInfo_;
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID getAhbProps_ = nullptr;
    PFN_vkImportSemaphoreFdKHR importSemaphoreFd_ = nullptr;
    uint32_t ahbExternalBufferFeatures_ = 0;
    bool drmFormatModifierEnabled_ = false;
    bool samplerYcbcrConversionEnabled_ = false;
    bool storageImageExtendedFormats_ = false;
    bool conditionalRenderingEnabled_ = false;
    bool displayTimingEnabled_ = false;
    bool float16ComputeEnabled_ = false;
};

}  // namespace rawrcam::vulkan
