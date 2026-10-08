#include "dual/Pipeline.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "rcd/Pipeline.hpp"
#include "vng4/Pipeline.hpp"
namespace dual {
namespace {
void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(what);
}
uint32_t memType(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties m{};
    vkGetPhysicalDeviceMemoryProperties(pd, &m);
    for (uint32_t i = 0; i < m.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (m.memoryTypes[i].propertyFlags & flags) == flags) return i;
    throw std::runtime_error("dual: no compatible memory type");
}
struct Img {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint64_t bytes = 0;
};
struct Buf {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    uint64_t bytes = 0;
};
struct AutoState {
    float resolvedThreshold = 0.0f, minVariance80 = 0.0f, minVariance40 = 0.0f, refinedVariance = 0.0f;
    uint32_t chosenPass = 0, tileX = 0, tileY = 0, tileSize = 0;
};
static_assert(sizeof(AutoState) == 32);
rcd::BayerPattern rp(BayerPattern p) { return static_cast<rcd::BayerPattern>(static_cast<uint32_t>(p)); }
vng4::BayerPattern vp(BayerPattern p) { return static_cast<vng4::BayerPattern>(static_cast<uint32_t>(p)); }
rcd::InputMode rm(InputMode m) { return static_cast<rcd::InputMode>(static_cast<uint32_t>(m)); }
vng4::InputMode vm(InputMode m) { return static_cast<vng4::InputMode>(static_cast<uint32_t>(m)); }
}  // namespace
struct DualDemosaicPipeline::Impl {
    VulkanContext ctx{};
    ShaderProvider shaders;
    PipelineConfig cfg{};
    std::unique_ptr<rcd::RcdPipeline> rcdPipe;
    std::unique_ptr<vng4::Vng4Pipeline> vngPipe;
    std::function<void()> passBoundary;
    static constexpr size_t kAutoTilesPipe = 5;  // dual_auto_tiles.comp
    void boundary() {
        if (passBoundary) passBoundary();
    }
    Img vngOut{}, lum{}, maskA{}, maskB{}, diagRcd{}, diagMaskPre{};
    Buf autoState{};
    bool initialized = false;
    uint64_t live = 0, peak = 0, allocs = 0;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    std::array<VkShaderModule, 9> modules{};
    std::array<VkPipeline, 9> pipes{};
    VkQueryPool queries = VK_NULL_HANDLE;
    float timestampNs = 0;
    uint32_t timestampBits = 0;
    FrameTelemetry last{};
    struct Push {
        uint32_t width, height;
        float threshold, outputFactor, outputAlpha, b1, b2, b3, B;
        float m00, m01, m02, m10, m11, m12, m20, m21, m22;
        uint32_t autoContrast, autoPass;
    };
    static_assert(sizeof(Push) == 80);
    Impl(VulkanContext c, ShaderProvider s, PipelineConfig p, PipelineAssets) : ctx(c), shaders(std::move(s)), cfg(p) {
        const char* why = nullptr;
        if (!DualDemosaicPipeline::validateConfig(cfg, {}, &why))
            throw std::runtime_error(std::string("dual invalid config: ") + (why ? why : "unknown"));
        if (!ctx.device || !ctx.physicalDevice) throw std::runtime_error("dual requires Vulkan device");
        if (!shaders) throw std::runtime_error("dual ShaderProvider empty");
        try {
            createImages();
            createDualPipelines();
            createSubPipelines();
            createTelemetry();
        } catch (...) {
            cleanup();
            throw;
        }
    }
    ~Impl() { cleanup(); }
    void add(uint64_t n) {
        live += n;
        peak = std::max(peak, live);
        ++allocs;
    }
    void sub(uint64_t n) { live -= n; }
    Img makeImg(VkFormat fmt) {
        Img o;
        VkImageCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = fmt;
        ci.extent = {cfg.width, cfg.height, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateImage(ctx.device, &ci, ctx.allocator, &o.image), "dual vkCreateImage");
        VkMemoryRequirements mr{};
        vkGetImageMemoryRequirements(ctx.device, o.image, &mr);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = memType(ctx.physicalDevice, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(ctx.device, &ai, ctx.allocator, &o.memory), "dual vkAllocateMemory");
        check(vkBindImageMemory(ctx.device, o.image, o.memory, 0), "dual vkBindImageMemory");
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = o.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = fmt;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        check(vkCreateImageView(ctx.device, &vi, ctx.allocator, &o.view), "dual vkCreateImageView");
        o.bytes = mr.size;
        add(o.bytes);
        return o;
    }
    void destroy(Img& o) {
        if (o.view) vkDestroyImageView(ctx.device, o.view, ctx.allocator);
        if (o.image) vkDestroyImage(ctx.device, o.image, ctx.allocator);
        if (o.memory) vkFreeMemory(ctx.device, o.memory, ctx.allocator);
        if (o.bytes) sub(o.bytes);
        o = {};
    }
    Buf makeHostBuffer(VkDeviceSize size) {
        Buf o;
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = size;
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(ctx.device, &bi, ctx.allocator, &o.buffer), "dual vkCreateBuffer");
        VkMemoryRequirements mr{};
        vkGetBufferMemoryRequirements(ctx.device, o.buffer, &mr);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = memType(ctx.physicalDevice, mr.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(vkAllocateMemory(ctx.device, &ai, ctx.allocator, &o.memory), "dual vkAllocateMemory buffer");
        check(vkBindBufferMemory(ctx.device, o.buffer, o.memory, 0), "dual vkBindBufferMemory");
        check(vkMapMemory(ctx.device, o.memory, 0, size, 0, &o.mapped), "dual vkMapMemory");
        o.bytes = mr.size;
        add(o.bytes);
        *static_cast<AutoState*>(o.mapped) = AutoState{};
        return o;
    }
    void destroy(Buf& o) {
        if (o.mapped && o.memory) vkUnmapMemory(ctx.device, o.memory);
        if (o.buffer) vkDestroyBuffer(ctx.device, o.buffer, ctx.allocator);
        if (o.memory) vkFreeMemory(ctx.device, o.memory, ctx.allocator);
        if (o.bytes) sub(o.bytes);
        o = {};
    }
    void createImages() {
        vngOut = makeImg(VK_FORMAT_R16G16B16A16_SFLOAT);
        lum = makeImg(VK_FORMAT_R32_SFLOAT);
        maskA = makeImg(VK_FORMAT_R32_SFLOAT);
        maskB = makeImg(VK_FORMAT_R32_SFLOAT);
        if (cfg.diagnosticBranchOutputs) {
            diagRcd = makeImg(VK_FORMAT_R16G16B16A16_SFLOAT);
            diagMaskPre = makeImg(VK_FORMAT_R32_SFLOAT);
        }
        autoState = makeHostBuffer(sizeof(AutoState));
    }
    void createSubPipelines() {
        rcd::PipelineConfig r{};
        r.autoBalance = cfg.autoBalance;
        r.width = cfg.width;
        r.height = cfg.height;
        r.pattern = rp(cfg.pattern);
        r.inputMode = rm(cfg.inputMode);
        r.outputScale = 1.0f / 255.0f;
        r.outputAlpha = 1.0f;
        r.telemetry = cfg.telemetry;
        vng4::PipelineConfig v{};
        v.width = cfg.width;
        v.height = cfg.height;
        v.pattern = vp(cfg.pattern);
        v.inputMode = vm(cfg.inputMode);
        v.outputScale = 1.0f / 255.0f;
        v.outputAlpha = 1.0f;
        v.telemetry = cfg.telemetry;
        rcd::VulkanContext rc{ctx.physicalDevice, ctx.device, ctx.queueFamilyIndex, ctx.allocator, ctx.pipelineCache};
        vng4::VulkanContext vc{ctx.physicalDevice, ctx.device, ctx.queueFamilyIndex, ctx.allocator, ctx.pipelineCache};
        rcdPipe = std::make_unique<rcd::RcdPipeline>(rc, [this](std::string_view n) { return shaders(n); }, r);
        vngPipe = std::make_unique<vng4::Vng4Pipeline>(vc, [this](std::string_view n) { return shaders(n); }, v);
    }
    void createDualPipelines() {
        std::array<VkDescriptorSetLayoutBinding, 6> b{};
        for (uint32_t i = 0; i < 5; ++i)
            b[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        b[5] = {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo sl{};
        sl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        sl.bindingCount = b.size();
        sl.pBindings = b.data();
        check(vkCreateDescriptorSetLayout(ctx.device, &sl, ctx.allocator, &setLayout), "dual set layout");
        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
        VkPipelineLayoutCreateInfo pl{};
        pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &setLayout;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &pr;
        check(vkCreatePipelineLayout(ctx.device, &pl, ctx.allocator, &pipeLayout), "dual pipeline layout");
        std::array<VkDescriptorPoolSize, 2> ps{
            {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 5}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}}};
        VkDescriptorPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pi.maxSets = 1;
        pi.poolSizeCount = ps.size();
        pi.pPoolSizes = ps.data();
        check(vkCreateDescriptorPool(ctx.device, &pi, ctx.allocator, &pool), "dual descriptor pool");
        VkDescriptorSetAllocateInfo da{};
        da.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        da.descriptorPool = pool;
        da.descriptorSetCount = 1;
        da.pSetLayouts = &setLayout;
        check(vkAllocateDescriptorSets(ctx.device, &da, &set), "dual descriptor set");
        const std::array<const char*, 9> names{
            {"dual_luminance.comp", "dual_mask.comp", "dual_gauss_h.comp", "dual_gauss_v.comp", "dual_blend.comp",
             "dual_auto_tiles.comp", "dual_auto_select.comp", "dual_auto_refine.comp", "dual_auto_threshold.comp"}};
        for (size_t i = 0; i < names.size(); ++i) {
            auto words = shaders(names[i]);
            if (words.empty()) throw std::runtime_error(std::string("dual empty SPIR-V: ") + names[i]);
            VkShaderModuleCreateInfo sm{};
            sm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            sm.codeSize = words.size() * 4;
            sm.pCode = words.data();
            check(vkCreateShaderModule(ctx.device, &sm, ctx.allocator, &modules[i]), "dual shader module");
            VkPipelineShaderStageCreateInfo st{};
            st.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            st.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            st.module = modules[i];
            st.pName = "main";
            VkComputePipelineCreateInfo ci{};
            ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            // The auto-contrast tile scan may be split into workgroup bands (dispatchBanded).
            if (i == kAutoTilesPipe) ci.flags = VK_PIPELINE_CREATE_DISPATCH_BASE_BIT;
            ci.stage = st;
            ci.layout = pipeLayout;
            check(vkCreateComputePipelines(ctx.device, ctx.pipelineCache, 1, &ci, ctx.allocator, &pipes[i]),
                  "dual compute pipeline");
        }
    }
    void createTelemetry() {
        if (!cfg.telemetry) return;
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(ctx.physicalDevice, &p);
        timestampNs = p.limits.timestampPeriod;
        uint32_t n = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice, &n, nullptr);
        std::vector<VkQueueFamilyProperties> q(n);
        vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice, &n, q.data());
        if (ctx.queueFamilyIndex >= n || q[ctx.queueFamilyIndex].timestampValidBits == 0)
            throw std::runtime_error("dual timestamps unsupported");
        timestampBits = q[ctx.queueFamilyIndex].timestampValidBits;
        VkQueryPoolCreateInfo qi{};
        qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 20;
        check(vkCreateQueryPool(ctx.device, &qi, ctx.allocator, &queries), "dual query pool");
    }
    void cleanup() noexcept {
        if (!ctx.device) return;
        vngPipe.reset();
        rcdPipe.reset();
        for (auto& p : pipes)
            if (p) vkDestroyPipeline(ctx.device, p, ctx.allocator);
        for (auto& m : modules)
            if (m) vkDestroyShaderModule(ctx.device, m, ctx.allocator);
        if (pool) vkDestroyDescriptorPool(ctx.device, pool, ctx.allocator);
        if (pipeLayout) vkDestroyPipelineLayout(ctx.device, pipeLayout, ctx.allocator);
        if (setLayout) vkDestroyDescriptorSetLayout(ctx.device, setLayout, ctx.allocator);
        if (queries) vkDestroyQueryPool(ctx.device, queries, ctx.allocator);
        destroy(autoState);
        destroy(diagMaskPre);
        destroy(diagRcd);
        destroy(maskB);
        destroy(maskA);
        destroy(lum);
        destroy(vngOut);
    }
    static VkImageMemoryBarrier barrier(VkImage im, VkAccessFlags src, VkAccessFlags dst, VkImageLayout oldL,
                                        VkImageLayout newL) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = src;
        b.dstAccessMask = dst;
        b.oldLayout = oldL;
        b.newLayout = newL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = im;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        return b;
    }
    void init(VkCommandBuffer cmd) {
        if (initialized) return;
        std::vector<VkImageMemoryBarrier> b{
            barrier(vngOut.image, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL),
            barrier(lum.image, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL),
            barrier(maskA.image, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL),
            barrier(maskB.image, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL)};
        if (diagRcd.image)
            b.push_back(barrier(diagRcd.image, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_GENERAL));
        if (diagMaskPre.image)
            b.push_back(barrier(diagMaskPre.image, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_GENERAL));
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                             0, nullptr, b.size(), b.data());
        initialized = true;
    }
    void compBarrier(VkCommandBuffer cmd, VkImage im, VkAccessFlags dst = VK_ACCESS_SHADER_READ_BIT) {
        auto b = barrier(im, VK_ACCESS_SHADER_WRITE_BIT, dst, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &b);
    }
    void bufferComputeBarrier(VkCommandBuffer cmd) {
        VkBufferMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.buffer = autoState.buffer;
        b.offset = 0;
        b.size = sizeof(AutoState);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             nullptr, 1, &b, 0, nullptr);
    }
    void bufferHostBarrier(VkCommandBuffer cmd) {
        VkBufferMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.buffer = autoState.buffer;
        b.offset = 0;
        b.size = sizeof(AutoState);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
                             &b, 0, nullptr);
    }
    void setDescriptors(const LinearRgbImage& out) {
        VkDescriptorImageInfo ii[5] = {{VK_NULL_HANDLE, out.view, VK_IMAGE_LAYOUT_GENERAL},
                                       {VK_NULL_HANDLE, vngOut.view, VK_IMAGE_LAYOUT_GENERAL},
                                       {VK_NULL_HANDLE, lum.view, VK_IMAGE_LAYOUT_GENERAL},
                                       {VK_NULL_HANDLE, maskA.view, VK_IMAGE_LAYOUT_GENERAL},
                                       {VK_NULL_HANDLE, maskB.view, VK_IMAGE_LAYOUT_GENERAL}};
        VkDescriptorBufferInfo bi{autoState.buffer, 0, sizeof(AutoState)};
        std::array<VkWriteDescriptorSet, 6> w{};
        for (uint32_t i = 0; i < 5; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = set;
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w[i].pImageInfo = &ii[i];
        }
        w[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[5].dstSet = set;
        w[5].dstBinding = 5;
        w[5].descriptorCount = 1;
        w[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[5].pBufferInfo = &bi;
        vkUpdateDescriptorSets(ctx.device, w.size(), w.data(), 0, nullptr);
    }
    Push push() const {
        Push p{};
        p.width = cfg.width;
        p.height = cfg.height;
        p.threshold = cfg.contrastPercent / 100.0f;
        p.outputFactor = cfg.outputScale * 255.0f;
        p.outputAlpha = cfg.outputAlpha;
        double sigma = 2.0;
        double q = 3.97156 - 4.14554 * std::sqrt(1.0 - 0.26891 * sigma);
        double b0 = 1.57825 + 2.44413 * q + 1.4281 * q * q + 0.422205 * q * q * q;
        double b1 = (2.44413 * q + 2.85619 * q * q + 1.26661 * q * q * q) / b0,
               b2 = (-1.4281 * q * q - 1.26661 * q * q * q) / b0, b3 = (0.422205 * q * q * q) / b0,
               B = 1.0 - (b1 + b2 + b3);
        double M[3][3];
        M[0][0] = -b3 * b1 + 1.0 - b3 * b3 - b2;
        M[0][1] = (b3 + b1) * (b2 + b3 * b1);
        M[0][2] = b3 * (b1 + b3 * b2);
        M[1][0] = b1 + b3 * b2;
        M[1][1] = -(b2 - 1.0) * (b2 + b3 * b1);
        M[1][2] = -(b3 * b1 + b3 * b3 + b2 - 1.0) * b3;
        M[2][0] = b3 * b1 + b2 + b1 * b1 - b2 * b2;
        M[2][1] = b1 * b2 + b3 * b2 * b2 - b1 * b3 * b3 - b3 * b3 * b3 - b3 * b2 + b3;
        M[2][2] = b3 * (b1 + b3 * b2);
        double f = (1.0 + b2 + (b1 - b3) * b3) / ((1.0 + b1 - b2 + b3) * (1.0 - b1 - b2 - b3));
        for (auto& row : M)
            for (double& x : row) x *= f;
        p.autoContrast = cfg.autoContrast ? 1u : 0u;
        p.autoPass = 0u;
        p.b1 = float(b1);
        p.b2 = float(b2);
        p.b3 = float(b3);
        p.B = float(B);
        p.m00 = float(M[0][0]);
        p.m01 = float(M[0][1]);
        p.m02 = float(M[0][2]);
        p.m10 = float(M[1][0]);
        p.m11 = float(M[1][1]);
        p.m12 = float(M[1][2]);
        p.m20 = float(M[2][0]);
        p.m21 = float(M[2][1]);
        p.m22 = float(M[2][2]);
        return p;
    }
    void dispatch(VkCommandBuffer cmd, uint32_t pipeIdx, uint32_t querySlot, uint32_t gx, uint32_t gy, const Push& pc) {
        if (queries) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, queries, querySlot * 2);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipes[pipeIdx]);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push), &pc);
        vkCmdDispatch(cmd, gx, gy, 1);
        if (queries) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, queries, querySlot * 2 + 1);
    }
    // 1-D dispatch split into workgroup bands with a pass boundary between them, so
    // one long scan (the 40x40 tile variance pass is ~90 ms at 12 MP on Adreno)
    // never occupies the GPU as a single submission. Bands write disjoint tiles.
    void dispatchBanded(VkCommandBuffer cmd, uint32_t pipeIdx, uint32_t querySlot, uint32_t gx, const Push& pc) {
        if (!passBoundary) {
            dispatch(cmd, pipeIdx, querySlot, gx, 1, pc);
            return;
        }
        constexpr uint32_t kBands = 6;
        const uint32_t perBand = std::max(1u, (gx + kBands - 1u) / kBands);
        if (queries) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, queries, querySlot * 2);
        for (uint32_t x = 0; x < gx; x += perBand) {
            if (x != 0) boundary();
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipes[pipeIdx]);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1, &set, 0, nullptr);
            vkCmdPushConstants(cmd, pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push), &pc);
            vkCmdDispatchBase(cmd, x, 0, 0, std::min(perBand, gx - x), 1, 1);
        }
        if (queries) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, queries, querySlot * 2 + 1);
    }
    void dispatchNoQuery(VkCommandBuffer cmd, uint32_t pipeIdx, uint32_t gx, uint32_t gy, const Push& pc) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipes[pipeIdx]);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push), &pc);
        vkCmdDispatch(cmd, gx, gy, 1);
    }
    void validateOut(const LinearRgbImage& o) const {
        if (!o.image || !o.view) throw std::runtime_error("dual output null");
        if (o.width != cfg.width || o.height != cfg.height) throw std::runtime_error("dual output dimensions mismatch");
        if (o.format != VK_FORMAT_R16G16B16A16_SFLOAT || o.layout != VK_IMAGE_LAYOUT_GENERAL)
            throw std::runtime_error("dual output must be RGBA16F GENERAL");
    }
    rcd::LinearRgbImage rout(const LinearRgbImage& out) const {
        return {out.image, out.view, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_LAYOUT_GENERAL, cfg.width, cfg.height};
    }
    vng4::LinearRgbImage vout() const {
        return {vngOut.image, vngOut.view, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_LAYOUT_GENERAL,
                cfg.width,    cfg.height};
    }
    bool fusedVng() const { return cfg.optimizationMode == OptimizationMode::VngExportBlend; }
    void noopQuery(VkCommandBuffer cmd, uint32_t slot) {
        if (!queries) return;
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, queries, slot * 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, queries, slot * 2 + 1);
    }
    void diagnosticCopy(VkCommandBuffer cmd, VkImage src, VkImage dst) {
        if (!src || !dst) return;
        VkImageMemoryBarrier pre = barrier(src, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                                           VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &pre);
        VkImageCopy c{};
        c.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.extent = {cfg.width, cfg.height, 1};
        vkCmdCopyImage(cmd, src, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        VkImageMemoryBarrier post = barrier(src, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                                            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &post);
    }
    template <class RRecord, class VRecord, class VBRecord>
    void run(VkCommandBuffer cmd, const LinearRgbImage& out, RRecord&& rr, VRecord&& vr, VBRecord&& vbr) {
        validateOut(out);
        init(cmd);
        setDescriptors(out);
        rr(rout(out));
        if (cfg.diagnosticBranchOutputs) diagnosticCopy(cmd, out.image, diagRcd.image);
        if (!cfg.autoContrast && cfg.contrastPercent == 0.0f) {
            last.peakAllocatedBytes = peak + rcdPipe->peakAllocatedBytes() + vngPipe->peakAllocatedBytes();
            last.liveAllocatedBytes = live + rcdPipe->currentAllocatedBytes() + vngPipe->currentAllocatedBytes();
            last.bufferAllocationsTotal = allocs;
            return;
        }
        if (queries) vkCmdResetQueryPool(cmd, queries, 0, 20);
        compBarrier(cmd, out.image);
        boundary();
        Push pc = push();
        dispatch(cmd, 0, 0, (cfg.width + 15) / 16, (cfg.height + 15) / 16, pc);
        compBarrier(cmd, lum.image);
        boundary();
        if (cfg.autoContrast) {
            const uint32_t n80 = (cfg.width / 80u) * (cfg.height / 80u);
            const uint32_t n40 = (cfg.width / 10u - 3u) * (cfg.height / 10u - 3u);
            pc.autoPass = 0u;
            dispatchBanded(cmd, kAutoTilesPipe, 1, (n80 + 63u) / 64u, pc);
            compBarrier(cmd, maskB.image);
            boundary();
            // Select pass 0 immediately. If RT's <=1 variance condition succeeds, the
            // 40x40 tile shader sees chosenPass==0 and returns before doing any image work.
            dispatchNoQuery(cmd, 6, 1, 1, pc);
            bufferComputeBarrier(cmd);
            pc.autoPass = 1u;
            dispatchBanded(cmd, kAutoTilesPipe, 2, (n40 + 63u) / 64u, pc);
            compBarrier(cmd, maskB.image);
            boundary();
            dispatch(cmd, 6, 3, 1, 1, pc);
            bufferComputeBarrier(cmd);
            dispatch(cmd, 7, 4, 7, 1, pc);
            compBarrier(cmd, maskB.image);
            boundary();
            dispatch(cmd, 8, 5, 1, 1, pc);
            bufferComputeBarrier(cmd);
            boundary();
        } else if (queries) {
            for (uint32_t slot = 1; slot <= 5; ++slot) noopQuery(cmd, slot);
        }
        dispatch(cmd, 1, 6, (cfg.width + 15) / 16, (cfg.height + 15) / 16, pc);
        if (cfg.diagnosticBranchOutputs) diagnosticCopy(cmd, maskA.image, diagMaskPre.image);
        compBarrier(cmd, maskA.image);
        boundary();
        dispatch(cmd, 2, 7, (cfg.height + 63) / 64, 1, pc);
        compBarrier(cmd, maskB.image);
        boundary();
        dispatch(cmd, 3, 8, (cfg.width + 63) / 64, 1, pc);
        compBarrier(cmd, maskA.image);
        boundary();
        if (fusedVng()) {
            if (cfg.diagnosticBranchOutputs) {
                vr(vout());
                compBarrier(cmd, vngOut.image);
            }
            vbr(rout(out), maskA.view, pc.outputFactor, pc.outputAlpha);
            noopQuery(cmd, 9);
        } else {
            vr(vout());
            compBarrier(cmd, vngOut.image);
            boundary();
            dispatch(cmd, 4, 9, (cfg.width + 15) / 16, (cfg.height + 15) / 16, pc);
        }
        if (cfg.autoContrast) bufferHostBarrier(cmd);
        last.peakAllocatedBytes = peak + rcdPipe->peakAllocatedBytes() + vngPipe->peakAllocatedBytes();
        last.liveAllocatedBytes = live + rcdPipe->currentAllocatedBytes() + vngPipe->currentAllocatedBytes();
        last.bufferAllocationsTotal = allocs;
    }
    void raw(VkCommandBuffer c, const RawCfaImageView& i, const LinearRgbImage& o) {
        if (i.width != cfg.width || i.height != cfg.height || i.pattern != cfg.pattern)
            throw std::runtime_error("dual RAW mismatch");
        rcd::RawCfaImageView ri{
            i.image,     i.view,   i.format,      i.layout,
            i.width,     i.height, rp(i.pattern), {i.blackLevel[0], i.blackLevel[1], i.blackLevel[2], i.blackLevel[3]},
            i.whiteLevel};
        vng4::RawCfaImageView vi{
            i.image,     i.view,   i.format,      i.layout,
            i.width,     i.height, vp(i.pattern), {i.blackLevel[0], i.blackLevel[1], i.blackLevel[2], i.blackLevel[3]},
            i.whiteLevel};
        run(
            c, o, [&](const rcd::LinearRgbImage& x) { rcdPipe->record(c, ri, x); },
            [&](const vng4::LinearRgbImage& x) { vngPipe->record(c, vi, x); },
            [&](const rcd::LinearRgbImage& x, VkImageView m, float f, float a) {
                vng4::LinearRgbImage y{x.image, x.view, x.format, x.layout, x.width, x.height};
                vngPipe->recordBlended(c, vi, y, m, f, a);
            });
    }
    void norm(VkCommandBuffer c, const NormalizedBayerBufferView& i, const LinearRgbImage& o) {
        rcd::NormalizedBayerBufferView ri{
            {i.buffer.buffer, i.buffer.offset, i.buffer.range}, i.width, i.height, rp(i.pattern)};
        vng4::NormalizedBayerBufferView vi{
            {i.buffer.buffer, i.buffer.offset, i.buffer.range}, i.width, i.height, vp(i.pattern)};
        run(
            c, o, [&](const rcd::LinearRgbImage& x) { rcdPipe->record(c, ri, x); },
            [&](const vng4::LinearRgbImage& x) { vngPipe->record(c, vi, x); },
            [&](const rcd::LinearRgbImage& x, VkImageView m, float f, float a) {
                vng4::LinearRgbImage y{x.image, x.view, x.format, x.layout, x.width, x.height};
                vngPipe->recordBlended(c, vi, y, m, f, a);
            });
    }
    void packed(VkCommandBuffer c, const PackedCfaImageView& i, const LinearRgbImage& o) {
        rcd::PackedCfaImageView ri{i.image,  i.view,     i.format,    i.layout,     i.width,
                                   i.height, i.rawWidth, i.rawHeight, rp(i.pattern)};
        vng4::PackedCfaImageView vi{i.image,  i.view,     i.format,    i.layout,     i.width,
                                    i.height, i.rawWidth, i.rawHeight, vp(i.pattern)};
        run(
            c, o, [&](const rcd::LinearRgbImage& x) { rcdPipe->record(c, ri, x); },
            [&](const vng4::LinearRgbImage& x) { vngPipe->record(c, vi, x); },
            [&](const rcd::LinearRgbImage& x, VkImageView m, float f, float a) {
                vng4::LinearRgbImage y{x.image, x.view, x.format, x.layout, x.width, x.height};
                vngPipe->recordBlended(c, vi, y, m, f, a);
            });
    }
    bool telemetry(FrameTelemetry& o) {
        o = last;
        o.gpuEvents.clear();
        rcd::FrameTelemetry rt{};
        if (!rcdPipe->collectTelemetry(rt)) return false;
        for (const auto& e : rt.gpuEvents) o.gpuEvents.push_back({e.name, e.milliseconds});
        if (cfg.autoContrast || cfg.contrastPercent > 0.0f) {
            vng4::FrameTelemetry vt{};
            if (!vngPipe->collectTelemetry(vt)) return false;
            for (const auto& e : vt.gpuEvents) o.gpuEvents.push_back({e.name, e.milliseconds});
        }
        if (!queries || (!cfg.autoContrast && cfg.contrastPercent == 0.0f)) return true;
        uint64_t maskBits = timestampBits >= 64 ? ~0ull : ((1ull << timestampBits) - 1);
        auto d = [&](uint64_t a, uint64_t b) { return (b - a) & maskBits; };
        const char* n[10] = {"dual.luminance",    "dual.auto_tiles80",
                             "dual.auto_tiles40", "dual.auto_select40",
                             "dual.auto_refine",  "dual.auto_threshold",
                             "dual.mask",         "dual.gauss_h",
                             "dual.gauss_v",      fusedVng() ? "dual.blend_fused_into_vng_export" : "dual.blend"};
        std::array<uint64_t, 20> t{};
        if (vkGetQueryPoolResults(ctx.device, queries, 0, 20, sizeof(t), t.data(), sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS)
            return false;
        for (int i = 0; i < 10; ++i) {
            if (!cfg.autoContrast && i >= 1 && i <= 5) continue;
            o.gpuEvents.push_back({n[i], double(d(t[i * 2], t[i * 2 + 1])) * timestampNs * 1e-6});
        }
        last = o;
        return true;
    }
};
bool DualDemosaicPipeline::validateConfig(const PipelineConfig& c, const PipelineAssets&,
                                          const char** reason) noexcept {
    const char* w = nullptr;
    if (c.width < 7 || c.height < 7)
        w = "dual requires width,height >= 7";
    else if (c.autoContrast && (c.width < 80 || c.height < 80))
        w = "dual autoContrast requires width,height >= 80";
    else if (c.inputMode == InputMode::PackedCfaRgba16fImage && ((c.width & 1u) || (c.height & 1u)))
        w = "packed CFA requires even dimensions";
    else if (!std::isfinite(c.outputScale) || !std::isfinite(c.outputAlpha) || !std::isfinite(c.contrastPercent))
        w = "scale/alpha/contrast must be finite";
    else if (c.contrastPercent < kRawTherapeeDualContrastMinPercent ||
             c.contrastPercent > kRawTherapeeDualContrastMaxPercent)
        w = "contrastPercent must be in RawTherapee range [0,100]";
    else if (static_cast<uint32_t>(c.optimizationMode) > static_cast<uint32_t>(OptimizationMode::VngExportBlend))
        w = "unknown optimizationMode";
    else if (c.pixelWorkgroupX != 16 || c.pixelWorkgroupY != 16)
        w = "dual pixel workgroup is compiled as 16x16";
    if (reason) *reason = w;
    return !w;
}
DualDemosaicPipeline::DualDemosaicPipeline(VulkanContext c, ShaderProvider s, PipelineConfig p, PipelineAssets a)
    : impl_(std::make_unique<Impl>(c, std::move(s), p, a)) {}
DualDemosaicPipeline::~DualDemosaicPipeline() = default;
void DualDemosaicPipeline::record(VkCommandBuffer c, const RawCfaImageView& i, const LinearRgbImage& o) {
    impl_->raw(c, i, o);
}
void DualDemosaicPipeline::record(VkCommandBuffer c, const NormalizedBayerBufferView& i, const LinearRgbImage& o) {
    impl_->norm(c, i, o);
}
void DualDemosaicPipeline::record(VkCommandBuffer c, const PackedCfaImageView& i, const LinearRgbImage& o) {
    impl_->packed(c, i, o);
}
void DualDemosaicPipeline::setPassBoundary(std::function<void()> boundary) {
    impl_->rcdPipe->setPassBoundary(boundary);
    impl_->vngPipe->setPassBoundary(boundary);
    impl_->passBoundary = std::move(boundary);
}
const PipelineConfig& DualDemosaicPipeline::config() const { return impl_->cfg; }
void DualDemosaicPipeline::setContrastPercent(float v) {
    if (!std::isfinite(v) || v < kRawTherapeeDualContrastMinPercent || v > kRawTherapeeDualContrastMaxPercent)
        throw std::invalid_argument("dual contrastPercent must be finite and in [0,100]");
    impl_->cfg.contrastPercent = v;
}
float DualDemosaicPipeline::contrastPercent() const { return impl_->cfg.contrastPercent; }
void DualDemosaicPipeline::setAutoContrast(bool v) {
    if (v && (impl_->cfg.width < 80 || impl_->cfg.height < 80))
        throw std::invalid_argument("dual autoContrast requires width,height >= 80");
    impl_->cfg.autoContrast = v;
}
bool DualDemosaicPipeline::autoContrast() const { return impl_->cfg.autoContrast; }
float DualDemosaicPipeline::resolvedContrastPercent() const {
    if (!impl_->cfg.autoContrast) return impl_->cfg.contrastPercent;
    const auto* state = static_cast<const AutoState*>(impl_->autoState.mapped);
    return state ? state->resolvedThreshold * 100.0f : 0.0f;
}
uint32_t DualDemosaicPipeline::autoDetectionTileSize() const {
    if (!impl_->cfg.autoContrast) return 0u;
    const auto* state = static_cast<const AutoState*>(impl_->autoState.mapped);
    return state ? state->tileSize : 0u;
}
bool DualDemosaicPipeline::collectTelemetry(FrameTelemetry& o) { return impl_->telemetry(o); }
uint64_t DualDemosaicPipeline::currentAllocatedBytes() const {
    return impl_->live + impl_->rcdPipe->currentAllocatedBytes() + impl_->vngPipe->currentAllocatedBytes();
}
uint64_t DualDemosaicPipeline::peakAllocatedBytes() const {
    return impl_->peak + impl_->rcdPipe->peakAllocatedBytes() + impl_->vngPipe->peakAllocatedBytes();
}
VkImage DualDemosaicPipeline::diagnosticRcdImage() const noexcept {
    return impl_ ? impl_->diagRcd.image : VK_NULL_HANDLE;
}
VkImage DualDemosaicPipeline::diagnosticVngImage() const noexcept {
    return (impl_ && impl_->cfg.diagnosticBranchOutputs) ? impl_->vngOut.image : VK_NULL_HANDLE;
}
VkImage DualDemosaicPipeline::diagnosticPreBlurMaskImage() const noexcept {
    return impl_ ? impl_->diagMaskPre.image : VK_NULL_HANDLE;
}
VkImage DualDemosaicPipeline::diagnosticBlendMaskImage() const noexcept {
    return impl_ ? impl_->maskA.image : VK_NULL_HANDLE;
}

}  // namespace dual
