#include "rcd/Pipeline.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace rcd {
namespace {

void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(what);
}

uint32_t memoryType(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties m{};
    vkGetPhysicalDeviceMemoryProperties(pd, &m);
    for (uint32_t i = 0; i < m.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (m.memoryTypes[i].propertyFlags & flags) == flags) return i;
    throw std::runtime_error("RCD: no compatible Vulkan memory type");
}

struct OwnedImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint64_t allocationBytes = 0;
};
struct OwnedBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint64_t allocationBytes = 0;
};

}  // namespace

struct RcdPipeline::Impl {
    VulkanContext ctx{};
    ShaderProvider shaders;
    PipelineConfig cfg{};
    std::function<void()> passBoundary;

    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    std::array<VkShaderModule, 7> modules{};
    std::array<VkPipeline, 7> pipelines{};
    VkSampler packedSampler = VK_NULL_HANDLE;
    VkQueryPool timestampPool = VK_NULL_HANDLE;
    float timestampPeriodNs = 0.0f;
    uint32_t timestampValidBits = 0;

    OwnedImage direction{};
    OwnedImage dummyR16{};
    OwnedImage dummyR32{};
    OwnedImage dummyPacked{};
    OwnedBuffer dummyNormalized{};
    OwnedBuffer balance{};
    bool internalLayoutsInitialized = false;

    uint64_t liveBytes = 0;
    uint64_t peakBytes = 0;
    uint64_t allocationCount = 0;
    FrameTelemetry lastTelemetry{};

    struct Push {
        uint32_t width, height, pattern, inputMode;
        float black[4];
        float invRange[4];
        float outputFactor;
        float outputAlpha;
        uint32_t autoBalance;
        uint32_t pad;
        float fixedBalance[4];
    };
    static_assert(sizeof(Push) == 80, "RCD push constant ABI changed");

    Impl(VulkanContext c, ShaderProvider sp, PipelineConfig pc, PipelineAssets)
        : ctx(c), shaders(std::move(sp)), cfg(pc) {
        const char* why = nullptr;
        if (!RcdPipeline::validateConfig(cfg, {}, &why))
            throw std::runtime_error(std::string("RCD configuration invalid: ") + (why ? why : "unknown"));
        if (ctx.physicalDevice == VK_NULL_HANDLE || ctx.device == VK_NULL_HANDLE)
            throw std::runtime_error("RCD VulkanContext requires physicalDevice and device");
        if (!shaders) throw std::runtime_error("RCD ShaderProvider is empty");
        try {
            createResources();
            createTelemetryResources();
        } catch (...) {
            cleanup();
            throw;
        }
    }

    ~Impl() { cleanup(); }

    void createTelemetryResources() {
        if (!cfg.telemetry) return;
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(ctx.physicalDevice, &props);
        timestampPeriodNs = props.limits.timestampPeriod;
        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice, &count, nullptr);
        std::vector<VkQueueFamilyProperties> q(count);
        vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice, &count, q.data());
        if (ctx.queueFamilyIndex >= count || q[ctx.queueFamilyIndex].timestampValidBits == 0)
            throw std::runtime_error("RCD telemetry requires timestamp queries on the selected queue family");
        timestampValidBits = q[ctx.queueFamilyIndex].timestampValidBits;
        VkQueryPoolCreateInfo qi{};
        qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 10;
        check(vkCreateQueryPool(ctx.device, &qi, ctx.allocator, &timestampPool), "RCD vkCreateQueryPool failed");
    }

    void addAllocation(uint64_t n) {
        liveBytes += n;
        peakBytes = std::max(peakBytes, liveBytes);
        ++allocationCount;
    }
    void removeAllocation(uint64_t n) { liveBytes -= n; }

    OwnedImage makeImage(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage) {
        OwnedImage o{};
        o.format = format;
        VkImageCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = format;
        ci.extent = {w, h, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = usage;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        check(vkCreateImage(ctx.device, &ci, ctx.allocator, &o.image), "RCD vkCreateImage failed");
        VkMemoryRequirements mr{};
        vkGetImageMemoryRequirements(ctx.device, o.image, &mr);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = memoryType(ctx.physicalDevice, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(ctx.device, &ai, ctx.allocator, &o.memory), "RCD vkAllocateMemory(image) failed");
        check(vkBindImageMemory(ctx.device, o.image, o.memory, 0), "RCD vkBindImageMemory failed");
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = o.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        check(vkCreateImageView(ctx.device, &vi, ctx.allocator, &o.view), "RCD vkCreateImageView failed");
        o.allocationBytes = mr.size;
        addAllocation(o.allocationBytes);
        return o;
    }

    OwnedBuffer makeBuffer(VkDeviceSize bytes) {
        OwnedBuffer o{};
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = bytes;
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(ctx.device, &bi, ctx.allocator, &o.buffer), "RCD vkCreateBuffer failed");
        VkMemoryRequirements mr{};
        vkGetBufferMemoryRequirements(ctx.device, o.buffer, &mr);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = memoryType(ctx.physicalDevice, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(ctx.device, &ai, ctx.allocator, &o.memory), "RCD vkAllocateMemory(buffer) failed");
        check(vkBindBufferMemory(ctx.device, o.buffer, o.memory, 0), "RCD vkBindBufferMemory failed");
        o.allocationBytes = mr.size;
        addAllocation(o.allocationBytes);
        return o;
    }

    void destroy(OwnedImage& o) {
        if (o.view) vkDestroyImageView(ctx.device, o.view, ctx.allocator);
        if (o.image) vkDestroyImage(ctx.device, o.image, ctx.allocator);
        if (o.memory) vkFreeMemory(ctx.device, o.memory, ctx.allocator);
        if (o.allocationBytes) removeAllocation(o.allocationBytes);
        o = {};
    }
    void destroy(OwnedBuffer& o) {
        if (o.buffer) vkDestroyBuffer(ctx.device, o.buffer, ctx.allocator);
        if (o.memory) vkFreeMemory(ctx.device, o.memory, ctx.allocator);
        if (o.allocationBytes) removeAllocation(o.allocationBytes);
        o = {};
    }

    void createResources() {
        // Direction scratch stores VH and P/Q discrimination. RCD needs no full-RGB private scratch;
        // the caller-owned output image is used as the pass-to-pass RGB working image.
        // Directional discrimination is algorithmically float32 in librtprocess.
        // Keep both VH/PQ weights in RG32F so the only intentional half-float
        // quantization is the caller-owned RGBA16F RGB working/output image.
        direction = makeImage(cfg.width, cfg.height, VK_FORMAT_R32G32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT);
        dummyR16 = makeImage(1, 1, VK_FORMAT_R16_UINT, VK_IMAGE_USAGE_STORAGE_BIT);
        dummyR32 = makeImage(1, 1, VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT);
        dummyPacked = makeImage(1, 1, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT);
        dummyNormalized = makeBuffer(sizeof(float));
        balance =
            makeBuffer(cfg.autoBalance ? 16 + VkDeviceSize((cfg.width + 63) / 64) * ((cfg.height + 63) / 64) * 32 : 16);

        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = VK_FILTER_NEAREST;
        si.minFilter = VK_FILTER_NEAREST;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = 0.0f;
        check(vkCreateSampler(ctx.device, &si, ctx.allocator, &packedSampler), "RCD vkCreateSampler failed");

        std::array<VkDescriptorSetLayoutBinding, 7> b{};
        b[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        b[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        b[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        b[3] = {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        b[4] = {4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        b[5] = {5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        b[6] = {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo sl{};
        sl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        sl.bindingCount = b.size();
        sl.pBindings = b.data();
        check(vkCreateDescriptorSetLayout(ctx.device, &sl, ctx.allocator, &setLayout),
              "RCD descriptor-set layout failed");

        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
        VkPipelineLayoutCreateInfo pl{};
        pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &setLayout;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &pr;
        check(vkCreatePipelineLayout(ctx.device, &pl, ctx.allocator, &pipelineLayout), "RCD pipeline layout failed");

        std::array<VkDescriptorPoolSize, 3> ps{{{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
                                                {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4},
                                                {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}}};
        VkDescriptorPoolCreateInfo dpi{};
        dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpi.maxSets = 1;
        dpi.poolSizeCount = ps.size();
        dpi.pPoolSizes = ps.data();
        check(vkCreateDescriptorPool(ctx.device, &dpi, ctx.allocator, &descriptorPool), "RCD descriptor pool failed");
        VkDescriptorSetAllocateInfo dai{};
        dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dai.descriptorPool = descriptorPool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &setLayout;
        check(vkAllocateDescriptorSets(ctx.device, &dai, &descriptorSet), "RCD descriptor set allocation failed");

        const char* names[7] = {"rcd_direction.comp",     "rcd_green.comp",  "rcd_diagonal.comp",
                                "rcd_green_sites.comp",   "rcd_export.comp", "rcd_balance.comp",
                                "rcd_balance_reduce.comp"};
        for (size_t i = 0; i < (cfg.autoBalance ? 7u : 5u); ++i) {
            auto words = shaders(names[i]);
            if (words.empty()) throw std::runtime_error(std::string("RCD empty SPIR-V: ") + names[i]);
            VkShaderModuleCreateInfo sm{};
            sm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            sm.codeSize = words.size() * sizeof(uint32_t);
            sm.pCode = words.data();
            check(vkCreateShaderModule(ctx.device, &sm, ctx.allocator, &modules[i]), "RCD shader module failed");
            VkPipelineShaderStageCreateInfo st{};
            st.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            st.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            st.module = modules[i];
            st.pName = "main";
            VkComputePipelineCreateInfo ci{};
            ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            ci.stage = st;
            ci.layout = pipelineLayout;
            check(vkCreateComputePipelines(ctx.device, ctx.pipelineCache, 1, &ci, ctx.allocator, &pipelines[i]),
                  "RCD compute pipeline failed");
        }
    }

    void cleanup() noexcept {
        if (ctx.device == VK_NULL_HANDLE) return;
        for (auto& p : pipelines)
            if (p) {
                vkDestroyPipeline(ctx.device, p, ctx.allocator);
                p = VK_NULL_HANDLE;
            }
        for (auto& m : modules)
            if (m) {
                vkDestroyShaderModule(ctx.device, m, ctx.allocator);
                m = VK_NULL_HANDLE;
            }
        if (descriptorPool)
            vkDestroyDescriptorPool(ctx.device, descriptorPool, ctx.allocator), descriptorPool = VK_NULL_HANDLE;
        if (pipelineLayout)
            vkDestroyPipelineLayout(ctx.device, pipelineLayout, ctx.allocator), pipelineLayout = VK_NULL_HANDLE;
        if (setLayout) vkDestroyDescriptorSetLayout(ctx.device, setLayout, ctx.allocator), setLayout = VK_NULL_HANDLE;
        if (timestampPool) vkDestroyQueryPool(ctx.device, timestampPool, ctx.allocator), timestampPool = VK_NULL_HANDLE;
        if (packedSampler) vkDestroySampler(ctx.device, packedSampler, ctx.allocator), packedSampler = VK_NULL_HANDLE;
        destroy(balance);
        destroy(dummyNormalized);
        destroy(dummyPacked);
        destroy(dummyR32);
        destroy(dummyR16);
        destroy(direction);
    }

    static VkImageMemoryBarrier imageBarrier(VkImage image, VkAccessFlags src, VkAccessFlags dst,
                                             VkImageLayout oldLayout, VkImageLayout newLayout) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = src;
        b.dstAccessMask = dst;
        b.oldLayout = oldLayout;
        b.newLayout = newLayout;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        return b;
    }

    void initializeInternalLayouts(VkCommandBuffer cmd) {
        if (internalLayoutsInitialized) return;
        std::array<VkImageMemoryBarrier, 4> bs{{imageBarrier(direction.image, 0, VK_ACCESS_SHADER_WRITE_BIT,
                                                             VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL),
                                                imageBarrier(dummyR16.image, 0, VK_ACCESS_SHADER_READ_BIT,
                                                             VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL),
                                                imageBarrier(dummyR32.image, 0, VK_ACCESS_SHADER_READ_BIT,
                                                             VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL),
                                                imageBarrier(dummyPacked.image, 0, VK_ACCESS_SHADER_READ_BIT,
                                                             VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL)}};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, bs.size(), bs.data());
        internalLayoutsInitialized = true;
    }

    void computeImageBarrier(VkCommandBuffer cmd, VkImage image, VkAccessFlags src, VkAccessFlags dst) {
        auto b = imageBarrier(image, src, dst, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &b);
    }

    void validateOutput(const LinearRgbImage& o) const {
        if (o.image == VK_NULL_HANDLE || o.view == VK_NULL_HANDLE)
            throw std::runtime_error("RCD output image/view is null");
        if (o.width != cfg.width || o.height != cfg.height)
            throw std::runtime_error("RCD output dimensions do not match pipeline");
        if (o.format != VK_FORMAT_R16G16B16A16_SFLOAT) throw std::runtime_error("RCD output must be RGBA16F");
        if (o.layout != VK_IMAGE_LAYOUT_GENERAL) throw std::runtime_error("RCD output must be GENERAL while recording");
    }

    void updateDescriptors(VkBuffer normalized, VkDeviceSize normalizedRange, VkImageView r16, VkImageView r32,
                           VkImageView packed, VkImageLayout packedLayout, VkImageView output) {
        VkDescriptorBufferInfo bi{normalized ? normalized : dummyNormalized.buffer, 0,
                                  normalized ? normalizedRange : sizeof(float)};
        VkDescriptorImageInfo i1{VK_NULL_HANDLE, r16 ? r16 : dummyR16.view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo i2{VK_NULL_HANDLE, r32 ? r32 : dummyR32.view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo i3{packedSampler, packed ? packed : dummyPacked.view,
                                 packed ? packedLayout : VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo i4{VK_NULL_HANDLE, direction.view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo i5{VK_NULL_HANDLE, output, VK_IMAGE_LAYOUT_GENERAL};
        std::array<VkWriteDescriptorSet, 7> w{};
        for (uint32_t i = 0; i < w.size(); ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = descriptorSet;
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
        }
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[0].pBufferInfo = &bi;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[1].pImageInfo = &i1;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[2].pImageInfo = &i2;
        w[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[3].pImageInfo = &i3;
        w[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[4].pImageInfo = &i4;
        w[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[5].pImageInfo = &i5;
        VkDescriptorBufferInfo balanceInfo{balance.buffer, 0, VK_WHOLE_SIZE};
        w[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[6].pBufferInfo = &balanceInfo;
        vkUpdateDescriptorSets(ctx.device, w.size(), w.data(), 0, nullptr);
    }

    Push makePush(InputMode mode, const float black[4], float white) const {
        Push p{};
        p.width = cfg.width;
        p.height = cfg.height;
        p.pattern = static_cast<uint32_t>(cfg.pattern);
        p.inputMode = static_cast<uint32_t>(mode);
        for (int i = 0; i < 4; ++i) {
            p.black[i] = black ? black[i] : 0.0f;
            p.invRange[i] = black ? 1.0f / (white - black[i]) : 1.0f;
        }
        p.outputFactor = cfg.outputScale * 255.0f;
        p.outputAlpha = cfg.outputAlpha;
        p.autoBalance = cfg.autoBalance;
        for (int c = 0; c < 3; ++c) p.fixedBalance[c] = cfg.inputBalance[c];
        p.fixedBalance[3] = 1;
        return p;
    }

    void dispatchAll(VkCommandBuffer cmd, const Push& pc, const LinearRgbImage& output) {
        initializeInternalLayouts(cmd);
        auto inherit = [&](uint32_t v, uint32_t common) { return v ? v : common; };
        const std::array<uint32_t, 5> wx = {
            inherit(cfg.directionWorkgroupX, cfg.workgroupX), inherit(cfg.greenWorkgroupX, cfg.workgroupX),
            inherit(cfg.diagonalWorkgroupX, cfg.workgroupX), inherit(cfg.greenSitesWorkgroupX, cfg.workgroupX),
            inherit(cfg.exportWorkgroupX, cfg.workgroupX)};
        const std::array<uint32_t, 5> wy = {
            inherit(cfg.directionWorkgroupY, cfg.workgroupY), inherit(cfg.greenWorkgroupY, cfg.workgroupY),
            inherit(cfg.diagonalWorkgroupY, cfg.workgroupY), inherit(cfg.greenSitesWorkgroupY, cfg.workgroupY),
            inherit(cfg.exportWorkgroupY, cfg.workgroupY)};
        for (size_t i = 0; i < 5; ++i)
            if (wx[i] == 0u || wy[i] == 0u) throw std::runtime_error("RCD workgroup dimensions must be non-zero");
        const uint32_t halfWidth = (cfg.width + 1u) / 2u;
        std::array<uint32_t, 5> gx{};
        std::array<uint32_t, 5> gy{};
        for (size_t i = 0; i < 5; ++i) {
            const uint32_t logicalWidth = (i == 2u || i == 3u) ? halfWidth : cfg.width;
            gx[i] = (logicalWidth + wx[i] - 1u) / wx[i];
            gy[i] = (cfg.height + wy[i] - 1u) / wy[i];
        }
        if (timestampPool) vkCmdResetQueryPool(cmd, timestampPool, 0, 10);
        if (timestampPool) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, timestampPool, 0);
        if (cfg.autoBalance) {
            // Cover reuse after the preceding frame, then publish both reductions.
            VkMemoryBarrier mb{};
            mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            mb.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            auto sync = [&] {
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                     1, &mb, 0, nullptr, 0, nullptr);
            };
            sync();
            for (uint32_t i = 5; i < 7; ++i) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[i]);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &descriptorSet, 0,
                                        nullptr);
                vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push), &pc);
                vkCmdDispatch(cmd, i == 5 ? (cfg.width + 63) / 64 : 1, i == 5 ? (cfg.height + 63) / 64 : 1, 1);
                sync();
            }
        }
        auto dispatch = [&](size_t i) {
            if (timestampPool && i != 0)
                vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, timestampPool, uint32_t(i * 2));
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[i]);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &descriptorSet, 0,
                                    nullptr);
            vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push), &pc);
            vkCmdDispatch(cmd, gx[i], gy[i], 1);
            if (timestampPool)
                vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, timestampPool, uint32_t(i * 2 + 1));
        };
        dispatch(0);  // direction
        computeImageBarrier(cmd, direction.image, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        if (passBoundary) passBoundary();
        dispatch(1);  // green at R/B / working RGB initialization (margin 4)
        computeImageBarrier(cmd, output.image, VK_ACCESS_SHADER_WRITE_BIT,
                            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        if (passBoundary) passBoundary();
        dispatch(2);  // opposite chroma at R/B: compressed one invocation per non-green site
        computeImageBarrier(cmd, output.image, VK_ACCESS_SHADER_WRITE_BIT,
                            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        if (passBoundary) passBoundary();
        dispatch(3);  // chroma at green sites: compressed one invocation per green site
        computeImageBarrier(cmd, output.image, VK_ACCESS_SHADER_WRITE_BIT,
                            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        if (passBoundary) passBoundary();
        dispatch(4);  // final export: scale + alpha + librtprocess border replacement
        computeImageBarrier(cmd, output.image, VK_ACCESS_SHADER_WRITE_BIT,
                            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        lastTelemetry = {};
        lastTelemetry.peakAllocatedBytes = peakBytes;
        lastTelemetry.liveAllocatedBytes = liveBytes;
        lastTelemetry.bufferAllocationsTotal = allocationCount;
    }

    bool collectTelemetry(FrameTelemetry& out) {
        out = lastTelemetry;
        if (!timestampPool) return true;
        std::array<uint64_t, 10> t{};
        const VkResult r = vkGetQueryPoolResults(ctx.device, timestampPool, 0, uint32_t(t.size()), sizeof(t), t.data(),
                                                 sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        if (r != VK_SUCCESS) return false;
        const uint64_t mask = timestampValidBits >= 64 ? ~uint64_t(0) : ((uint64_t(1) << timestampValidBits) - 1u);
        auto delta = [&](uint64_t a, uint64_t b) { return (b - a) & mask; };
        static const char* names[5] = {"rcd.direction", "rcd.green", "rcd.diagonal", "rcd.green_sites", "rcd.export"};
        out.gpuEvents.clear();
        out.gpuEvents.reserve(6);
        for (size_t i = 0; i < 5; ++i)
            out.gpuEvents.push_back(
                {names[i], double(delta(t[i * 2], t[i * 2 + 1])) * double(timestampPeriodNs) * 1e-6});
        out.gpuEvents.push_back({"rcd.total", double(delta(t[0], t[9])) * double(timestampPeriodNs) * 1e-6});
        lastTelemetry = out;
        return true;
    }

    void recordRaw(VkCommandBuffer cmd, const RawCfaImageView& in, const LinearRgbImage& out) {
        if (cmd == VK_NULL_HANDLE) throw std::runtime_error("RCD command buffer is null");
        if (cfg.inputMode != InputMode::RawR16UintImage && cfg.inputMode != InputMode::RawR32FloatImage)
            throw std::runtime_error("RCD pipeline not configured for RAW image input");
        if (in.width != cfg.width || in.height != cfg.height || in.pattern != cfg.pattern)
            throw std::runtime_error("RCD RAW input dimensions/pattern mismatch");
        if (in.image == VK_NULL_HANDLE || in.view == VK_NULL_HANDLE)
            throw std::runtime_error("RCD RAW image/view is null");
        if (in.layout != VK_IMAGE_LAYOUT_GENERAL) throw std::runtime_error("RCD RAW input must be GENERAL");
        VkFormat expected = cfg.inputMode == InputMode::RawR16UintImage ? VK_FORMAT_R16_UINT : VK_FORMAT_R32_SFLOAT;
        if (in.format != expected) throw std::runtime_error("RCD RAW VkFormat does not match inputMode");
        for (float b : in.blackLevel)
            if (!std::isfinite(b) || !std::isfinite(in.whiteLevel) || !(in.whiteLevel > b))
                throw std::runtime_error("RCD requires finite whiteLevel > each black level");
        validateOutput(out);
        updateDescriptors(VK_NULL_HANDLE, 0, cfg.inputMode == InputMode::RawR16UintImage ? in.view : VK_NULL_HANDLE,
                          cfg.inputMode == InputMode::RawR32FloatImage ? in.view : VK_NULL_HANDLE, VK_NULL_HANDLE,
                          VK_IMAGE_LAYOUT_GENERAL, out.view);
        auto pc = makePush(cfg.inputMode, in.blackLevel, in.whiteLevel);
        dispatchAll(cmd, pc, out);
    }

    void recordNormalized(VkCommandBuffer cmd, const NormalizedBayerBufferView& in, const LinearRgbImage& out) {
        if (cmd == VK_NULL_HANDLE) throw std::runtime_error("RCD command buffer is null");
        if (cfg.inputMode != InputMode::NormalizedFloatBuffer)
            throw std::runtime_error("RCD pipeline not configured for normalized buffer input");
        if (in.width != cfg.width || in.height != cfg.height || in.pattern != cfg.pattern)
            throw std::runtime_error("RCD normalized input mismatch");
        if (in.buffer.buffer == VK_NULL_HANDLE || in.buffer.offset != 0)
            throw std::runtime_error("RCD normalized input requires non-null buffer and offset=0");
        VkDeviceSize needed = VkDeviceSize(size_t(cfg.width) * cfg.height * sizeof(float));
        VkDeviceSize range = in.buffer.range == VK_WHOLE_SIZE ? needed : in.buffer.range;
        if (range < needed) throw std::runtime_error("RCD normalized input buffer is too small");
        validateOutput(out);
        updateDescriptors(in.buffer.buffer, range, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE,
                          VK_IMAGE_LAYOUT_GENERAL, out.view);
        auto pc = makePush(cfg.inputMode, nullptr, 1.0f);
        dispatchAll(cmd, pc, out);
    }

    void recordPacked(VkCommandBuffer cmd, const PackedCfaImageView& in, const LinearRgbImage& out) {
        if (cmd == VK_NULL_HANDLE) throw std::runtime_error("RCD command buffer is null");
        if (cfg.inputMode != InputMode::PackedCfaRgba16fImage)
            throw std::runtime_error("RCD pipeline not configured for packed CFA input");
        if ((cfg.width & 1u) || (cfg.height & 1u))
            throw std::runtime_error("RCD packed CFA requires even raw dimensions");
        if (in.rawWidth != cfg.width || in.rawHeight != cfg.height || in.pattern != cfg.pattern)
            throw std::runtime_error("RCD packed CFA raw dimensions/pattern mismatch");
        if (in.width != cfg.width / 2u || in.height != cfg.height / 2u)
            throw std::runtime_error("RCD packed CFA dimensions must be rawWidth/2 x rawHeight/2");
        if (in.image == VK_NULL_HANDLE || in.view == VK_NULL_HANDLE)
            throw std::runtime_error("RCD packed CFA image/view is null");
        if (in.format != VK_FORMAT_R16G16B16A16_SFLOAT) throw std::runtime_error("RCD packed CFA must be RGBA16F");
        if (in.layout != VK_IMAGE_LAYOUT_GENERAL)
            throw std::runtime_error("RCD packed CFA must be GENERAL while recording");
        validateOutput(out);
        updateDescriptors(VK_NULL_HANDLE, 0, VK_NULL_HANDLE, VK_NULL_HANDLE, in.view, in.layout, out.view);
        auto pc = makePush(cfg.inputMode, nullptr, 1.0f);
        dispatchAll(cmd, pc, out);
    }
};

bool RcdPipeline::validateConfig(const PipelineConfig& c, const PipelineAssets&, const char** reason) noexcept {
    const char* why = nullptr;
    for (float g : c.inputBalance)
        if (!std::isfinite(g) || g < .05f || g > 1.f) {
            if (reason) *reason = "invalid RCD input balance";
            return false;
        }
    if (c.width < 19u || c.height < 19u)
        why = "RCD requires width and height >= 19";
    else if (c.inputMode == InputMode::PackedCfaRgba16fImage && ((c.width & 1u) || (c.height & 1u)))
        why = "packed CFA input requires even raw dimensions";
    else if (!std::isfinite(c.outputScale) || !std::isfinite(c.outputAlpha))
        why = "outputScale and outputAlpha must be finite";
    if (reason) *reason = why;
    return why == nullptr;
}

RcdPipeline::RcdPipeline(VulkanContext c, ShaderProvider s, PipelineConfig p, PipelineAssets a)
    : impl_(std::make_unique<Impl>(c, std::move(s), p, a)) {}
RcdPipeline::~RcdPipeline() = default;
void RcdPipeline::record(VkCommandBuffer c, const RawCfaImageView& i, const LinearRgbImage& o) {
    impl_->recordRaw(c, i, o);
}
void RcdPipeline::record(VkCommandBuffer c, const NormalizedBayerBufferView& i, const LinearRgbImage& o) {
    impl_->recordNormalized(c, i, o);
}
void RcdPipeline::record(VkCommandBuffer c, const PackedCfaImageView& i, const LinearRgbImage& o) {
    impl_->recordPacked(c, i, o);
}
void RcdPipeline::setPassBoundary(std::function<void()> boundary) { impl_->passBoundary = std::move(boundary); }
const PipelineConfig& RcdPipeline::config() const { return impl_->cfg; }
bool RcdPipeline::collectTelemetry(FrameTelemetry& out) { return impl_->collectTelemetry(out); }
uint64_t RcdPipeline::currentAllocatedBytes() const { return impl_->liveBytes; }
uint64_t RcdPipeline::peakAllocatedBytes() const { return impl_->peakBytes; }
void RcdPipeline::forgetExternalImageView(VkImageView) {}

}  // namespace rcd
