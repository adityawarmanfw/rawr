#include "tonemap/TonemapEngine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

#include "RawrNeutralTechnicalLut.h"
#include "GamutConversions.h"
#include "tonemap/lut/LutGpuPayload.h"

namespace tonemap {
namespace {

struct alignas(16) GpuParameters {
    float cameraToWorking[16];
    float toneKnots0123[4];
    float toneKnot4ExposureSaturationGamut[4];
    float vibranceControls[4];
    float renderingConfig[4];
    float toneControls[4];
    float curveSegment0[4];
    float curveSegment1[4];
    float curveSegment2[4];
    float curveSegment3[4];
    uint32_t imageExtent[4];
    uint32_t lutControl[4];
    uint32_t lutOutput[4];
    float lutBlend[4];
    uint32_t lutStageIndexSize[lut::kMaxGpuLutStages][4];
    float lutDomainMin[lut::kMaxGpuLutStages][4];
    float lutDomainMax[lut::kMaxGpuLutStages][4];
    float neutralGamut[16], userInputGamut[16], userOutputGamut[16];
    uint32_t directRender[4];
    float adaptiveTone[4];
};
static_assert(sizeof(GpuParameters) == 880, "GPU parameter layout changed");

constexpr VkDeviceSize kRawLscBytes = 64 * 1024;
struct RawPush {
    float black[4], invRange[4], wb[4];
    uint32_t width, height, outWidth, outHeight;
    uint32_t cropX, cropY, pattern, stridePixels;
    uint32_t bufferEnabled, reduceCfa, lscEnabled, lscWidth, lscHeight, monitorEnabled;
};
static_assert(sizeof(RawPush) == 104, "RAW push layout changed");

struct MemoryTypeSelection {
    uint32_t index = 0;
    VkMemoryPropertyFlags properties = 0;
};

MemoryTypeSelection findHostVisibleMemoryType(VkPhysicalDevice physicalDevice, uint32_t allowedTypes) {
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);

    // Prefer coherent memory, but do not require it. Vulkan only guarantees
    // host-visible memory; non-coherent allocations are flushed explicitly.
    for (int pass = 0; pass < 2; ++pass) {
        for (uint32_t index = 0; index < memoryProperties.memoryTypeCount; ++index) {
            if ((allowedTypes & (1u << index)) == 0) continue;
            const VkMemoryPropertyFlags properties = memoryProperties.memoryTypes[index].propertyFlags;
            if ((properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0) continue;
            const bool coherent = (properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
            if ((pass == 0 && coherent) || (pass == 1 && !coherent)) {
                return {index, properties};
            }
        }
    }
    throw std::runtime_error("TonemapEngine: no host-visible Vulkan memory type");
}

void checkVulkanResult(VkResult result, const char* message) {
    if (result != VK_SUCCESS) throw std::runtime_error(message);
}

bool finite(float value) noexcept { return std::isfinite(value); }

bool validConfig(const TonemapConfig& config) noexcept {
    return finite(config.middleGray) && finite(config.shadowAnchorEV) && finite(config.shoulderOutputCap) &&
           finite(config.gamutCompressionStart) && finite(config.vibranceStrength) &&
           finite(config.vibranceShadowStart) && finite(config.vibranceShadowEnd) && config.middleGray > 0.0f &&
           config.middleGray < 1.0f && config.shadowAnchorEV < 0.0f && config.shoulderOutputCap > config.middleGray &&
           config.shoulderOutputCap < 1.0f && config.gamutCompressionStart > 0.0f &&
           config.gamutCompressionStart < 1.0f && config.vibranceStrength >= 0.0f &&
           config.vibranceShadowStart >= 0.0f && config.vibranceShadowEnd > config.vibranceShadowStart;
}

bool validParams(const TonemapParams& params) noexcept {
    if (static_cast<unsigned>(params.renderTransform) > 3 ||
        static_cast<unsigned>(params.outputSpace.gamut) > 7 ||
        static_cast<unsigned>(params.outputSpace.transfer) > 10) return false;
    return validAdaptiveTone(params.adaptiveTone) && finite(params.exposureEV) && finite(params.blackPointEV) &&
           finite(params.shadowLiftEV) && finite(params.midtoneLiftEV) && finite(params.contrast) &&
           finite(params.shoulderStartEV) && finite(params.whitePointEV) && finite(params.highlightBiasEV) &&
           finite(params.saturation) && finite(params.vibrance) && finite(params.aePostGain) &&
           params.blackPointEV >= -100.0f && params.blackPointEV <= 100.0f && params.shadowLiftEV >= -100.0f &&
           params.shadowLiftEV <= 100.0f && params.midtoneLiftEV >= -100.0f && params.midtoneLiftEV <= 100.0f &&
           params.contrast >= -100.0f && params.contrast <= 100.0f && params.whitePointEV >= -100.0f &&
           params.whitePointEV <= 100.0f && params.highlightBiasEV >= -100.0f && params.highlightBiasEV <= 100.0f &&
           params.saturation >= -100.0f && params.saturation <= 100.0f && params.vibrance >= -100.0f &&
           params.vibrance <= 100.0f && params.aePostGain > 0.0f && params.aePostGain <= kMaxAePostGain;
}

bool supportsStorageImage(VkPhysicalDevice physicalDevice, VkFormat format) noexcept {
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &properties);
    return (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
}

GpuParameters buildGpuParameters(const TonemapParams& params, const float* matrix,
                                 uint32_t outputWidth, uint32_t outputHeight,
                                 uint32_t inputWidth, uint32_t inputHeight, const TonemapConfig& config,
                                 const lut::LutGpuPayload& lutPayload, const detail::GamutConversions& conversions) {
    // The photographic controls are post-render display-domain operators.
    // Compile the base render curve from fixed neutral anchors so slider changes
    // cannot alter the base spline a second time.
    TonemapParams base = params;
    base.blackPointEV = -10.0f;
    base.shadowLiftEV = 0.0f;
    base.midtoneLiftEV = 0.0f;
    base.contrast = 1.0f;
    base.shoulderStartEV = 2.0f;
    base.whitePointEV = 6.0f;
    base.highlightBiasEV = 0.0f;
    const CurveCoefficients curve = compileCurve(base, config);
    GpuParameters gpu{};
    gpu.adaptiveTone[0] = params.adaptiveTone.liftEv;
    gpu.adaptiveTone[1] = params.adaptiveTone.keyEv;
    gpu.adaptiveTone[2] = params.adaptiveTone.highlightEv;
    gpu.directRender[0] = static_cast<uint32_t>(params.renderTransform);
    gpu.directRender[1] = static_cast<uint32_t>(params.outputSpace.gamut);
    gpu.directRender[2] = static_cast<uint32_t>(params.outputSpace.transfer);
    std::memcpy(gpu.neutralGamut, conversions.neutral.data(), sizeof(gpu.neutralGamut));
    std::memcpy(gpu.userInputGamut, conversions.userInput.data(), sizeof(gpu.userInputGamut));
    std::memcpy(gpu.userOutputGamut, conversions.userOutput.data(), sizeof(gpu.userOutputGamut));

    // GLSL matrices are column-major. The public transform is a column-major mat3.
    gpu.cameraToWorking[0] = matrix[0];
    gpu.cameraToWorking[1] = matrix[1];
    gpu.cameraToWorking[2] = matrix[2];
    gpu.cameraToWorking[4] = matrix[3];
    gpu.cameraToWorking[5] = matrix[4];
    gpu.cameraToWorking[6] = matrix[5];
    gpu.cameraToWorking[8] = matrix[6];
    gpu.cameraToWorking[9] = matrix[7];
    gpu.cameraToWorking[10] = matrix[8];
    gpu.cameraToWorking[15] = 1.0f;

    std::memcpy(gpu.toneKnots0123, curve.x.data(), sizeof(gpu.toneKnots0123));
    gpu.toneKnot4ExposureSaturationGamut[0] = curve.x[4];
    gpu.toneKnot4ExposureSaturationGamut[1] = params.exposureEV;
    gpu.toneKnot4ExposureSaturationGamut[2] = params.saturation;
    gpu.toneKnot4ExposureSaturationGamut[3] = config.gamutCompressionStart;

    gpu.vibranceControls[0] = params.vibrance;
    gpu.vibranceControls[1] = config.vibranceStrength;
    gpu.vibranceControls[2] = config.vibranceShadowStart;
    gpu.vibranceControls[3] = config.vibranceShadowEnd;
    gpu.renderingConfig[0] = config.middleGray;
    // A scalar scene-linear gain commutes with the camera->working matrix.
    // Store it in stops so the shader can add it at the existing pre-tone
    // exposure position without three RGB multiplies.
    gpu.renderingConfig[1] = std::log2(params.aePostGain);
    gpu.renderingConfig[2] = params.shadowLiftEV;
    gpu.renderingConfig[3] = params.midtoneLiftEV;
    gpu.toneControls[0] = params.contrast;
    gpu.toneControls[1] = params.highlightBiasEV;
    gpu.toneControls[2] = params.blackPointEV;
    gpu.toneControls[3] = params.whitePointEV;

    std::memcpy(gpu.curveSegment0, curve.cubic[0].data(), sizeof(gpu.curveSegment0));
    std::memcpy(gpu.curveSegment1, curve.cubic[1].data(), sizeof(gpu.curveSegment1));
    std::memcpy(gpu.curveSegment2, curve.cubic[2].data(), sizeof(gpu.curveSegment2));
    std::memcpy(gpu.curveSegment3, curve.cubic[3].data(), sizeof(gpu.curveSegment3));

    gpu.imageExtent[0] = outputWidth;
    gpu.imageExtent[1] = outputHeight;
    gpu.imageExtent[2] = inputWidth;
    gpu.imageExtent[3] = inputHeight;

    gpu.lutControl[0] = lutPayload.enabled ? 1u : 0u;
    gpu.lutControl[1] = lutPayload.stageCount;
    gpu.lutControl[2] = static_cast<uint32_t>(lutPayload.inputSpace.gamut);
    gpu.lutControl[3] = static_cast<uint32_t>(lutPayload.inputSpace.transfer);
    gpu.lutOutput[0] = static_cast<uint32_t>(lutPayload.outputSpace.gamut);
    gpu.lutOutput[1] = static_cast<uint32_t>(lutPayload.outputSpace.transfer);
    gpu.lutOutput[2] = static_cast<uint32_t>(lutPayload.placement);
    gpu.lutOutput[3] = static_cast<uint32_t>(lutPayload.afterAction);
    gpu.lutBlend[0] = lutPayload.intensity;
    gpu.lutBlend[1] = conversions.combineUser ? 1.f : 0.f;
    gpu.lutBlend[2] = conversions.combineNeutral ? 1.f : 0.f;
    for (uint32_t i = 0; i < lut::kMaxGpuLutStages; ++i) {
        const auto& stage = lutPayload.stages[i];
        gpu.lutStageIndexSize[i][0] = stage.texelOffset;
        gpu.lutStageIndexSize[i][1] = stage.size;
        for (int c = 0; c < 3; ++c) {
            gpu.lutDomainMin[i][c] = stage.domainMin[c];
            gpu.lutDomainMax[i][c] = stage.domainMax[c];
        }
    }
    return gpu;
}

}  // namespace

struct TonemapEngine::Impl {
    VulkanContext context{};
    uint32_t frameSlotCount = 0;
    uint32_t workgroupSizeX = 0;
    uint32_t workgroupSizeY = 0;
    VkFormat outputFormat = VK_FORMAT_R8G8B8A8_UNORM;
    bool rawVideoInput = false;
    bool videoMonitorOutput = false;
    bool neutralLutTexture = false;
    bool userLutTexture = false;
    TonemapConfig rendererConfig{};

    VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> descriptorSets;
    std::vector<VkBuffer> uniformBuffers;
    std::vector<VkDeviceMemory> uniformMemory;
    std::vector<void*> mappedUniformMemory;
    std::vector<bool> uniformMemoryCoherent;
    std::vector<VkBuffer> rawLscBuffers;
    std::vector<VkDeviceMemory> rawLscMemory;
    std::vector<void*> mappedRawLsc;
    std::vector<bool> rawLscCoherent;

    VkBuffer neutralLutBuffer = VK_NULL_HANDLE;
    VkDeviceMemory neutralLutMemory = VK_NULL_HANDLE;
    VkImage neutralLutImage = VK_NULL_HANDLE;
    VkImageView neutralLutView = VK_NULL_HANDLE;
    VkSampler neutralLutSampler = VK_NULL_HANDLE;
    struct LutTexture {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
    };
    std::array<LutTexture, lut::kMaxGpuLutStages> userTextures{};
    VkBuffer userLutBuffer = VK_NULL_HANDLE;
    VkDeviceMemory userLutMemory = VK_NULL_HANDLE;
    lut::LutGpuPayload userLutPayload{};
    detail::GamutConversions gamutConversions;

    explicit Impl(const TonemapCreateInfo& createInfo)
        : context(createInfo.context),
          frameSlotCount(createInfo.maxFramesInFlight),
          workgroupSizeX(createInfo.workgroupSizeX),
          workgroupSizeY(createInfo.workgroupSizeY),
          outputFormat(createInfo.outputFormat),
          rawVideoInput(createInfo.rawVideoInput),
          videoMonitorOutput(createInfo.videoMonitorOutput),
          neutralLutTexture(createInfo.neutralLutTexture),
          userLutTexture(createInfo.userLutTexture),
          rendererConfig(createInfo.config),
          userLutPayload(lut::packGpuPayload(createInfo.lutChain)),
          gamutConversions(userLutPayload) {
        const char* reason = nullptr;
        if (!TonemapEngine::validateCreateInfo(createInfo, &reason)) {
            throw std::invalid_argument(reason ? reason : "TonemapEngine: invalid create info");
        }
        try {
            createDescriptorResources();
            if (neutralLutTexture) createLutTexture(createInfo, 65, rawr_neutral_technical::kRgba.data(),
                neutralLutImage, neutralLutMemory, neutralLutView, neutralLutSampler);
            else createNeutralLutBuffer();
            if (userLutTexture) {
                for (uint32_t i = 0; i < userLutPayload.stageCount; ++i) {
                    const auto& stage = userLutPayload.stages[i];
                    auto& texture = userTextures[i];
                    createLutTexture(createInfo, stage.size, userLutPayload.rgbaTexels[stage.texelOffset].data(),
                        texture.image, texture.memory, texture.view, texture.sampler);
                }
            } else createUserLutBuffer();
            createComputePipeline(createInfo.shaderSpirv, createInfo.shaderSpirvBytes);
            createFrameResources();
            if (rawVideoInput) createRawLscResources();
        } catch (...) {
            destroyResources();
            throw;
        }
    }

    ~Impl() { destroyResources(); }

    void createDescriptorResources() {
        std::vector<VkDescriptorSetLayoutBinding> bindings{
            {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {3, neutralLutTexture ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
             1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {4, userLutTexture ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
             userLutTexture ? lut::kMaxGpuLutStages : 1u, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        };
        if (rawVideoInput) {
            bindings.push_back({5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
            bindings.push_back({6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
            bindings.push_back({7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        } else if (videoMonitorOutput) {
            bindings.push_back({7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        }
        VkDescriptorSetLayoutCreateInfo layoutInfo{};

        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();
        checkVulkanResult(
            vkCreateDescriptorSetLayout(context.device, &layoutInfo, context.allocator, &descriptorSetLayout),
            "TonemapEngine: failed to create descriptor-set layout");

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};

        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &descriptorSetLayout;
        VkPushConstantRange rawPush{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(RawPush)};
        if (rawVideoInput) {
            pipelineLayoutInfo.pushConstantRangeCount = 1;
            pipelineLayoutInfo.pPushConstantRanges = &rawPush;
        } else if (videoMonitorOutput) {
            rawPush.size = 3 * sizeof(uint32_t);
            pipelineLayoutInfo.pushConstantRangeCount = 1;
            pipelineLayoutInfo.pPushConstantRanges = &rawPush;
        }
        checkVulkanResult(
            vkCreatePipelineLayout(context.device, &pipelineLayoutInfo, context.allocator, &pipelineLayout),
            "TonemapEngine: failed to create pipeline layout");

        std::vector<VkDescriptorPoolSize> poolSizes{
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, ((rawVideoInput || videoMonitorOutput) ? 3u : 2u) * frameSlotCount},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, frameSlotCount},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, ((rawVideoInput ? 4u : 2u) - (neutralLutTexture ? 1u : 0u) - (userLutTexture ? 1u : 0u)) * frameSlotCount},
        };
        if (neutralLutTexture) poolSizes.push_back({VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            (1u + (userLutTexture ? lut::kMaxGpuLutStages : 0u)) * frameSlotCount});
        // A zero-count pool entry is invalid (the texture-only RGB variant has no SSBOs).
        poolSizes.erase(std::remove_if(poolSizes.begin(), poolSizes.end(), [](const auto& size) {
            return size.descriptorCount == 0;
        }), poolSizes.end());
        VkDescriptorPoolCreateInfo poolInfo{};

        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = frameSlotCount;
        poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        checkVulkanResult(vkCreateDescriptorPool(context.device, &poolInfo, context.allocator, &descriptorPool),
                          "TonemapEngine: failed to create descriptor pool");

        descriptorSets.resize(frameSlotCount);
        const std::vector<VkDescriptorSetLayout> layouts(frameSlotCount, descriptorSetLayout);
        VkDescriptorSetAllocateInfo allocateInfo{};

        allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocateInfo.descriptorPool = descriptorPool;
        allocateInfo.descriptorSetCount = frameSlotCount;
        allocateInfo.pSetLayouts = layouts.data();
        checkVulkanResult(vkAllocateDescriptorSets(context.device, &allocateInfo, descriptorSets.data()),
                          "TonemapEngine: failed to allocate descriptor sets");
    }

    void createLutTexture(const TonemapCreateInfo& ci, uint32_t size, const float* data,
                          VkImage& textureImage, VkDeviceMemory& textureMemory,
                          VkImageView& textureView, VkSampler& textureSampler) {
        const VkDeviceSize bytes = VkDeviceSize(size) * size * size * 4 * sizeof(float);
        VkImageCreateInfo image{};
        image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image.imageType = VK_IMAGE_TYPE_3D;
        image.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        image.extent = {size, size, size};
        image.mipLevels = image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        checkVulkanResult(vkCreateImage(context.device, &image, context.allocator, &textureImage),
                          "TonemapEngine: create LUT texture");
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(context.device, textureImage, &requirements);
        VkPhysicalDeviceMemoryProperties properties{};
        vkGetPhysicalDeviceMemoryProperties(context.physicalDevice, &properties);
        uint32_t type = properties.memoryTypeCount;
        for (int pass = 0; pass < 2 && type == properties.memoryTypeCount; ++pass)
            for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
                if ((requirements.memoryTypeBits & (1u << i)) &&
                    (pass == 1 || (properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))) {
                    type = i;
                    break;
                }
        if (type == properties.memoryTypeCount) throw std::runtime_error("TonemapEngine: LUT image memory type");
        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = type;
        checkVulkanResult(vkAllocateMemory(context.device, &allocation, context.allocator, &textureMemory),
                          "TonemapEngine: allocate LUT texture");
        checkVulkanResult(vkBindImageMemory(context.device, textureImage, textureMemory, 0),
                          "TonemapEngine: bind LUT texture");
        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = textureImage;
        view.viewType = VK_IMAGE_VIEW_TYPE_3D;
        view.format = image.format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        checkVulkanResult(vkCreateImageView(context.device, &view, context.allocator, &textureView),
                          "TonemapEngine: LUT texture view");
        VkSamplerCreateInfo sampler{};
        sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        checkVulkanResult(vkCreateSampler(context.device, &sampler, context.allocator, &textureSampler),
                          "TonemapEngine: LUT sampler");

        VkBuffer staging = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkCommandPool pool = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        void* mapped = nullptr;
        auto cleanup = [&]() {
            if (mapped) vkUnmapMemory(context.device, memory);
            if (fence) vkDestroyFence(context.device, fence, context.allocator);
            if (pool) vkDestroyCommandPool(context.device, pool, context.allocator);
            if (staging) vkDestroyBuffer(context.device, staging, context.allocator);
            if (memory) vkFreeMemory(context.device, memory, context.allocator);
        };
        try {
            VkBufferCreateInfo buffer{};
        buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            buffer.size = bytes;
            buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            checkVulkanResult(vkCreateBuffer(context.device, &buffer, context.allocator, &staging), "LUT staging buffer");
            vkGetBufferMemoryRequirements(context.device, staging, &requirements);
            const auto host = findHostVisibleMemoryType(context.physicalDevice, requirements.memoryTypeBits);
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = host.index;
            checkVulkanResult(vkAllocateMemory(context.device, &allocation, context.allocator, &memory), "LUT staging memory");
            checkVulkanResult(vkBindBufferMemory(context.device, staging, memory, 0), "LUT staging bind");
            checkVulkanResult(vkMapMemory(context.device, memory, 0, VK_WHOLE_SIZE, 0, &mapped), "LUT staging map");
            std::memcpy(mapped, data, static_cast<size_t>(bytes));
            if (!(host.properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
                range.memory = memory;
                range.size = VK_WHOLE_SIZE;
                checkVulkanResult(vkFlushMappedMemoryRanges(context.device, 1, &range), "LUT staging flush");
            }
            vkUnmapMemory(context.device, memory);
            mapped = nullptr;
            VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            pci.queueFamilyIndex = ci.lutUploadQueueFamily;
            checkVulkanResult(vkCreateCommandPool(context.device, &pci, context.allocator, &pool), "LUT upload pool");
            VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            cai.commandPool = pool;
            cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cai.commandBufferCount = 1;
            VkCommandBuffer command;
            checkVulkanResult(vkAllocateCommandBuffers(context.device, &cai, &command), "LUT upload command");
            VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            checkVulkanResult(vkBeginCommandBuffer(command, &begin), "LUT upload begin");
            VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = textureImage;
            barrier.subresourceRange = view.subresourceRange;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &barrier);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = image.extent;
            vkCmdCopyBufferToImage(command, staging, textureImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            barrier.oldLayout = barrier.newLayout;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &barrier);
            checkVulkanResult(vkEndCommandBuffer(command), "LUT upload end");
            VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            checkVulkanResult(vkCreateFence(context.device, &fci, context.allocator, &fence), "LUT upload fence");
            VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &command;
            checkVulkanResult(vkQueueSubmit(ci.lutUploadQueue, 1, &submit, fence), "LUT upload submit");
            checkVulkanResult(vkWaitForFences(context.device, 1, &fence, VK_TRUE, UINT64_MAX), "LUT upload wait");
        } catch (...) {
            cleanup();
            throw;
        }
        cleanup();
    }

    void createNeutralLutBuffer() {
        constexpr VkDeviceSize kBytes = sizeof(float) * rawr_neutral_technical::kFloatCount;
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = kBytes;
        bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        checkVulkanResult(vkCreateBuffer(context.device, &bufferInfo, context.allocator, &neutralLutBuffer),
                          "TonemapEngine: failed to create Rawr neutral LUT buffer");

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(context.device, neutralLutBuffer, &requirements);
        const MemoryTypeSelection memoryType =
            findHostVisibleMemoryType(context.physicalDevice, requirements.memoryTypeBits);
        VkMemoryAllocateInfo memoryInfo{};
        memoryInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        memoryInfo.allocationSize = requirements.size;
        memoryInfo.memoryTypeIndex = memoryType.index;
        checkVulkanResult(vkAllocateMemory(context.device, &memoryInfo, context.allocator, &neutralLutMemory),
                          "TonemapEngine: failed to allocate Rawr neutral LUT memory");
        checkVulkanResult(vkBindBufferMemory(context.device, neutralLutBuffer, neutralLutMemory, 0),
                          "TonemapEngine: failed to bind Rawr neutral LUT memory");

        void* mapped = nullptr;
        checkVulkanResult(vkMapMemory(context.device, neutralLutMemory, 0, kBytes, 0, &mapped),
                          "TonemapEngine: failed to map Rawr neutral LUT memory");
        std::memcpy(mapped, rawr_neutral_technical::kRgba.data(), static_cast<size_t>(kBytes));
        if ((memoryType.properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0) {
            VkMappedMemoryRange range{};
            range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            range.memory = neutralLutMemory;
            range.offset = 0;
            range.size = VK_WHOLE_SIZE;
            checkVulkanResult(vkFlushMappedMemoryRanges(context.device, 1, &range),
                              "TonemapEngine: failed to flush Rawr neutral LUT memory");
        }
        vkUnmapMemory(context.device, neutralLutMemory);
    }

    void createUserLutBuffer() {
        const VkDeviceSize kBytes =
            static_cast<VkDeviceSize>(userLutPayload.rgbaTexels.size()) * sizeof(userLutPayload.rgbaTexels[0]);
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(context.physicalDevice, &properties);
        if (kBytes > properties.limits.maxStorageBufferRange)
            throw std::runtime_error("TonemapEngine: user LUT chain exceeds maxStorageBufferRange");
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = kBytes;
        bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        checkVulkanResult(vkCreateBuffer(context.device, &bufferInfo, context.allocator, &userLutBuffer),
                          "TonemapEngine: failed to create user LUT buffer");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(context.device, userLutBuffer, &requirements);
        const MemoryTypeSelection memoryType =
            findHostVisibleMemoryType(context.physicalDevice, requirements.memoryTypeBits);
        VkMemoryAllocateInfo memoryInfo{};
        memoryInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        memoryInfo.allocationSize = requirements.size;
        memoryInfo.memoryTypeIndex = memoryType.index;
        checkVulkanResult(vkAllocateMemory(context.device, &memoryInfo, context.allocator, &userLutMemory),
                          "TonemapEngine: failed to allocate user LUT memory");
        checkVulkanResult(vkBindBufferMemory(context.device, userLutBuffer, userLutMemory, 0),
                          "TonemapEngine: failed to bind user LUT memory");
        void* mapped = nullptr;
        checkVulkanResult(vkMapMemory(context.device, userLutMemory, 0, kBytes, 0, &mapped),
                          "TonemapEngine: failed to map user LUT memory");
        std::memcpy(mapped, userLutPayload.rgbaTexels.data(), static_cast<size_t>(kBytes));
        if ((memoryType.properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0) {
            VkMappedMemoryRange range{};
            range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            range.memory = userLutMemory;
            range.offset = 0;
            range.size = VK_WHOLE_SIZE;
            checkVulkanResult(vkFlushMappedMemoryRanges(context.device, 1, &range),
                              "TonemapEngine: failed to flush user LUT memory");
        }
        vkUnmapMemory(context.device, userLutMemory);
    }

    void createComputePipeline(const uint32_t* spirv, size_t spirvBytes) {
        VkShaderModuleCreateInfo moduleInfo{};

        moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        moduleInfo.codeSize = spirvBytes;
        moduleInfo.pCode = spirv;
        VkShaderModule shaderModule = VK_NULL_HANDLE;
        checkVulkanResult(vkCreateShaderModule(context.device, &moduleInfo, context.allocator, &shaderModule),
                          "TonemapEngine: failed to create shader module");

        VkPipelineShaderStageCreateInfo stage{};

        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = shaderModule;
        stage.pName = "main";
        const std::array<VkBool32, 2> cstChoices{{gamutConversions.combineUser ? VK_TRUE : VK_FALSE,
                                               gamutConversions.combineNeutral ? VK_TRUE : VK_FALSE}};
        const std::array<VkSpecializationMapEntry, 2> cstEntries{{
            {0, 0, sizeof(VkBool32)}, {1, sizeof(VkBool32), sizeof(VkBool32)}}};
        VkSpecializationInfo cstSpecialization{};
        cstSpecialization.mapEntryCount = static_cast<uint32_t>(cstEntries.size());
        cstSpecialization.pMapEntries = cstEntries.data();
        cstSpecialization.dataSize = sizeof(cstChoices);
        cstSpecialization.pData = cstChoices.data();
        stage.pSpecializationInfo = &cstSpecialization;
        VkComputePipelineCreateInfo pipelineInfo{};

        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.stage = stage;
        pipelineInfo.layout = pipelineLayout;
        const VkResult result =
            vkCreateComputePipelines(context.device, VK_NULL_HANDLE, 1, &pipelineInfo, context.allocator, &pipeline);
        vkDestroyShaderModule(context.device, shaderModule, context.allocator);
        checkVulkanResult(result, "TonemapEngine: failed to create compute pipeline");
    }

    void createFrameResources() {
        uniformBuffers.resize(frameSlotCount, VK_NULL_HANDLE);
        uniformMemory.resize(frameSlotCount, VK_NULL_HANDLE);
        mappedUniformMemory.resize(frameSlotCount, nullptr);
        uniformMemoryCoherent.resize(frameSlotCount, false);

        for (uint32_t slot = 0; slot < frameSlotCount; ++slot) {
            VkBufferCreateInfo bufferInfo{};

            bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bufferInfo.size = sizeof(GpuParameters);
            bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            checkVulkanResult(vkCreateBuffer(context.device, &bufferInfo, context.allocator, &uniformBuffers[slot]),
                              "TonemapEngine: failed to create uniform buffer");

            VkMemoryRequirements requirements{};
            vkGetBufferMemoryRequirements(context.device, uniformBuffers[slot], &requirements);
            const MemoryTypeSelection memoryType =
                findHostVisibleMemoryType(context.physicalDevice, requirements.memoryTypeBits);
            uniformMemoryCoherent[slot] = (memoryType.properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;

            VkMemoryAllocateInfo memoryInfo{};

            memoryInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            memoryInfo.allocationSize = requirements.size;
            memoryInfo.memoryTypeIndex = memoryType.index;
            checkVulkanResult(vkAllocateMemory(context.device, &memoryInfo, context.allocator, &uniformMemory[slot]),
                              "TonemapEngine: failed to allocate uniform-buffer memory");
            checkVulkanResult(vkBindBufferMemory(context.device, uniformBuffers[slot], uniformMemory[slot], 0),
                              "TonemapEngine: failed to bind uniform-buffer memory");
            checkVulkanResult(
                vkMapMemory(context.device, uniformMemory[slot], 0, VK_WHOLE_SIZE, 0, &mappedUniformMemory[slot]),
                "TonemapEngine: failed to map uniform-buffer memory");

            VkDescriptorBufferInfo bufferDescriptor{uniformBuffers[slot], 0, sizeof(GpuParameters)};
            VkDescriptorBufferInfo lutDescriptor{neutralLutBuffer, 0,
                                                 sizeof(float) * rawr_neutral_technical::kFloatCount};
            VkDescriptorBufferInfo userLutDescriptor{
                userLutBuffer, 0,
                static_cast<VkDeviceSize>(userLutPayload.rgbaTexels.size()) * sizeof(userLutPayload.rgbaTexels[0])};
            VkDescriptorImageInfo neutralImage{neutralLutSampler, neutralLutView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            std::array<VkDescriptorImageInfo, lut::kMaxGpuLutStages> userImages{};
            if (userLutTexture) {
                for (uint32_t i = 0; i < lut::kMaxGpuLutStages; ++i) {
                    const auto& texture = userTextures[i];
                    userImages[i] = texture.view ? VkDescriptorImageInfo{texture.sampler, texture.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL} : neutralImage;
                }
            }
            std::array<VkWriteDescriptorSet, 3> writes{};

            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = descriptorSets[slot];
            writes[0].dstBinding = 2;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[0].pBufferInfo = &bufferDescriptor;
            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet = descriptorSets[slot];
            writes[1].dstBinding = 3;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType = neutralLutTexture ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            if (neutralLutTexture) writes[1].pImageInfo = &neutralImage;
            else writes[1].pBufferInfo = &lutDescriptor;
            writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[2].dstSet = descriptorSets[slot];
            writes[2].dstBinding = 4;
            writes[2].descriptorCount = userLutTexture ? lut::kMaxGpuLutStages : 1u;
            writes[2].descriptorType = userLutTexture ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            if (userLutTexture) writes[2].pImageInfo = userImages.data();
            else writes[2].pBufferInfo = &userLutDescriptor;
            vkUpdateDescriptorSets(context.device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
    }

    void uploadParameters(uint32_t frameSlot, const GpuParameters& parameters) {
        std::memcpy(mappedUniformMemory[frameSlot], &parameters, sizeof(parameters));
        if (!uniformMemoryCoherent[frameSlot]) {
            VkMappedMemoryRange range{};

            range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            range.memory = uniformMemory[frameSlot];
            range.offset = 0;
            range.size = VK_WHOLE_SIZE;
            checkVulkanResult(vkFlushMappedMemoryRanges(context.device, 1, &range),
                              "TonemapEngine: failed to flush uniform-buffer memory");
        }
    }

    void createRawLscResources() {
        rawLscBuffers.resize(frameSlotCount, VK_NULL_HANDLE);
        rawLscMemory.resize(frameSlotCount, VK_NULL_HANDLE);
        mappedRawLsc.resize(frameSlotCount, nullptr);
        rawLscCoherent.resize(frameSlotCount, false);
        for (uint32_t slot = 0; slot < frameSlotCount; ++slot) {
            VkBufferCreateInfo bufferInfo{};
            bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bufferInfo.size = kRawLscBytes;
            bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            checkVulkanResult(vkCreateBuffer(context.device, &bufferInfo, context.allocator, &rawLscBuffers[slot]),
                              "TonemapEngine: failed to create RAW LSC buffer");
            VkMemoryRequirements requirements{};
            vkGetBufferMemoryRequirements(context.device, rawLscBuffers[slot], &requirements);
            const MemoryTypeSelection type =
                findHostVisibleMemoryType(context.physicalDevice, requirements.memoryTypeBits);
            rawLscCoherent[slot] = (type.properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
            VkMemoryAllocateInfo allocation{};
            allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = type.index;
            checkVulkanResult(vkAllocateMemory(context.device, &allocation, context.allocator, &rawLscMemory[slot]),
                              "TonemapEngine: failed to allocate RAW LSC memory");
            checkVulkanResult(vkBindBufferMemory(context.device, rawLscBuffers[slot], rawLscMemory[slot], 0),
                              "TonemapEngine: failed to bind RAW LSC memory");
            checkVulkanResult(vkMapMemory(context.device, rawLscMemory[slot], 0, VK_WHOLE_SIZE, 0,
                                          &mappedRawLsc[slot]), "TonemapEngine: failed to map RAW LSC memory");
        }
    }

    void uploadRawLsc(uint32_t slot, const float* data, size_t count) {
        std::memcpy(mappedRawLsc[slot], data, count * sizeof(float));
        if (!rawLscCoherent[slot]) {
            VkMappedMemoryRange range{};
            range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            range.memory = rawLscMemory[slot];
            range.offset = 0;
            range.size = VK_WHOLE_SIZE;
            checkVulkanResult(vkFlushMappedMemoryRanges(context.device, 1, &range),
                              "TonemapEngine: failed to flush RAW LSC memory");
        }
    }

    void destroyResources() noexcept {
        if (!context.device) return;
        for (size_t slot = 0; slot < uniformBuffers.size(); ++slot) {
            if (slot < mappedUniformMemory.size() && mappedUniformMemory[slot] && slot < uniformMemory.size() &&
                uniformMemory[slot]) {
                vkUnmapMemory(context.device, uniformMemory[slot]);
            }
            if (uniformBuffers[slot]) vkDestroyBuffer(context.device, uniformBuffers[slot], context.allocator);
            if (slot < uniformMemory.size() && uniformMemory[slot])
                vkFreeMemory(context.device, uniformMemory[slot], context.allocator);
        }
        for (size_t slot = 0; slot < rawLscBuffers.size(); ++slot) {
            if (slot < mappedRawLsc.size() && mappedRawLsc[slot] && rawLscMemory[slot])
                vkUnmapMemory(context.device, rawLscMemory[slot]);
            if (rawLscBuffers[slot]) vkDestroyBuffer(context.device, rawLscBuffers[slot], context.allocator);
            if (rawLscMemory[slot]) vkFreeMemory(context.device, rawLscMemory[slot], context.allocator);
        }
        for (const auto& texture : userTextures) {
            if (texture.sampler) vkDestroySampler(context.device, texture.sampler, context.allocator);
            if (texture.view) vkDestroyImageView(context.device, texture.view, context.allocator);
            if (texture.image) vkDestroyImage(context.device, texture.image, context.allocator);
            if (texture.memory) vkFreeMemory(context.device, texture.memory, context.allocator);
        }
        if (neutralLutSampler) vkDestroySampler(context.device, neutralLutSampler, context.allocator);
        if (neutralLutView) vkDestroyImageView(context.device, neutralLutView, context.allocator);
        if (neutralLutImage) vkDestroyImage(context.device, neutralLutImage, context.allocator);
        if (neutralLutBuffer) vkDestroyBuffer(context.device, neutralLutBuffer, context.allocator);
        if (neutralLutMemory) vkFreeMemory(context.device, neutralLutMemory, context.allocator);
        if (userLutBuffer) vkDestroyBuffer(context.device, userLutBuffer, context.allocator);
        if (userLutMemory) vkFreeMemory(context.device, userLutMemory, context.allocator);
        if (descriptorPool) vkDestroyDescriptorPool(context.device, descriptorPool, context.allocator);
        if (pipeline) vkDestroyPipeline(context.device, pipeline, context.allocator);
        if (pipelineLayout) vkDestroyPipelineLayout(context.device, pipelineLayout, context.allocator);
        if (descriptorSetLayout) vkDestroyDescriptorSetLayout(context.device, descriptorSetLayout, context.allocator);
    }
};


bool TonemapEngine::supportsNeutralLutTexture(VkPhysicalDevice device) noexcept {
    if (!device) return false;
    VkImageFormatProperties properties{};
    return vkGetPhysicalDeviceImageFormatProperties(device, VK_FORMAT_R32G32B32A32_SFLOAT,
        VK_IMAGE_TYPE_3D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        0, &properties) == VK_SUCCESS && properties.maxExtent.width >= 65 &&
        properties.maxExtent.height >= 65 && properties.maxExtent.depth >= 65;
}

bool TonemapEngine::validateCreateInfo(const TonemapCreateInfo& createInfo, const char** reason) noexcept {
    auto fail = [&](const char* message) {
        if (reason) *reason = message;
        return false;
    };
    if (reason) *reason = nullptr;
    if (!createInfo.context.physicalDevice || !createInfo.context.device)
        return fail("TonemapEngine: Vulkan context is incomplete");
    if (!createInfo.shaderSpirv || createInfo.shaderSpirvBytes < sizeof(uint32_t) ||
        (createInfo.shaderSpirvBytes % sizeof(uint32_t)) != 0)
        return fail("TonemapEngine: SPIR-V buffer is invalid");
    if (createInfo.shaderSpirv[0] != 0x07230203u) return fail("TonemapEngine: SPIR-V magic is invalid");
    if (createInfo.maxFramesInFlight == 0 || createInfo.workgroupSizeX == 0 || createInfo.workgroupSizeY == 0)
        return fail("TonemapEngine: frame-slot/workgroup configuration is invalid");
    if (createInfo.userLutTexture && !createInfo.neutralLutTexture)
        return fail("TonemapEngine: user texture backend requires neutral texture backend");
    if (createInfo.userLutTexture) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(createInfo.context.physicalDevice, &properties);
        if (properties.limits.maxPerStageDescriptorSamplers < 1 + lut::kMaxGpuLutStages ||
            properties.limits.maxPerStageDescriptorSampledImages < 1 + lut::kMaxGpuLutStages)
            return fail("TonemapEngine: insufficient LUT texture descriptors");
    }
    if (createInfo.neutralLutTexture && (!createInfo.lutUploadQueue ||
        !supportsNeutralLutTexture(createInfo.context.physicalDevice)))
        return fail("TonemapEngine: texture LUT requires a compatible device and upload queue");
    if (!validConfig(createInfo.config)) return fail("TonemapEngine: renderer configuration is invalid");
    if (createInfo.outputFormat != VK_FORMAT_R8G8B8A8_UNORM &&
        createInfo.outputFormat != VK_FORMAT_R16G16B16A16_SFLOAT &&
        createInfo.outputFormat != VK_FORMAT_R32G32B32A32_SFLOAT &&
        !(createInfo.videoMonitorOutput && createInfo.outputFormat == VK_FORMAT_A2B10G10R10_UNORM_PACK32))
        return fail("TonemapEngine: output format must be RGBA8, RGBA16F, RGBA32F or video A2B10G10R10");
    if (createInfo.rawVideoInput && (createInfo.outputFormat != VK_FORMAT_R16G16B16A16_SFLOAT ||
                                     createInfo.workgroupSizeX != 8 || createInfo.workgroupSizeY != 8))
        return fail("TonemapEngine: fused RAW video requires RGBA16F and 8x8 workgroups");
    if (createInfo.videoMonitorOutput && createInfo.rawVideoInput)
        return fail("TonemapEngine: video monitor output cannot be combined with RAW input");

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(createInfo.context.physicalDevice, &properties);
    if (createInfo.workgroupSizeX > properties.limits.maxComputeWorkGroupSize[0] ||
        createInfo.workgroupSizeY > properties.limits.maxComputeWorkGroupSize[1] ||
        uint64_t(createInfo.workgroupSizeX) * uint64_t(createInfo.workgroupSizeY) >
            properties.limits.maxComputeWorkGroupInvocations)
        return fail("TonemapEngine: workgroup exceeds device limits");
    if (!supportsStorageImage(createInfo.context.physicalDevice, VK_FORMAT_R16G16B16A16_SFLOAT))
        return fail("TonemapEngine: RGBA16F storage images are unsupported");
    if (!supportsStorageImage(createInfo.context.physicalDevice, createInfo.outputFormat))
        return fail("TonemapEngine: output storage image format is unsupported");
    constexpr VkDeviceSize kNeutralLutBytes = sizeof(float) * rawr_neutral_technical::kFloatCount;
    if (properties.limits.maxStorageBufferRange < kNeutralLutBytes)
        return fail("TonemapEngine: Rawr neutral LUT exceeds maxStorageBufferRange");
    return true;
}

bool TonemapEngine::validateRecordInfo(const TonemapRecordInfo& recordInfo, uint32_t maxFramesInFlight,
                                       const char** reason) noexcept {
    auto fail = [&](const char* message) {
        if (reason) *reason = message;
        return false;
    };
    if (reason) *reason = nullptr;
    if (!recordInfo.commandBuffer) return fail("TonemapEngine: command buffer is null");
    if (!recordInfo.input.view || !recordInfo.output.view)
        return fail("TonemapEngine: input/output image view is null");
    if (recordInfo.input.format != VK_FORMAT_R16G16B16A16_SFLOAT)
        return fail("TonemapEngine: input must be VK_FORMAT_R16G16B16A16_SFLOAT");
    if (recordInfo.output.format != VK_FORMAT_R8G8B8A8_UNORM &&
        recordInfo.output.format != VK_FORMAT_R16G16B16A16_SFLOAT &&
        recordInfo.output.format != VK_FORMAT_R32G32B32A32_SFLOAT &&
        recordInfo.output.format != VK_FORMAT_A2B10G10R10_UNORM_PACK32)
        return fail("TonemapEngine: output must be RGBA8, RGBA16F or A2B10G10R10");
    if (recordInfo.input.layout != VK_IMAGE_LAYOUT_GENERAL || recordInfo.output.layout != VK_IMAGE_LAYOUT_GENERAL)
        return fail("TonemapEngine: input/output must be in VK_IMAGE_LAYOUT_GENERAL");
    if (recordInfo.input.width == 0 || recordInfo.input.height == 0 ||
        recordInfo.output.width == 0 || recordInfo.output.height == 0 ||
        recordInfo.output.width > recordInfo.input.width || recordInfo.output.height > recordInfo.input.height)
        return fail("TonemapEngine: output must fit inside the input");
    if (recordInfo.frameSlot >= maxFramesInFlight) return fail("TonemapEngine: frameSlot is out of range");
    if (!recordInfo.cameraToWorkingColumnMajor3x3) return fail("TonemapEngine: camera-to-working matrix is null");
    for (int index = 0; index < 9; ++index)
        if (!finite(recordInfo.cameraToWorkingColumnMajor3x3[index]))
            return fail("TonemapEngine: camera-to-working matrix contains non-finite values");
    if (!validParams(recordInfo.params)) return fail("TonemapEngine: photographic parameters are invalid");
    return true;
}

TonemapEngine::TonemapEngine(const TonemapCreateInfo& createInfo) : impl_(std::make_unique<Impl>(createInfo)) {}

TonemapEngine::~TonemapEngine() = default;

void TonemapEngine::record(const TonemapRecordInfo& recordInfo) {
    if (impl_->rawVideoInput)
        throw std::invalid_argument("TonemapEngine: use recordRaw with fused RAW video shader");
    const char* reason = nullptr;
    if (!validateRecordInfo(recordInfo, impl_->frameSlotCount, &reason))
        throw std::invalid_argument(reason ? reason : "TonemapEngine: invalid record info");
    if (recordInfo.output.format != impl_->outputFormat)
        throw std::invalid_argument("TonemapEngine: output format differs from shader variant");
    if (impl_->videoMonitorOutput && (!recordInfo.monitor.view ||
        recordInfo.monitor.format != VK_FORMAT_R8G8B8A8_UNORM ||
        recordInfo.monitor.layout != VK_IMAGE_LAYOUT_GENERAL ||
        recordInfo.monitor.width != recordInfo.output.width ||
        recordInfo.monitor.height != recordInfo.output.height))
        throw std::invalid_argument("TonemapEngine: invalid linear video monitor");

    const GpuParameters parameters =
        buildGpuParameters(recordInfo.params, recordInfo.cameraToWorkingColumnMajor3x3,
                           recordInfo.output.width, recordInfo.output.height,
                           recordInfo.input.width, recordInfo.input.height,
                           impl_->rendererConfig, impl_->userLutPayload, impl_->gamutConversions);
    impl_->uploadParameters(recordInfo.frameSlot, parameters);

    VkDescriptorImageInfo inputDescriptor{};
    inputDescriptor.imageView = recordInfo.input.view;
    inputDescriptor.imageLayout = recordInfo.input.layout;
    VkDescriptorImageInfo outputDescriptor{};
    outputDescriptor.imageView = recordInfo.output.view;
    outputDescriptor.imageLayout = recordInfo.output.layout;

    std::array<VkWriteDescriptorSet, 3> writes{};
    writes[0] = {};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = impl_->descriptorSets[recordInfo.frameSlot];
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[0].pImageInfo = &inputDescriptor;
    writes[1] = {};
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = impl_->descriptorSets[recordInfo.frameSlot];
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo = &outputDescriptor;
    VkDescriptorImageInfo monitorDescriptor{};
    if (impl_->videoMonitorOutput) {
        monitorDescriptor.imageView = recordInfo.monitor.view;
        monitorDescriptor.imageLayout = recordInfo.monitor.layout;
        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = impl_->descriptorSets[recordInfo.frameSlot];
        writes[2].dstBinding = 7;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[2].pImageInfo = &monitorDescriptor;
    }
    vkUpdateDescriptorSets(impl_->context.device, impl_->videoMonitorOutput ? 3u : 2u, writes.data(), 0, nullptr);

    vkCmdBindPipeline(recordInfo.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, impl_->pipeline);
    const VkDescriptorSet descriptorSet = impl_->descriptorSets[recordInfo.frameSlot];
    vkCmdBindDescriptorSets(recordInfo.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, impl_->pipelineLayout, 0, 1,
                            &descriptorSet, 0, nullptr);
    if (impl_->videoMonitorOutput) {
        struct {
            uint32_t monitorEnabled;
            float highlightCompression, highlightExposureGain;
        } push{recordInfo.monitorEnabled ? 1u : 0u, recordInfo.highlightCompression,
               recordInfo.highlightExposureGain};
        static_assert(sizeof(push) == 12);
        vkCmdPushConstants(recordInfo.commandBuffer, impl_->pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(push), &push);
    }
    vkCmdDispatch(recordInfo.commandBuffer,
                  (recordInfo.output.width + impl_->workgroupSizeX - 1) / impl_->workgroupSizeX,
                  (recordInfo.output.height + impl_->workgroupSizeY - 1) / impl_->workgroupSizeY, 1);
}

void TonemapEngine::recordRaw(const TonemapRawRecordInfo& r) {
    if (!impl_->rawVideoInput) throw std::invalid_argument("TonemapEngine: RAW video shader was not configured");
    if (!r.commandBuffer || !r.raw.rawImageView || !r.output.view || !r.monitor.view ||
        r.frameSlot >= impl_->frameSlotCount || !r.cameraToWorkingColumnMajor3x3 ||
        r.output.format != VK_FORMAT_R16G16B16A16_SFLOAT ||
        r.monitor.format != VK_FORMAT_R8G8B8A8_UNORM ||
        r.output.layout != VK_IMAGE_LAYOUT_GENERAL || r.monitor.layout != VK_IMAGE_LAYOUT_GENERAL ||
        r.output.width == 0 || r.output.height == 0 ||
        r.monitor.width != r.output.width || r.monitor.height != r.output.height ||
        r.raw.width < r.output.width || r.raw.height < r.output.height ||
        r.raw.cfa > 3 || !finite(r.raw.white) || r.raw.white <= 0.0f || !validParams(r.params))
        throw std::invalid_argument("TonemapEngine: invalid fused RAW video record");
    for (size_t i = 0; i < 4; ++i) {
        if (!finite(r.raw.black[i]) || !finite(r.raw.whiteBalance[i]))
            throw std::invalid_argument("TonemapEngine: non-finite RAW metadata");
    }
    for (size_t i = 0; i < 9; ++i) {
        if (!finite(r.cameraToWorkingColumnMajor3x3[i]))
            throw std::invalid_argument("TonemapEngine: non-finite camera transform");
    }
    const bool useBuffer = r.raw.rawBuffer && r.raw.rawStridePixels >= r.raw.width;
    const bool useLsc = r.raw.lensShading && r.raw.lensShadingWidth >= 2 && r.raw.lensShadingHeight >= 2 &&
                        r.raw.lensShadingCount == size_t(r.raw.lensShadingWidth) * r.raw.lensShadingHeight * 4u &&
                        r.raw.lensShadingCount * sizeof(float) <= kRawLscBytes;
    if (useLsc) impl_->uploadRawLsc(r.frameSlot, r.raw.lensShading, r.raw.lensShadingCount);

    const GpuParameters parameters =
        buildGpuParameters(r.params, r.cameraToWorkingColumnMajor3x3,
                           r.output.width, r.output.height, r.output.width, r.output.height,
                           impl_->rendererConfig, impl_->userLutPayload, impl_->gamutConversions);
    impl_->uploadParameters(r.frameSlot, parameters);

    VkDescriptorImageInfo rawImage{VK_NULL_HANDLE, r.raw.rawImageView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo output{VK_NULL_HANDLE, r.output.view, r.output.layout};
    VkDescriptorImageInfo monitor{VK_NULL_HANDLE, r.monitor.view, r.monitor.layout};
    VkDescriptorBufferInfo rawBuffer{useBuffer ? r.raw.rawBuffer : impl_->rawLscBuffers[r.frameSlot], 0,
                                     VK_WHOLE_SIZE};
    VkDescriptorBufferInfo lscBuffer{impl_->rawLscBuffers[r.frameSlot], 0, VK_WHOLE_SIZE};
    const std::array<uint32_t, 5> bindings{0, 1, 5, 6, 7};
    std::array<VkWriteDescriptorSet, 5> writes{};
    for (size_t i = 0; i < writes.size(); ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = impl_->descriptorSets[r.frameSlot];
        writes[i].dstBinding = bindings[i];
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = (i == 2 || i == 3) ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                                        : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
    writes[0].pImageInfo = &rawImage;
    writes[1].pImageInfo = &output;
    writes[2].pBufferInfo = &rawBuffer;
    writes[3].pBufferInfo = &lscBuffer;
    writes[4].pImageInfo = &monitor;
    vkUpdateDescriptorSets(impl_->context.device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

    RawPush push{};
    for (size_t i = 0; i < 4; ++i) {
        push.black[i] = r.raw.black[i];
        push.invRange[i] = 1.0f / std::max(r.raw.white - r.raw.black[i], 1.0f);
        push.wb[i] = r.raw.whiteBalance[i];
    }
    push.width = r.raw.width;
    push.height = r.raw.height;
    push.outWidth = r.output.width;
    push.outHeight = r.output.height;
    const bool reduce = uint64_t(r.output.width) * 2u <= r.raw.width &&
                        uint64_t(r.output.height) * 2u <= r.raw.height;
    const uint32_t sourceWidth = reduce ? r.output.width * 2u : r.output.width;
    const uint32_t sourceHeight = reduce ? r.output.height * 2u : r.output.height;
    push.cropX = ((r.raw.width - sourceWidth) / 2u) & ~1u;
    push.cropY = ((r.raw.height - sourceHeight) / 2u) & ~1u;
    push.pattern = r.raw.cfa;
    push.stridePixels = r.raw.rawStridePixels;
    push.bufferEnabled = useBuffer ? 1u : 0u;
    push.reduceCfa = reduce ? 1u : 0u;
    push.lscEnabled = useLsc ? 1u : 0u;
    push.lscWidth = r.raw.lensShadingWidth;
    push.lscHeight = r.raw.lensShadingHeight;
    push.monitorEnabled = r.monitorEnabled ? 1u : 0u;
    vkCmdBindPipeline(r.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, impl_->pipeline);
    const VkDescriptorSet set = impl_->descriptorSets[r.frameSlot];
    vkCmdBindDescriptorSets(r.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, impl_->pipelineLayout,
                            0, 1, &set, 0, nullptr);
    vkCmdPushConstants(r.commandBuffer, impl_->pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);
    vkCmdDispatch(r.commandBuffer, (r.output.width + 7u) / 8u, (r.output.height + 7u) / 8u, 1);
}

const TonemapConfig& TonemapEngine::config() const noexcept { return impl_->rendererConfig; }
uint32_t TonemapEngine::maxFramesInFlight() const noexcept { return impl_->frameSlotCount; }

}  // namespace tonemap
