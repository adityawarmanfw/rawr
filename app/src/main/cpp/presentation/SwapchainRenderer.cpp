#include "presentation/SwapchainRenderer.h"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

#include "present_frag.h"
#include "present_vert.h"

namespace rawrcam::presentation {
namespace {

void vkCheck(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " VkResult=" + std::to_string(result));
    }
}

}  // namespace

SwapchainRenderer::SwapchainRenderer(Diagnostic diagnostic)
    : diagnostic_(std::move(diagnostic)), recorder_(diagnostic_) {}

SwapchainRenderer::~SwapchainRenderer() { destroySwapchain(); }

void SwapchainRenderer::initialize(const vulkan::VulkanContext& context) {
    if (swapchain_ != VK_NULL_HANDLE) {
        throw std::logic_error("Cannot reinitialize SwapchainRenderer while a swapchain exists");
    }
    context_ = &context;
    physicalDevice_ = context.physicalDevice();
    device_ = context.device();
}

void SwapchainRenderer::createSwapchain(VkSurfaceKHR surface, ANativeWindow* window) {
    if (device_ == VK_NULL_HANDLE || physicalDevice_ == VK_NULL_HANDLE || surface == VK_NULL_HANDLE ||
        window == nullptr) {
        throw std::logic_error("SwapchainRenderer is missing Vulkan surface/device state");
    }

    vkCheck(context_->waitIdle(), "presentation vkDeviceWaitIdle");
    destroySwapchain();

    VkSurfaceCapabilitiesKHR caps{};
    vkCheck(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface, &caps), "presentation surface caps");

    uint32_t formatCount = 0;
    vkCheck(vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface, &formatCount, nullptr),
            "presentation surface format count");
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkCheck(vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface, &formatCount, formats.data()),
            "presentation surface formats");
    if (formats.empty()) throw std::runtime_error("No presentation surface formats");

    surfaceFormat_ = formats.front();
    for (const auto& format : formats) {
        if (format.format == VK_FORMAT_R8G8B8A8_UNORM || format.format == VK_FORMAT_B8G8R8A8_UNORM) {
            surfaceFormat_ = format;
            break;
        }
    }

    swapExtent_ = caps.currentExtent;
    if (swapExtent_.width == UINT32_MAX) {
        swapExtent_.width = static_cast<uint32_t>(ANativeWindow_getWidth(window));
        swapExtent_.height = static_cast<uint32_t>(ANativeWindow_getHeight(window));
    }

    uint32_t imageCount = std::max(3u, caps.minImageCount);
    if (caps.maxImageCount != 0 && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = surface;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat_.format;
    createInfo.imageColorSpace = surfaceFormat_.colorSpace;
    createInfo.imageExtent = swapExtent_;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    createInfo.preTransform = caps.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    createInfo.clipped = VK_TRUE;
    vkCheck(vkCreateSwapchainKHR(device_, &createInfo, nullptr, &swapchain_), "presentation vkCreateSwapchainKHR");

    uint32_t swapImageCount = 0;
    vkCheck(vkGetSwapchainImagesKHR(device_, swapchain_, &swapImageCount, nullptr), "presentation swap image count");
    swapImages_.resize(swapImageCount);
    vkCheck(vkGetSwapchainImagesKHR(device_, swapchain_, &swapImageCount, swapImages_.data()),
            "presentation swap images");
    swapInitialized_.assign(swapImageCount, false);

    createPipeline();
    createFramebuffers();
    if (hasFrameSources_) rebuildDescriptors();
    recorder_.resetLogging();
}

void SwapchainRenderer::setFrameSources(
    const std::array<VkImageView, rawrcam::imaging::kRealtimeFramesInFlight>& tonemappedViews,
    const std::array<VkImageView, rawrcam::imaging::kRealtimeFramesInFlight>& linearViews,
    const std::array<VkImageView, rawrcam::imaging::kRealtimeFramesInFlight>& overlayViews,
    const std::array<std::array<VkImageView, 3>, rawrcam::imaging::kRealtimeFramesInFlight>& scopeViews,
    float scopeSourceAspect) {
    tonemappedViews_ = tonemappedViews;
    linearViews_ = linearViews;
    overlayViews_ = overlayViews;
    scopeViews_ = scopeViews;
    scopeSourceAspect_ = scopeSourceAspect > 0.0f ? scopeSourceAspect : 1.0f;
    hasFrameSources_ = true;
    if (descriptorPool_ != VK_NULL_HANDLE) rebuildDescriptors();
}

void SwapchainRenderer::setVideoFrameSources(
    const std::array<VkImageView, rawrcam::imaging::kRealtimeFramesInFlight>& views) {
    videoViews_ = views;
    hasVideoFrameSources_ =
        std::all_of(views.begin(), views.end(), [](VkImageView view) { return view != VK_NULL_HANDLE; });
    if (descriptorPool_ != VK_NULL_HANDLE && hasFrameSources_) rebuildDescriptors();
}

void SwapchainRenderer::clearFrameSources() noexcept {
    tonemappedViews_.fill(VK_NULL_HANDLE);
    linearViews_.fill(VK_NULL_HANDLE);
    overlayViews_.fill(VK_NULL_HANDLE);
    for (auto& views : scopeViews_) views.fill(VK_NULL_HANDLE);
    tonemappedSets_.fill(VK_NULL_HANDLE);
    linearSets_.fill(VK_NULL_HANDLE);
    for (auto& sets : scopeSets_) sets.fill(VK_NULL_HANDLE);
    hasFrameSources_ = false;
}

VkResult SwapchainRenderer::acquireNextImage(VkSemaphore imageAvailable, uint32_t* imageIndex) const {
    if (swapchain_ == VK_NULL_HANDLE) return VK_ERROR_OUT_OF_DATE_KHR;
    return vkAcquireNextImageKHR(device_, swapchain_, 0, imageAvailable, VK_NULL_HANDLE, imageIndex);
}

VkResult SwapchainRenderer::present(VkQueue queue, VkSemaphore renderFinished, uint32_t imageIndex) const {
    if (swapchain_ == VK_NULL_HANDLE) return VK_ERROR_OUT_OF_DATE_KHR;
    VkPresentInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &renderFinished;
    info.swapchainCount = 1;
    info.pSwapchains = &swapchain_;
    info.pImageIndices = &imageIndex;
    return vkQueuePresentKHR(queue, &info);
}

void SwapchainRenderer::record(VkCommandBuffer command, uint32_t frameSlot, uint32_t imageIndex, uint32_t previewWidth,
                               uint32_t previewHeight, int sensorOrientationDegrees, int displayRotationDegrees,
                               uint32_t diagnosticMode, bool monitoringOverlayEnabled, uint32_t exifOrientation,
                               bool videoSource, uint32_t cropX, uint32_t cropY, uint32_t cropW, uint32_t cropH,
                               const rawrcam::geometry::LensDistortionModel* lensWarp, uint32_t lensMirror) {
    if (frameSlot >= rawrcam::imaging::kRealtimeFramesInFlight || imageIndex >= swapImages_.size() ||
        !hasFrameSources_ || (videoSource && !hasVideoFrameSources_)) {
        throw std::out_of_range("SwapchainRenderer record indices/sources invalid");
    }

    RecordInfo info{};
    info.command = command;
    info.swapImage = swapImages_[imageIndex];
    info.swapInitialized = swapInitialized_[imageIndex];
    info.renderPass = renderPass_;
    info.framebuffer = framebuffers_[imageIndex];
    info.swapExtent = swapExtent_;
    info.pipeline = pipeline_;
    info.layout = pipelineLayout_;
    info.tonemappedSet = videoSource ? videoSets_[frameSlot] : tonemappedSets_[frameSlot];
    info.linearSet = linearSets_[frameSlot];
    info.previewWidth = previewWidth;
    info.previewHeight = previewHeight;
    // Crop applies to preview-source frames only; video-source frames carry
    // their own record crop already.
    info.cropX = videoSource ? 0u : cropX;
    info.cropY = videoSource ? 0u : cropY;
    info.cropW = videoSource ? 0u : cropW;
    info.cropH = videoSource ? 0u : cropH;
    info.sensorOrientationDegrees = sensorOrientationDegrees;
    info.displayRotationDegrees = displayRotationDegrees;
    info.exifOrientation = exifOrientation;
    info.diagnosticMode = diagnosticMode;
    info.lensWarp = videoSource ? nullptr : lensWarp;
    // Video-source frames were mirrored by the recorder.
    info.lensMirror = videoSource ? 0u : lensMirror;
    if (!videoSource) {
        // Same gate as PresentRecorder (live view only, no diagnostic/EXIF review).
        const bool live = exifOrientation == 0u && diagnosticMode == 0u;
        std::lock_guard<std::mutex> lock(lensWarpMutex_);
        if (lensWarp && live) presentedLensWarp_ = *lensWarp;
        else presentedLensWarp_.reset();
        presentedLensMirror_ = live ? lensMirror : 0u;
    }
    info.overlayEnabled =
        !videoSource && monitoringOverlayEnabled && overlayViews_[frameSlot] != VK_NULL_HANDLE && diagnosticMode == 0u;
    for (uint32_t i = 0; i < 3; ++i) {
        const auto& placement = scopeState_.placements[i];
        info.scopes[i].descriptor = scopeSets_[frameSlot][i];
        info.scopes[i].x = placement.x;
        info.scopes[i].y = placement.y;
        info.scopes[i].width = placement.width;
        info.scopes[i].height = placement.height;
        info.scopes[i].quarterTurns = placement.presentationQuarterTurns & 3u;
        const float sourceAspect = videoSource ? static_cast<float>(previewWidth) / previewHeight : scopeSourceAspect_;
        info.scopes[i].sourceAspect = (info.scopes[i].quarterTurns & 1u) != 0u ? (1.0f / sourceAspect) : sourceAspect;
        info.scopes[i].cornerRadius = placement.cornerRadius;
        info.scopes[i].enabled = placement.type != rawrcam::monitoring::ScopeType::None && diagnosticMode == 0u;
    }
    recorder_.record(info);
    swapInitialized_[imageIndex] = true;
}

VkShaderModule SwapchainRenderer::createShaderModule(const unsigned char* bytes, size_t size) const {
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = size;
    info.pCode = reinterpret_cast<const uint32_t*>(bytes);
    VkShaderModule module = VK_NULL_HANDLE;
    vkCheck(vkCreateShaderModule(device_, &info, nullptr, &module), "presentation vkCreateShaderModule");
    return module;
}

void SwapchainRenderer::createPipeline() {
    VkAttachmentDescription attachment{};
    attachment.format = surfaceFormat_.format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference attachmentRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &attachmentRef;
    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &attachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    vkCheck(vkCreateRenderPass(device_, &renderPassInfo, nullptr, &renderPass_), "presentation render pass");

    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    for (uint32_t i = 0; i < bindings.size(); ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo descriptorInfo{};
    descriptorInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptorInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    descriptorInfo.pBindings = bindings.data();
    vkCheck(vkCreateDescriptorSetLayout(device_, &descriptorInfo, nullptr, &descriptorSetLayout_),
            "presentation descriptor layout");

    VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PresentPush)};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &descriptorSetLayout_;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &push;
    vkCheck(vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &pipelineLayout_), "presentation pipeline layout");

    VkShaderModule vertex = createShaderModule(present_vert_spv, present_vert_spv_size);
    VkShaderModule fragment = createShaderModule(present_frag_spv, present_frag_spv_size);
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{0, 0, static_cast<float>(swapExtent_.width), static_cast<float>(swapExtent_.height), 0, 1};
    VkRect2D scissor{{0, 0}, swapExtent_};
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;
    const std::array<VkDynamicState, 2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &assembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &raster;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pColorBlendState = &blend;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = pipelineLayout_;
    pipelineInfo.renderPass = renderPass_;
    const VkResult pipelineResult =
        vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline_);
    vkDestroyShaderModule(device_, vertex, nullptr);
    vkDestroyShaderModule(device_, fragment, nullptr);
    vkCheck(pipelineResult, "presentation pipeline");

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0;
    vkCheck(vkCreateSampler(device_, &samplerInfo, nullptr, &sampler_), "presentation sampler");

    constexpr uint32_t kDescriptorSetCount = 6u * rawrcam::imaging::kRealtimeFramesInFlight;
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2u * kDescriptorSetCount};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = kDescriptorSetCount;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    vkCheck(vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_), "presentation descriptor pool");
}

void SwapchainRenderer::createFramebuffers() {
    swapViews_.resize(swapImages_.size());
    framebuffers_.resize(swapImages_.size());
    for (size_t i = 0; i < swapImages_.size(); ++i) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = swapImages_[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = surfaceFormat_.format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCheck(vkCreateImageView(device_, &viewInfo, nullptr, &swapViews_[i]), "presentation swap view");

        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = renderPass_;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &swapViews_[i];
        framebufferInfo.width = swapExtent_.width;
        framebufferInfo.height = swapExtent_.height;
        framebufferInfo.layers = 1;
        vkCheck(vkCreateFramebuffer(device_, &framebufferInfo, nullptr, &framebuffers_[i]), "presentation framebuffer");
    }
}

void SwapchainRenderer::rebuildDescriptors() {
    if (!hasFrameSources_ || descriptorPool_ == VK_NULL_HANDLE) return;
    vkCheck(vkResetDescriptorPool(device_, descriptorPool_, 0), "presentation reset descriptor pool");
    constexpr uint32_t kSetCount = 6u * rawrcam::imaging::kRealtimeFramesInFlight;
    std::array<VkDescriptorSetLayout, kSetCount> layouts{};
    layouts.fill(descriptorSetLayout_);
    std::array<VkDescriptorSet, kSetCount> sets{};
    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = descriptorPool_;
    allocateInfo.descriptorSetCount = static_cast<uint32_t>(sets.size());
    allocateInfo.pSetLayouts = layouts.data();
    vkCheck(vkAllocateDescriptorSets(device_, &allocateInfo, sets.data()), "presentation descriptor sets");

    uint32_t cursor = 0;
    for (uint32_t i = 0; i < rawrcam::imaging::kRealtimeFramesInFlight; ++i) tonemappedSets_[i] = sets[cursor++];
    for (uint32_t i = 0; i < rawrcam::imaging::kRealtimeFramesInFlight; ++i) linearSets_[i] = sets[cursor++];
    for (uint32_t i = 0; i < rawrcam::imaging::kRealtimeFramesInFlight; ++i) videoSets_[i] = sets[cursor++];
    for (uint32_t i = 0; i < rawrcam::imaging::kRealtimeFramesInFlight; ++i)
        for (uint32_t j = 0; j < 3; ++j) scopeSets_[i][j] = sets[cursor++];

    auto updateSet = [&](VkDescriptorSet set, VkImageView source, VkImageView overlay) {
        VkDescriptorImageInfo sourceInfo{sampler_, source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo overlayInfo{sampler_, overlay, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        std::array<VkWriteDescriptorSet, 2> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = set;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &sourceInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = set;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo = &overlayInfo;
        vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    };
    for (uint32_t i = 0; i < rawrcam::imaging::kRealtimeFramesInFlight; ++i) {
        updateSet(tonemappedSets_[i], tonemappedViews_[i], overlayViews_[i]);
        updateSet(linearSets_[i], linearViews_[i], overlayViews_[i]);
        updateSet(videoSets_[i], hasVideoFrameSources_ ? videoViews_[i] : tonemappedViews_[i], overlayViews_[i]);
        for (uint32_t j = 0; j < 3; ++j) updateSet(scopeSets_[i][j], scopeViews_[i][j], scopeViews_[i][j]);
    }
}

void SwapchainRenderer::destroySwapchain() {
    if (device_ == VK_NULL_HANDLE) return;
    for (VkFramebuffer framebuffer : framebuffers_)
        if (framebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(device_, framebuffer, nullptr);
    framebuffers_.clear();
    for (VkImageView view : swapViews_)
        if (view != VK_NULL_HANDLE) vkDestroyImageView(device_, view, nullptr);
    swapViews_.clear();
    swapImages_.clear();
    swapInitialized_.clear();
    tonemappedSets_.fill(VK_NULL_HANDLE);
    linearSets_.fill(VK_NULL_HANDLE);
    for (auto& sets : scopeSets_) sets.fill(VK_NULL_HANDLE);
    if (descriptorPool_ != VK_NULL_HANDLE) vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
    descriptorPool_ = VK_NULL_HANDLE;
    if (sampler_ != VK_NULL_HANDLE) vkDestroySampler(device_, sampler_, nullptr);
    sampler_ = VK_NULL_HANDLE;
    if (pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device_, pipeline_, nullptr);
    pipeline_ = VK_NULL_HANDLE;
    if (pipelineLayout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
    pipelineLayout_ = VK_NULL_HANDLE;
    if (descriptorSetLayout_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_, nullptr);
    descriptorSetLayout_ = VK_NULL_HANDLE;
    if (renderPass_ != VK_NULL_HANDLE) vkDestroyRenderPass(device_, renderPass_, nullptr);
    renderPass_ = VK_NULL_HANDLE;
    if (swapchain_ != VK_NULL_HANDLE) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
    surfaceFormat_ = {};
    swapExtent_ = {};
}

}  // namespace rawrcam::presentation
