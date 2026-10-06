// Encoder input A/B probe (debug tooling). Renders identical synthetic frames
// into the video encoder either through a Vulkan swapchain on the encoder
// Surface (A2B10G10R10, the shipped path) or into ImageWriter P010 buffers via
// VK_ANDROID_external_format_resolve. Self-contained on purpose: it needs a
// Vulkan 1.3 device with features the shared VulkanContext does not enable,
// so it loads libvulkan itself and never touches the camera pipeline.
#include <android/hardware_buffer_jni.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <dlfcn.h>
#include <jni.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "encoder_ab_pattern.h"
#include "present_vert.h"

namespace {
constexpr const char* kTag = "RawrEncoderAb";

void check(VkResult result, const char* what) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(what) + " VkResult=" + std::to_string(result));
}

#define ENCODER_AB_INSTANCE_FUNCS(X)          \
    X(EnumeratePhysicalDevices)               \
    X(GetPhysicalDeviceProperties)            \
    X(GetPhysicalDeviceProperties2)           \
    X(GetPhysicalDeviceQueueFamilyProperties) \
    X(GetPhysicalDeviceMemoryProperties)      \
    X(CreateDevice)                           \
    X(GetDeviceProcAddr)                      \
    X(DestroyInstance)                        \
    X(CreateAndroidSurfaceKHR)                \
    X(DestroySurfaceKHR)                      \
    X(GetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(GetPhysicalDeviceSurfaceFormatsKHR)

#define ENCODER_AB_DEVICE_FUNCS(X)              \
    X(GetDeviceQueue)                           \
    X(DestroyDevice)                            \
    X(DeviceWaitIdle)                           \
    X(CreateSwapchainKHR)                       \
    X(DestroySwapchainKHR)                      \
    X(GetSwapchainImagesKHR)                    \
    X(AcquireNextImageKHR)                      \
    X(QueuePresentKHR)                          \
    X(CreateImage)                              \
    X(DestroyImage)                             \
    X(CreateImageView)                          \
    X(DestroyImageView)                         \
    X(GetAndroidHardwareBufferPropertiesANDROID) \
    X(AllocateMemory)                           \
    X(FreeMemory)                               \
    X(BindImageMemory)                          \
    X(CreateSamplerYcbcrConversion)             \
    X(DestroySamplerYcbcrConversion)            \
    X(CreateShaderModule)                       \
    X(DestroyShaderModule)                      \
    X(CreatePipelineLayout)                     \
    X(DestroyPipelineLayout)                    \
    X(CreateGraphicsPipelines)                  \
    X(DestroyPipeline)                          \
    X(CreateCommandPool)                        \
    X(DestroyCommandPool)                       \
    X(AllocateCommandBuffers)                   \
    X(ResetCommandBuffer)                       \
    X(BeginCommandBuffer)                       \
    X(EndCommandBuffer)                         \
    X(CmdPipelineBarrier)                       \
    X(CmdBeginRendering)                        \
    X(CmdEndRendering)                          \
    X(CmdBindPipeline)                          \
    X(CmdPushConstants)                         \
    X(CmdDraw)                                  \
    X(CmdSetViewport)                           \
    X(CmdSetScissor)                            \
    X(CmdResetQueryPool)                        \
    X(CmdWriteTimestamp)                        \
    X(CreateQueryPool)                          \
    X(DestroyQueryPool)                         \
    X(GetQueryPoolResults)                      \
    X(QueueSubmit)                              \
    X(CreateFence)                              \
    X(DestroyFence)                             \
    X(WaitForFences)                            \
    X(ResetFences)                              \
    X(CreateSemaphore)                          \
    X(DestroySemaphore)

struct Fn {
#define DECLARE(name) PFN_vk##name name = nullptr;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
    PFN_vkCreateInstance CreateInstance = nullptr;
    ENCODER_AB_INSTANCE_FUNCS(DECLARE)
    ENCODER_AB_DEVICE_FUNCS(DECLARE)
#undef DECLARE
};

struct PatternPush {
    uint32_t frame;
    float width;
    float height;
};

// Imported encoder buffer, cached per AHardwareBuffer id (ImageWriter recycles them).
struct ImportedBuffer {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImage colorImage = VK_NULL_HANDLE;  // only when the driver needs a color attachment
    VkDeviceMemory colorMemory = VK_NULL_HANDLE;
    VkImageView colorView = VK_NULL_HANDLE;
};

class Probe {
   public:
    Probe(JNIEnv* env, jobject surface, uint32_t width, uint32_t height, bool p010)
        : width_(width), height_(height), p010_(p010) {
        loadInstance();
        createDevice();
        createCommon();
        if (!p010_) createSwapchain(env, surface);
    }
    ~Probe() { destroy(); }

    // Returns GPU time in microseconds (>= 1), 0 when no encoder image was
    // free within the app's 3 ms wait, negative on error.
    int64_t renderSwapchain(uint32_t frame, int64_t ptsNs) {
        uint32_t index = 0;
        const VkSemaphore acquired = acquireSemaphores_[acquireCursor_];
        const VkResult result = fn_.AcquireNextImageKHR(device_, swapchain_, 3'000'000, acquired, VK_NULL_HANDLE,
                                                        &index);
        if (result == VK_TIMEOUT || result == VK_NOT_READY) return 0;
        check(result, "acquire");
        acquireCursor_ = (acquireCursor_ + 1) % acquireSemaphores_.size();
        beginFrame();
        transition(swapImages_[index], swapInitialized_[index] ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                                                               : VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
        swapInitialized_[index] = true;
        VkRenderingAttachmentInfo attachment{};
        attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        attachment.imageView = swapViews_[index];
        attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        draw(attachment, swapPipeline_, frame);
        transition(swapImages_[index], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                   VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
        submit(acquired, renderedSemaphores_[index]);
        VkPresentTimeGOOGLE time{0, static_cast<uint64_t>(ptsNs)};
        VkPresentTimesInfoGOOGLE times{};
        times.sType = VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE;
        times.swapchainCount = 1;
        times.pTimes = &time;
        VkPresentInfoKHR present{};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.pNext = &times;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &renderedSemaphores_[index];
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain_;
        present.pImageIndices = &index;
        check(fn_.QueuePresentKHR(queue_, &present), "present");
        return finishFrame();
    }

    // Renders into one ImageWriter P010 buffer and waits for the GPU, so the
    // caller can queue the image right away.
    int64_t renderBuffer(AHardwareBuffer* buffer, uint32_t frame) {
        ImportedBuffer& imported = import(buffer);
        beginFrame();
        transition(imported.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_QUEUE_FAMILY_FOREIGN_EXT, queueFamily_);
        if (imported.colorImage)
            transition(imported.colorImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                       VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
        VkRenderingAttachmentInfo attachment{};
        attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        attachment.imageView = imported.colorView;  // VK_NULL_HANDLE when the driver allows it
        attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.resolveMode = VK_RESOLVE_MODE_EXTERNAL_FORMAT_DOWNSAMPLE_ANDROID;
        attachment.resolveImageView = imported.view;
        attachment.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        draw(attachment, externalPipeline_, frame);
        transition(imported.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, queueFamily_,
                   VK_QUEUE_FAMILY_FOREIGN_EXT);
        submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
        return finishFrame();
    }

    std::string info() const { return info_; }

   private:
    void loadInstance() {
        library_ = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        if (!library_) throw std::runtime_error("libvulkan.so unavailable");
        fn_.GetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library_, "vkGetInstanceProcAddr"));
        fn_.CreateInstance =
            reinterpret_cast<PFN_vkCreateInstance>(fn_.GetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
        const char* extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "RawrEncoderAb";
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo create{};
        create.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        create.pApplicationInfo = &app;
        create.enabledExtensionCount = 2;
        create.ppEnabledExtensionNames = extensions;
        check(fn_.CreateInstance(&create, nullptr, &instance_), "instance");
#define LOAD_INSTANCE(name)                                                                              \
    fn_.name = reinterpret_cast<PFN_vk##name>(fn_.GetInstanceProcAddr(instance_, "vk" #name));           \
    if (!fn_.name) throw std::runtime_error("missing vk" #name);
        ENCODER_AB_INSTANCE_FUNCS(LOAD_INSTANCE)
#undef LOAD_INSTANCE
    }

    void createDevice() {
        uint32_t count = 1;
        VkResult enumerated = fn_.EnumeratePhysicalDevices(instance_, &count, &physical_);
        if ((enumerated != VK_SUCCESS && enumerated != VK_INCOMPLETE) || count == 0)
            throw std::runtime_error("no Vulkan device");
        uint32_t families = 0;
        fn_.GetPhysicalDeviceQueueFamilyProperties(physical_, &families, nullptr);
        std::vector<VkQueueFamilyProperties> props(families);
        fn_.GetPhysicalDeviceQueueFamilyProperties(physical_, &families, props.data());
        queueFamily_ = UINT32_MAX;
        for (uint32_t i = 0; i < families; ++i)
            if (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                queueFamily_ = i;
                break;
            }
        if (queueFamily_ == UINT32_MAX) throw std::runtime_error("no graphics queue");
        VkPhysicalDeviceProperties properties{};
        fn_.GetPhysicalDeviceProperties(physical_, &properties);
        timestampPeriodNs_ = properties.limits.timestampPeriod;

        VkPhysicalDeviceExternalFormatResolvePropertiesANDROID resolveProps{};
        resolveProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FORMAT_RESOLVE_PROPERTIES_ANDROID;
        VkPhysicalDeviceProperties2 props2{};
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props2.pNext = &resolveProps;
        fn_.GetPhysicalDeviceProperties2(physical_, &props2);
        nullColorAttachment_ = resolveProps.nullColorAttachmentWithExternalFormatResolve == VK_TRUE;

        std::vector<const char*> extensions{
            VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
            VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME, VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME};
        if (p010_) extensions.push_back(VK_ANDROID_EXTERNAL_FORMAT_RESOLVE_EXTENSION_NAME);
        VkPhysicalDeviceExternalFormatResolveFeaturesANDROID resolveFeatures{};
        resolveFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FORMAT_RESOLVE_FEATURES_ANDROID;
        resolveFeatures.externalFormatResolve = VK_TRUE;
        VkPhysicalDeviceVulkan13Features features13{};
        features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        features13.dynamicRendering = VK_TRUE;
        features13.pNext = p010_ ? &resolveFeatures : nullptr;
        VkPhysicalDeviceVulkan11Features features11{};
        features11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
        features11.samplerYcbcrConversion = VK_TRUE;
        features11.pNext = &features13;
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = queueFamily_;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        VkDeviceCreateInfo create{};
        create.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        create.pNext = &features11;
        create.queueCreateInfoCount = 1;
        create.pQueueCreateInfos = &queueInfo;
        create.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        create.ppEnabledExtensionNames = extensions.data();
        check(fn_.CreateDevice(physical_, &create, nullptr, &device_), "device");
#define LOAD_DEVICE(name)                                                                      \
    fn_.name = reinterpret_cast<PFN_vk##name>(fn_.GetDeviceProcAddr(device_, "vk" #name));     \
    if (!fn_.name) throw std::runtime_error("missing vk" #name);
        ENCODER_AB_DEVICE_FUNCS(LOAD_DEVICE)
#undef LOAD_DEVICE
        fn_.GetDeviceQueue(device_, queueFamily_, 0, &queue_);
    }

    void createCommon() {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamily_;
        check(fn_.CreateCommandPool(device_, &poolInfo, nullptr, &pool_), "command pool");
        VkCommandBufferAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc.commandPool = pool_;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        check(fn_.AllocateCommandBuffers(device_, &alloc, &command_), "command buffer");
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        check(fn_.CreateFence(device_, &fenceInfo, nullptr, &fence_), "fence");
        VkQueryPoolCreateInfo queryInfo{};
        queryInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        queryInfo.queryCount = 2;
        check(fn_.CreateQueryPool(device_, &queryInfo, nullptr, &queries_), "query pool");
        VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PatternPush)};
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &range;
        check(fn_.CreatePipelineLayout(device_, &layoutInfo, nullptr, &layout_), "pipeline layout");
        vertex_ = shader(present_vert_spv, present_vert_spv_size);
        fragment_ = shader(encoder_ab_pattern_spv, encoder_ab_pattern_spv_size);
    }

    VkShaderModule shader(const unsigned char* code, size_t size) {
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = size;
        info.pCode = reinterpret_cast<const uint32_t*>(code);
        VkShaderModule module = VK_NULL_HANDLE;
        check(fn_.CreateShaderModule(device_, &info, nullptr, &module), "shader module");
        return module;
    }

    // colorFormat UNDEFINED + externalFormat != 0 builds the resolve pipeline.
    VkPipeline pipeline(VkFormat colorFormat, uint64_t externalFormat) {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertex_;
        stages[0].pName = "main";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragment_;
        stages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo assembly{};
        assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{};
        viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{};
        raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{};
        blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.attachmentCount = 1;
        blend.pAttachments = &blendAttachment;
        const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{};
        dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamicStates;
        VkExternalFormatANDROID external{};
        external.sType = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID;
        external.externalFormat = externalFormat;
        VkPipelineRenderingCreateInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        rendering.pNext = externalFormat ? &external : nullptr;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &colorFormat;
        VkGraphicsPipelineCreateInfo create{};
        create.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        create.pNext = &rendering;
        create.stageCount = 2;
        create.pStages = stages;
        create.pVertexInputState = &vertexInput;
        create.pInputAssemblyState = &assembly;
        create.pViewportState = &viewport;
        create.pRasterizationState = &raster;
        create.pMultisampleState = &multisample;
        create.pColorBlendState = &blend;
        create.pDynamicState = &dynamic;
        create.layout = layout_;
        VkPipeline result = VK_NULL_HANDLE;
        check(fn_.CreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &create, nullptr, &result), "pipeline");
        return result;
    }

    void createSwapchain(JNIEnv* env, jobject surfaceObject) {
        window_ = ANativeWindow_fromSurface(env, surfaceObject);
        if (!window_) throw std::runtime_error("encoder window unavailable");
        VkAndroidSurfaceCreateInfoKHR surfaceInfo{};
        surfaceInfo.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
        surfaceInfo.window = window_;
        check(fn_.CreateAndroidSurfaceKHR(instance_, &surfaceInfo, nullptr, &surface_), "surface");
        VkSurfaceCapabilitiesKHR caps{};
        check(fn_.GetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps), "surface caps");
        uint32_t count = 0;
        check(fn_.GetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &count, nullptr), "formats");
        std::vector<VkSurfaceFormatKHR> formats(count);
        check(fn_.GetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &count, formats.data()), "formats");
        const VkSurfaceFormatKHR* selected = nullptr;
        for (const auto& format : formats)
            if (format.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32) selected = &format;
        if (!selected) throw std::runtime_error("encoder Surface does not offer A2B10G10R10");
        // Same image count the app ends up with: its request of 5 never
        // exceeds this Surface's minimum.
        VkSwapchainCreateInfoKHR swapInfo{};
        swapInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        swapInfo.surface = surface_;
        swapInfo.minImageCount = std::max(5u, caps.minImageCount);
        if (caps.maxImageCount) swapInfo.minImageCount = std::min(swapInfo.minImageCount, caps.maxImageCount);
        swapInfo.imageFormat = selected->format;
        swapInfo.imageColorSpace = selected->colorSpace;
        swapInfo.imageExtent = {width_, height_};
        swapInfo.imageArrayLayers = 1;
        swapInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        swapInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        swapInfo.preTransform = caps.currentTransform;
        swapInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        swapInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        swapInfo.clipped = VK_TRUE;
        check(fn_.CreateSwapchainKHR(device_, &swapInfo, nullptr, &swapchain_), "swapchain");
        check(fn_.GetSwapchainImagesKHR(device_, swapchain_, &count, nullptr), "swap images");
        swapImages_.resize(count);
        check(fn_.GetSwapchainImagesKHR(device_, swapchain_, &count, swapImages_.data()), "swap images");
        swapInitialized_.assign(count, false);
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        for (VkImage image : swapImages_) {
            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = image;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = selected->format;
            viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkImageView view = VK_NULL_HANDLE;
            check(fn_.CreateImageView(device_, &viewInfo, nullptr, &view), "swap view");
            swapViews_.push_back(view);
            VkSemaphore rendered = VK_NULL_HANDLE;
            check(fn_.CreateSemaphore(device_, &semaphoreInfo, nullptr, &rendered), "semaphore");
            renderedSemaphores_.push_back(rendered);
        }
        for (uint32_t i = 0; i <= count; ++i) {
            VkSemaphore acquire = VK_NULL_HANDLE;
            check(fn_.CreateSemaphore(device_, &semaphoreInfo, nullptr, &acquire), "semaphore");
            acquireSemaphores_.push_back(acquire);
        }
        swapPipeline_ = pipeline(selected->format, 0);
        info_ = "{\"mode\":\"rgb10a2\",\"encoderImages\":" + std::to_string(count) +
                ",\"surfaceMinImages\":" + std::to_string(caps.minImageCount) + "}";
    }

    ImportedBuffer& import(AHardwareBuffer* buffer) {
        uint64_t id = 0;
        AHardwareBuffer_getId(buffer, &id);
        auto found = imported_.find(id);
        if (found != imported_.end()) return found->second;

        VkAndroidHardwareBufferFormatResolvePropertiesANDROID resolveFormat{};
        resolveFormat.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_RESOLVE_PROPERTIES_ANDROID;
        VkAndroidHardwareBufferFormatPropertiesANDROID format{};
        format.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID;
        format.pNext = &resolveFormat;
        VkAndroidHardwareBufferPropertiesANDROID properties{};
        properties.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
        properties.pNext = &format;
        check(fn_.GetAndroidHardwareBufferPropertiesANDROID(device_, buffer, &properties), "AHB properties");
        if (format.externalFormat == 0) throw std::runtime_error("P010 buffer has no external format");

        if (!externalPipeline_) {
            externalFormat_ = format.externalFormat;
            colorAttachmentFormat_ = resolveFormat.colorAttachmentFormat;
            VkExternalFormatANDROID external{};
            external.sType = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID;
            external.externalFormat = format.externalFormat;
            VkSamplerYcbcrConversionCreateInfo conversion{};
            conversion.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO;
            conversion.pNext = &external;
            conversion.format = VK_FORMAT_UNDEFINED;
            // BT.709 limited range, matching the codec's color keys.
            conversion.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
            conversion.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;
            conversion.components = format.samplerYcbcrConversionComponents;
            conversion.xChromaOffset = format.suggestedXChromaOffset;
            conversion.yChromaOffset = format.suggestedYChromaOffset;
            conversion.chromaFilter = VK_FILTER_NEAREST;
            check(fn_.CreateSamplerYcbcrConversion(device_, &conversion, nullptr, &conversion_), "ycbcr conversion");
            externalPipeline_ = pipeline(VK_FORMAT_UNDEFINED, format.externalFormat);
            info_ = "{\"mode\":\"p010\",\"externalFormat\":" + std::to_string(format.externalFormat) +
                    ",\"formatFeatures\":" + std::to_string(format.formatFeatures) +
                    ",\"colorAttachmentFormat\":" + std::to_string(resolveFormat.colorAttachmentFormat) +
                    ",\"nullColorAttachment\":" + (nullColorAttachment_ ? "true" : "false") +
                    ",\"suggestedModel\":" + std::to_string(format.suggestedYcbcrModel) +
                    ",\"suggestedRange\":" + std::to_string(format.suggestedYcbcrRange) + "}";
        }

        ImportedBuffer result;
        VkExternalFormatANDROID external{};
        external.sType = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID;
        external.externalFormat = format.externalFormat;
        VkExternalMemoryImageCreateInfo externalMemory{};
        externalMemory.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        externalMemory.pNext = &external;
        externalMemory.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.pNext = &externalMemory;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_UNDEFINED;
        imageInfo.extent = {width_, height_, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        check(fn_.CreateImage(device_, &imageInfo, nullptr, &result.image), "external image");
        VkImportAndroidHardwareBufferInfoANDROID importInfo{};
        importInfo.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
        importInfo.buffer = buffer;
        VkMemoryDedicatedAllocateInfo dedicated{};
        dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        dedicated.pNext = &importInfo;
        dedicated.image = result.image;
        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.pNext = &dedicated;
        alloc.allocationSize = properties.allocationSize;
        alloc.memoryTypeIndex = static_cast<uint32_t>(__builtin_ctz(properties.memoryTypeBits));
        check(fn_.AllocateMemory(device_, &alloc, nullptr, &result.memory), "import memory");
        check(fn_.BindImageMemory(device_, result.image, result.memory, 0), "bind import");
        VkSamplerYcbcrConversionInfo conversionInfo{};
        conversionInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
        conversionInfo.conversion = conversion_;
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.pNext = &conversionInfo;
        viewInfo.image = result.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_UNDEFINED;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        check(fn_.CreateImageView(device_, &viewInfo, nullptr, &result.view), "external view");
        if (!nullColorAttachment_) createColorAttachment(result, properties.memoryTypeBits);
        return imported_.emplace(id, result).first->second;
    }

    // Drivers without nullColorAttachmentWithExternalFormatResolve render into
    // a regular image of colorAttachmentFormat that resolves into the buffer.
    void createColorAttachment(ImportedBuffer& target, uint32_t) {
        if (colorAttachmentFormat_ == VK_FORMAT_UNDEFINED)
            throw std::runtime_error("driver reports no colorAttachmentFormat for external resolve");
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = colorAttachmentFormat_;
        imageInfo.extent = {width_, height_, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(fn_.CreateImage(device_, &imageInfo, nullptr, &target.colorImage), "color image");
        VkMemoryRequirements requirements{};
        // Device-local memory: the first type the driver allows.
        auto getRequirements = reinterpret_cast<PFN_vkGetImageMemoryRequirements>(
            fn_.GetDeviceProcAddr(device_, "vkGetImageMemoryRequirements"));
        getRequirements(device_, target.colorImage, &requirements);
        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = requirements.size;
        alloc.memoryTypeIndex = static_cast<uint32_t>(__builtin_ctz(requirements.memoryTypeBits));
        check(fn_.AllocateMemory(device_, &alloc, nullptr, &target.colorMemory), "color memory");
        check(fn_.BindImageMemory(device_, target.colorImage, target.colorMemory, 0), "bind color");
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = target.colorImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = colorAttachmentFormat_;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        check(fn_.CreateImageView(device_, &viewInfo, nullptr, &target.colorView), "color view");
    }

    void beginFrame() {
        check(fn_.ResetCommandBuffer(command_, 0), "reset command");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(fn_.BeginCommandBuffer(command_, &begin), "begin command");
        fn_.CmdResetQueryPool(command_, queries_, 0, 2);
        fn_.CmdWriteTimestamp(command_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries_, 0);
    }

    void transition(VkImage image, VkImageLayout from, VkImageLayout to, uint32_t srcFamily, uint32_t dstFamily) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = from;
        barrier.newLayout = to;
        barrier.srcQueueFamilyIndex = srcFamily;
        barrier.dstQueueFamilyIndex = dstFamily;
        barrier.image = image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.srcAccessMask = from == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                                                                                  : 0;
        barrier.dstAccessMask = to == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                                                                                : 0;
        fn_.CmdPipelineBarrier(command_, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    void draw(const VkRenderingAttachmentInfo& attachment, VkPipeline pipeline, uint32_t frame) {
        VkRenderingInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering.renderArea = {{0, 0}, {width_, height_}};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &attachment;
        fn_.CmdBeginRendering(command_, &rendering);
        fn_.CmdBindPipeline(command_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        VkViewport viewport{0, 0, float(width_), float(height_), 0, 1};
        VkRect2D scissor{{0, 0}, {width_, height_}};
        fn_.CmdSetViewport(command_, 0, 1, &viewport);
        fn_.CmdSetScissor(command_, 0, 1, &scissor);
        PatternPush push{frame, float(width_), float(height_)};
        fn_.CmdPushConstants(command_, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        fn_.CmdDraw(command_, 3, 1, 0, 0);
        fn_.CmdEndRendering(command_);
    }

    void submit(VkSemaphore wait, VkSemaphore signal) {
        fn_.CmdWriteTimestamp(command_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_, 1);
        check(fn_.EndCommandBuffer(command_), "end command");
        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.waitSemaphoreCount = wait ? 1 : 0;
        submitInfo.pWaitSemaphores = &wait;
        submitInfo.pWaitDstStageMask = &waitStage;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &command_;
        submitInfo.signalSemaphoreCount = signal ? 1 : 0;
        submitInfo.pSignalSemaphores = &signal;
        check(fn_.ResetFences(device_, 1, &fence_), "reset fence");
        check(fn_.QueueSubmit(queue_, 1, &submitInfo, fence_), "submit");
    }

    // Both modes wait for the GPU here, so the producers stay symmetric.
    int64_t finishFrame() {
        check(fn_.WaitForFences(device_, 1, &fence_, VK_TRUE, 1'000'000'000ull), "wait fence");
        uint64_t stamps[2]{};
        if (fn_.GetQueryPoolResults(device_, queries_, 0, 2, sizeof(stamps), stamps, sizeof(uint64_t),
                                    VK_QUERY_RESULT_64_BIT) != VK_SUCCESS ||
            stamps[1] <= stamps[0])
            return 1;
        return std::max<int64_t>(1, int64_t(double(stamps[1] - stamps[0]) * timestampPeriodNs_ / 1000.0));
    }

    void destroy() noexcept {
        if (device_) {
            fn_.DeviceWaitIdle(device_);
            for (auto& [id, buffer] : imported_) {
                fn_.DestroyImageView(device_, buffer.view, nullptr);
                fn_.DestroyImage(device_, buffer.image, nullptr);
                fn_.FreeMemory(device_, buffer.memory, nullptr);
                if (buffer.colorView) fn_.DestroyImageView(device_, buffer.colorView, nullptr);
                if (buffer.colorImage) fn_.DestroyImage(device_, buffer.colorImage, nullptr);
                if (buffer.colorMemory) fn_.FreeMemory(device_, buffer.colorMemory, nullptr);
            }
            for (VkImageView view : swapViews_) fn_.DestroyImageView(device_, view, nullptr);
            for (VkSemaphore s : renderedSemaphores_) fn_.DestroySemaphore(device_, s, nullptr);
            for (VkSemaphore s : acquireSemaphores_) fn_.DestroySemaphore(device_, s, nullptr);
            if (swapchain_) fn_.DestroySwapchainKHR(device_, swapchain_, nullptr);
            if (conversion_) fn_.DestroySamplerYcbcrConversion(device_, conversion_, nullptr);
            if (swapPipeline_) fn_.DestroyPipeline(device_, swapPipeline_, nullptr);
            if (externalPipeline_) fn_.DestroyPipeline(device_, externalPipeline_, nullptr);
            if (vertex_) fn_.DestroyShaderModule(device_, vertex_, nullptr);
            if (fragment_) fn_.DestroyShaderModule(device_, fragment_, nullptr);
            if (layout_) fn_.DestroyPipelineLayout(device_, layout_, nullptr);
            if (queries_) fn_.DestroyQueryPool(device_, queries_, nullptr);
            if (fence_) fn_.DestroyFence(device_, fence_, nullptr);
            if (pool_) fn_.DestroyCommandPool(device_, pool_, nullptr);
            fn_.DestroyDevice(device_, nullptr);
        }
        if (surface_) fn_.DestroySurfaceKHR(instance_, surface_, nullptr);
        if (instance_) fn_.DestroyInstance(instance_, nullptr);
        if (window_) ANativeWindow_release(window_);
        if (library_) dlclose(library_);
    }

    uint32_t width_, height_;
    bool p010_;
    void* library_ = nullptr;
    Fn fn_{};
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    float timestampPeriodNs_ = 1.0f;
    bool nullColorAttachment_ = false;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkQueryPool queries_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkShaderModule vertex_ = VK_NULL_HANDLE, fragment_ = VK_NULL_HANDLE;
    ANativeWindow* window_ = nullptr;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    std::vector<VkImage> swapImages_;
    std::vector<VkImageView> swapViews_;
    std::vector<bool> swapInitialized_;
    std::vector<VkSemaphore> acquireSemaphores_, renderedSemaphores_;
    size_t acquireCursor_ = 0;
    VkPipeline swapPipeline_ = VK_NULL_HANDLE, externalPipeline_ = VK_NULL_HANDLE;
    uint64_t externalFormat_ = 0;
    VkFormat colorAttachmentFormat_ = VK_FORMAT_UNDEFINED;
    VkSamplerYcbcrConversion conversion_ = VK_NULL_HANDLE;
    std::map<uint64_t, ImportedBuffer> imported_;
    std::string info_ = "{}";
};

std::string lastError;
}  // namespace

extern "C" JNIEXPORT jlong JNICALL Java_com_rawr_camera_video_EncoderAbProbe_nativeCreate(
    JNIEnv* env, jobject, jobject surface, jint width, jint height, jboolean p010) {
    try {
        return reinterpret_cast<jlong>(
            new Probe(env, surface, static_cast<uint32_t>(width), static_cast<uint32_t>(height), p010 == JNI_TRUE));
    } catch (const std::exception& error) {
        lastError = error.what();
        __android_log_print(ANDROID_LOG_ERROR, kTag, "create: %s", error.what());
        return 0;
    }
}

extern "C" JNIEXPORT jlong JNICALL Java_com_rawr_camera_video_EncoderAbProbe_nativeRenderSwapchain(
    JNIEnv*, jobject, jlong handle, jint frame, jlong ptsNs) {
    try {
        return reinterpret_cast<Probe*>(handle)->renderSwapchain(static_cast<uint32_t>(frame), ptsNs);
    } catch (const std::exception& error) {
        lastError = error.what();
        return -1;
    }
}

extern "C" JNIEXPORT jlong JNICALL Java_com_rawr_camera_video_EncoderAbProbe_nativeRenderBuffer(
    JNIEnv* env, jobject, jlong handle, jobject hardwareBuffer, jint frame) {
    try {
        AHardwareBuffer* buffer = AHardwareBuffer_fromHardwareBuffer(env, hardwareBuffer);
        if (!buffer) throw std::runtime_error("null HardwareBuffer");
        return reinterpret_cast<Probe*>(handle)->renderBuffer(buffer, static_cast<uint32_t>(frame));
    } catch (const std::exception& error) {
        lastError = error.what();
        return -1;
    }
}

extern "C" JNIEXPORT jstring JNICALL Java_com_rawr_camera_video_EncoderAbProbe_nativeInfo(JNIEnv* env, jobject,
                                                                                         jlong handle) {
    return env->NewStringUTF(handle ? reinterpret_cast<Probe*>(handle)->info().c_str() : "{}");
}

extern "C" JNIEXPORT jstring JNICALL Java_com_rawr_camera_video_EncoderAbProbe_nativeLastError(JNIEnv* env,
                                                                                              jobject) {
    return env->NewStringUTF(lastError.c_str());
}

extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_video_EncoderAbProbe_nativeDestroy(JNIEnv*, jobject,
                                                                                        jlong handle) {
    delete reinterpret_cast<Probe*>(handle);
}
