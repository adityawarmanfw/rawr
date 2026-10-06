#include "video/VideoOutput.h"

#include <android/log.h>
#include <android/native_window_jni.h>
#include <vulkan/vulkan_android.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <string>
#include <ctime>

#include "vulkan/VulkanContext.h"

namespace rawrcam::video {
namespace {
void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string(what) + " VkResult=" + std::to_string(r));
}
}  // namespace
VideoOutput::~VideoOutput() { stop(); }
bool VideoOutput::start(JNIEnv* env, jobject javaSurface, uint32_t width, uint32_t height, uint32_t bitDepth) {
    stop();
    const auto startedAt = std::chrono::steady_clock::now();
    const auto sinceStartMs = [&] {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - startedAt).count();
    };
    if (!javaSurface || !context_.device() || width == 0 || height == 0 || (bitDepth != 8 && bitDepth != 10))
        return false;
    try {
        window_ = ANativeWindow_fromSurface(env, javaSurface);
        if (!window_) throw std::runtime_error("video ANativeWindow unavailable");
        VkAndroidSurfaceCreateInfoKHR surfaceInfo{};
        surfaceInfo.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
        surfaceInfo.window = window_;
        check(vkCreateAndroidSurfaceKHR(context_.instance(), &surfaceInfo, nullptr, &surface_), "video surface");
        VkBool32 supported = VK_FALSE;
        check(vkGetPhysicalDeviceSurfaceSupportKHR(context_.physicalDevice(), context_.queueFamily(), surface_,
                                                   &supported),
              "video queue support");
        if (!supported) throw std::runtime_error("video queue cannot present to encoder Surface");
        VkSurfaceCapabilitiesKHR caps{};
        check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(context_.physicalDevice(), surface_, &caps), "video caps");
        uint32_t count = 0;
        check(vkGetPhysicalDeviceSurfaceFormatsKHR(context_.physicalDevice(), surface_, &count, nullptr),
              "video formats count");
        std::vector<VkSurfaceFormatKHR> formats(count);
        check(vkGetPhysicalDeviceSurfaceFormatsKHR(context_.physicalDevice(), surface_, &count, formats.data()),
              "video formats");
        offeredFormats_.clear();
        for (const auto& format : formats)
            offeredFormats_ += (offeredFormats_.empty() ? "" : ",") + std::to_string(format.format) + ":" +
                               std::to_string(format.colorSpace);
        const auto find = [&](VkFormat wanted) {
            return std::find_if(formats.begin(), formats.end(),
                                [wanted](const VkSurfaceFormatKHR& format) { return format.format == wanted; });
        };
        // Prefer the 4-byte formats: the encoder converts RGBA16F with extra
        // work that competes with recording at 4K and Open Gate.
        auto selected = bitDepth == 8                            ? find(VK_FORMAT_R8G8B8A8_UNORM)
                        : context_.storageImageExtendedFormats() ? find(VK_FORMAT_A2B10G10R10_UNORM_PACK32)
                                                                 : formats.end();
        VkFormatProperties packedProperties{};
        if (selected != formats.end()) {
            vkGetPhysicalDeviceFormatProperties(context_.physicalDevice(), selected->format, &packedProperties);
            if ((packedProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) == 0)
                selected = formats.end();
        }
        if (selected == formats.end()) selected = find(VK_FORMAT_R16G16B16A16_SFLOAT);
        if (selected == formats.end() || (caps.supportedUsageFlags & VK_IMAGE_USAGE_STORAGE_BIT) == 0)
            throw std::runtime_error("encoder Surface does not support RGBA16F storage");
        outputFormat_ = selected->format;
        extent_ = caps.currentExtent.width == UINT32_MAX ? VkExtent2D{width, height} : caps.currentExtent;
        if (extent_.width != width || extent_.height != height)
            throw std::runtime_error("encoder Surface extent differs from codec configuration");
        VkSwapchainCreateInfoKHR swapInfo{};
        swapInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        swapInfo.surface = surface_;
        if (caps.maxImageCount && std::max(3u, caps.minImageCount) > caps.maxImageCount)
            throw std::runtime_error("encoder Surface has too few swapchain images");
        swapInfo.imageFormat = selected->format;
        swapInfo.imageColorSpace = selected->colorSpace;
        swapInfo.imageExtent = extent_;
        swapInfo.imageArrayLayers = 1;
        swapInfo.imageUsage = VK_IMAGE_USAGE_STORAGE_BIT;
        swapInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        swapInfo.preTransform = caps.currentTransform;
        swapInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        swapInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        swapInfo.clipped = VK_TRUE;
        // Ask for spare images so the encoder can absorb a brief stall. On the
        // test phone the Surface's own minimum (11-18) is already higher; the
        // request matters where it is lower. Fall back if it is refused.
        VkResult created = VK_ERROR_INITIALIZATION_FAILED;
        for (uint32_t wanted : {kPreferredEncoderImages, 4u, 3u}) {
            uint32_t images = std::max(wanted, caps.minImageCount);
            if (caps.maxImageCount) images = std::min(images, caps.maxImageCount);
            swapInfo.minImageCount = images;
            created = vkCreateSwapchainKHR(context_.device(), &swapInfo, nullptr, &swapchain_);
            if (created == VK_SUCCESS) break;
            swapchain_ = VK_NULL_HANDLE;
        }
        check(created, "video swapchain");
        count = 0;
        check(vkGetSwapchainImagesKHR(context_.device(), swapchain_, &count, nullptr), "video images count");
        images_.resize(count);
        check(vkGetSwapchainImagesKHR(context_.device(), swapchain_, &count, images_.data()), "video images");
        views_.resize(count);
        initialized_.assign(count, false);
        for (uint32_t i = 0; i < count; ++i) {
            VkImageViewCreateInfo view{};
            view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view.image = images_[i];
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = outputFormat_;
            view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            check(vkCreateImageView(context_.device(), &view, nullptr, &views_[i]), "video image view");
        }
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        for (uint32_t i = 0; i < available_.size(); ++i) {
            check(vkCreateSemaphore(context_.device(), &semaphoreInfo, nullptr, &available_[i]),
                  "video available semaphore");
            check(vkCreateSemaphore(context_.device(), &semaphoreInfo, nullptr, &rendered_[i]),
                  "video rendered semaphore");
        }
        swapchainMs_ = sinceStartMs();
        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        startedAtNs_ = int64_t(now.tv_sec) * 1'000'000'000 + now.tv_nsec;
        return true;
    } catch (...) {
        stop();
        throw;
    }
}
VkFormat VideoOutput::preferredFormat(uint32_t bitDepth) const {
    const VkFormat format = bitDepth == 8                            ? VK_FORMAT_R8G8B8A8_UNORM
                            : context_.storageImageExtendedFormats() ? VK_FORMAT_A2B10G10R10_UNORM_PACK32
                                                                     : VK_FORMAT_R16G16B16A16_SFLOAT;
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(context_.physicalDevice(), format, &properties);
    return (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0
               ? format
               : VK_FORMAT_R16G16B16A16_SFLOAT;
}
void VideoOutput::stop() noexcept {
    const VkDevice device = context_.device();
    if (device) {
        if (swapchain_) (void)context_.waitIdle();
        for (auto& view : views_)
            if (view) vkDestroyImageView(device, view, nullptr);
        for (auto& semaphore : available_)
            if (semaphore) vkDestroySemaphore(device, semaphore, nullptr);
        for (auto& semaphore : rendered_)
            if (semaphore) vkDestroySemaphore(device, semaphore, nullptr);
        if (swapchain_) vkDestroySwapchainKHR(device, swapchain_, nullptr);
    }
    views_.clear();
    images_.clear();
    initialized_.clear();
    available_.fill(VK_NULL_HANDLE);
    rendered_.fill(VK_NULL_HANDLE);
    swapchain_ = VK_NULL_HANDLE;
    if (surface_ && context_.instance()) vkDestroySurfaceKHR(context_.instance(), surface_, nullptr);
    surface_ = VK_NULL_HANDLE;
    if (window_) ANativeWindow_release(window_);
    window_ = nullptr;
    extent_ = {};
    startedAtNs_ = 0;
}
bool VideoOutput::acquire(uint32_t frameSlot, uint32_t* imageIndex) {
    if (!ready() || frameSlot >= available_.size()) return false;
    const VkResult result = vkAcquireNextImageKHR(context_.device(), swapchain_, kEncoderAcquireWaitNs,
                                                  available_[frameSlot], VK_NULL_HANDLE, imageIndex);
    if (result == VK_NOT_READY || result == VK_TIMEOUT) return false;
    check(result, "video acquire");
    return true;
}
bool VideoOutput::stampsPresentTime() const noexcept { return context_.displayTimingEnabled(); }
VkResult VideoOutput::present(VkQueue queue, uint32_t frameSlot, uint32_t imageIndex, uint64_t presentTimeNs) {
    VkPresentTimeGOOGLE time{};
    time.presentID = 0;
    time.desiredPresentTime = presentTimeNs;
    VkPresentTimesInfoGOOGLE times{};
    times.sType = VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE;
    times.swapchainCount = 1;
    times.pTimes = &time;
    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    if (presentTimeNs != 0 && stampsPresentTime()) present.pNext = &times;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &rendered_[frameSlot];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &imageIndex;
    return vkQueuePresentKHR(queue, &present);
}
}  // namespace rawrcam::video
