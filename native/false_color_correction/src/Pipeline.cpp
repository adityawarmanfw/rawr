#include <algorithm>
#include <array>
#include <fcc/Pipeline.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace fcc {
namespace {
void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string(what) + " failed VkResult=" + std::to_string(int(r)));
}
uint32_t memoryType(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    throw std::runtime_error("fcc: no compatible memory type");
}
struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint64_t bytes = 0;
};
struct Push {
    uint32_t width, height;
    uint32_t normalizedChroma;
    float edgeSigma;
    float chromaBound;
};
VkImageMemoryBarrier imageBarrier(VkImage image, VkAccessFlags src, VkAccessFlags dst, VkImageLayout oldLayout,
                                  VkImageLayout newLayout) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = oldLayout;
    b.newLayout = newLayout;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return b;
}
}  // namespace
struct FalseColorCorrectionPipeline::Impl {
    VulkanContext ctx{};
    ShaderProvider shaders;
    PipelineConfig cfg{};
    Image chroma{};
    Image tempRgb{};
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> sets;
    // 0 median, 1 reconstruct (two-pass reference), 2 fused step used by record().
    std::array<VkShaderModule, 3> modules{};
    std::array<VkPipeline, 3> pipelines{};
    bool initialized = false;
    uint64_t liveBytes = 0, peakBytes = 0;

    Impl(VulkanContext c, ShaderProvider s, PipelineConfig pc) : ctx(c), shaders(std::move(s)), cfg(pc) {
        const char* reason = nullptr;
        if (!FalseColorCorrectionPipeline::validateConfig(cfg, &reason))
            throw std::invalid_argument(reason ? reason : "invalid FCC config");
        try {
            // The fused step keeps median chroma in shared memory; the 1x1
            // image only satisfies the descriptor layout shared with the
            // two-pass shaders.
            chroma = createImage(VK_FORMAT_R32G32_SFLOAT, 1, 1);
            if (cfg.maxSteps >= 2) tempRgb = createImage(VK_FORMAT_R16G16B16A16_SFLOAT);
            createPipelineObjects();
        } catch (...) {
            cleanup();
            throw;
        }
    }
    ~Impl() { cleanup(); }

    Image createImage(VkFormat format) { return createImage(format, cfg.width, cfg.height); }
    Image createImage(VkFormat format, uint32_t width, uint32_t height) {
        Image o{};
        VkImageCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = format;
        ci.extent = {width, height, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_STORAGE_BIT;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateImage(ctx.device, &ci, ctx.allocator, &o.image), "fcc vkCreateImage");
        try {
            VkMemoryRequirements mr{};
            vkGetImageMemoryRequirements(ctx.device, o.image, &mr);
            VkMemoryAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            ai.allocationSize = mr.size;
            ai.memoryTypeIndex = memoryType(ctx.physicalDevice, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            check(vkAllocateMemory(ctx.device, &ai, ctx.allocator, &o.memory), "fcc vkAllocateMemory");
            check(vkBindImageMemory(ctx.device, o.image, o.memory, 0), "fcc vkBindImageMemory");
            VkImageViewCreateInfo vi{};
            vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vi.image = o.image;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = format;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            check(vkCreateImageView(ctx.device, &vi, ctx.allocator, &o.view), "fcc vkCreateImageView");
            o.bytes = mr.size;
        } catch (...) {
            destroyImage(o);  // bytes is still 0, so liveBytes is untouched
            throw;
        }
        liveBytes += o.bytes;
        peakBytes = std::max(peakBytes, liveBytes);
        return o;
    }
    void destroyImage(Image& x) noexcept {
        if (x.view) vkDestroyImageView(ctx.device, x.view, ctx.allocator);
        if (x.image) vkDestroyImage(ctx.device, x.image, ctx.allocator);
        if (x.memory) vkFreeMemory(ctx.device, x.memory, ctx.allocator);
        if (x.bytes) liveBytes -= x.bytes;
        x = {};
    }
    void createPipelineObjects() {
        std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
        for (uint32_t i = 0; i < 3; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo sl{};
        sl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        sl.bindingCount = uint32_t(bindings.size());
        sl.pBindings = bindings.data();
        check(vkCreateDescriptorSetLayout(ctx.device, &sl, ctx.allocator, &setLayout), "fcc set layout");
        VkPushConstantRange pr{};
        pr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pr.offset = 0;
        pr.size = sizeof(Push);
        VkPipelineLayoutCreateInfo pl{};
        pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &setLayout;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &pr;
        check(vkCreatePipelineLayout(ctx.device, &pl, ctx.allocator, &pipelineLayout), "fcc pipeline layout");
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3u * cfg.maxSteps};
        VkDescriptorPoolCreateInfo dpi{};
        dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpi.maxSets = cfg.maxSteps;
        dpi.poolSizeCount = 1;
        dpi.pPoolSizes = &ps;
        check(vkCreateDescriptorPool(ctx.device, &dpi, ctx.allocator, &descriptorPool), "fcc descriptor pool");
        std::vector<VkDescriptorSetLayout> layouts(cfg.maxSteps, setLayout);
        sets.resize(cfg.maxSteps, VK_NULL_HANDLE);
        VkDescriptorSetAllocateInfo da{};
        da.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        da.descriptorPool = descriptorPool;
        da.descriptorSetCount = cfg.maxSteps;
        da.pSetLayouts = layouts.data();
        check(vkAllocateDescriptorSets(ctx.device, &da, sets.data()), "fcc descriptor sets");
        const char* names[3] = {"fcc_chroma_median.comp", "fcc_reconstruct.comp", "fcc_fused.comp"};
        for (uint32_t i = 0; i < 3; ++i) {
            auto words = shaders(names[i]);
            if (words.empty()) throw std::runtime_error(std::string("fcc empty SPIR-V: ") + names[i]);
            VkShaderModuleCreateInfo sm{};
            sm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            sm.codeSize = words.size() * sizeof(uint32_t);
            sm.pCode = words.data();
            check(vkCreateShaderModule(ctx.device, &sm, ctx.allocator, &modules[i]), "fcc shader module");
            VkPipelineShaderStageCreateInfo st{};
            st.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            st.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            st.module = modules[i];
            st.pName = "main";
            VkComputePipelineCreateInfo cp{};
            cp.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            cp.stage = st;
            cp.layout = pipelineLayout;
            check(vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &cp, ctx.allocator, &pipelines[i]),
                  "fcc compute pipeline");
        }
    }
    void cleanup() noexcept {
        if (!ctx.device) return;
        for (auto p : pipelines)
            if (p) vkDestroyPipeline(ctx.device, p, ctx.allocator);
        for (auto m : modules)
            if (m) vkDestroyShaderModule(ctx.device, m, ctx.allocator);
        if (descriptorPool) vkDestroyDescriptorPool(ctx.device, descriptorPool, ctx.allocator);
        if (pipelineLayout) vkDestroyPipelineLayout(ctx.device, pipelineLayout, ctx.allocator);
        if (setLayout) vkDestroyDescriptorSetLayout(ctx.device, setLayout, ctx.allocator);
        destroyImage(tempRgb);
        destroyImage(chroma);
    }
    void initializeScratch(VkCommandBuffer cmd) {
        if (initialized) return;
        std::array<VkImageMemoryBarrier, 2> bs{};
        uint32_t n = 0;
        bs[n++] = imageBarrier(chroma.image, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                               VK_IMAGE_LAYOUT_GENERAL);
        if (tempRgb.image)
            bs[n++] = imageBarrier(tempRgb.image, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                   VK_IMAGE_LAYOUT_GENERAL);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, n, bs.data());
        initialized = true;
    }
    void barrier(VkCommandBuffer cmd, VkImage image, VkAccessFlags src, VkAccessFlags dst) {
        auto b = imageBarrier(image, src, dst, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &b);
    }
    void descriptors(uint32_t setIndex, const LinearRgbImage& src, const LinearRgbImage& dst) {
        VkDescriptorImageInfo ii[3] = {{VK_NULL_HANDLE, src.view, VK_IMAGE_LAYOUT_GENERAL},
                                       {VK_NULL_HANDLE, chroma.view, VK_IMAGE_LAYOUT_GENERAL},
                                       {VK_NULL_HANDLE, dst.view, VK_IMAGE_LAYOUT_GENERAL}};
        std::array<VkWriteDescriptorSet, 3> w{};
        for (uint32_t i = 0; i < 3; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = sets[setIndex];
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w[i].pImageInfo = &ii[i];
        }
        vkUpdateDescriptorSets(ctx.device, uint32_t(w.size()), w.data(), 0, nullptr);
    }
    void dispatch(VkCommandBuffer cmd, uint32_t which, uint32_t setIndex) {
        Push p{cfg.width, cfg.height, cfg.normalizedChroma ? 1u : 0u, cfg.edgeSigma, cfg.chromaBound};
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[which]);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &sets[setIndex], 0, nullptr);
        vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cmd, (cfg.width + 15) / 16, (cfg.height + 15) / 16, 1);
    }
    void oneStep(VkCommandBuffer cmd, const LinearRgbImage& src, const LinearRgbImage& dst, uint32_t setIndex) {
        // Median and reconstruct in one dispatch (fcc_fused.comp): no RG32F
        // round trip and one neighborhood load per tile.
        descriptors(setIndex, src, dst);
        dispatch(cmd, 2, setIndex);
    }
    void record(VkCommandBuffer cmd, const LinearRgbImage& input, const LinearRgbImage& output, uint32_t steps) {
        if (!cmd) throw std::invalid_argument("fcc: null command buffer");
        auto valid = [&](const LinearRgbImage& im) {
            return im.image && im.view && im.format == VK_FORMAT_R16G16B16A16_SFLOAT &&
                   im.layout == VK_IMAGE_LAYOUT_GENERAL && im.width == cfg.width && im.height == cfg.height;
        };
        if (!valid(input) || !valid(output)) throw std::invalid_argument("fcc: invalid RGBA16F image contract");
        if (input.image == output.image) throw std::invalid_argument("fcc: input/output must be distinct images");
        if (steps < 1 || steps > cfg.maxSteps) throw std::invalid_argument("fcc: steps outside configured maxSteps");
        initializeScratch(cmd);
        if (steps == 1) {
            oneStep(cmd, input, output, 0);
            return;
        }
        LinearRgbImage temp{tempRgb.image,           tempRgb.view, VK_FORMAT_R16G16B16A16_SFLOAT,
                            VK_IMAGE_LAYOUT_GENERAL, cfg.width,    cfg.height};
        // Choose the first destination from parity so every step count ends in `output`.
        // Thereafter output/temp ping-pong; one distinct descriptor set per recorded iteration
        // is required because descriptor contents are not snapshotted at vkCmdBindDescriptorSets.
        const LinearRgbImage* src = &input;
        bool nextToOutput = (steps & 1u) != 0u;
        for (uint32_t i = 0; i < steps; ++i) {
            const LinearRgbImage& dst = nextToOutput ? output : temp;
            oneStep(cmd, *src, dst, i);
            if (i + 1u < steps) barrier(cmd, dst.image, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
            src = nextToOutput ? &output : &temp;
            nextToOutput = !nextToOutput;
        }
    }
};
bool FalseColorCorrectionPipeline::validateConfig(const PipelineConfig& c, const char** reason) noexcept {
    const char* r = nullptr;
    if (c.width < 3 || c.height < 4)
        r = "fcc: dimensions must be at least 3x4";
    else if (c.maxSteps < 1 || c.maxSteps > 8)
        r = "fcc: maxSteps must be in 1..8";
    else if (!(c.edgeSigma >= 0.0f) || !(c.edgeSigma <= 1.0f))
        r = "fcc: edgeSigma must be in [0,1]";
    else if (!(c.chromaBound >= 0.0f) || !(c.chromaBound <= 1.0f))
        r = "fcc: chromaBound must be in [0,1]";
    if (reason) *reason = r;
    return r == nullptr;
}
FalseColorCorrectionPipeline::FalseColorCorrectionPipeline(VulkanContext c, ShaderProvider s, PipelineConfig p)
    : impl_(std::make_unique<Impl>(c, std::move(s), p)) {}
FalseColorCorrectionPipeline::~FalseColorCorrectionPipeline() = default;
void FalseColorCorrectionPipeline::record(VkCommandBuffer c, const LinearRgbImage& i, const LinearRgbImage& o,
                                          uint32_t s) {
    impl_->record(c, i, o, s);
}
const PipelineConfig& FalseColorCorrectionPipeline::config() const noexcept { return impl_->cfg; }
uint64_t FalseColorCorrectionPipeline::currentAllocatedBytes() const noexcept { return impl_->liveBytes; }
uint64_t FalseColorCorrectionPipeline::peakAllocatedBytes() const noexcept { return impl_->peakBytes; }
}  // namespace fcc
