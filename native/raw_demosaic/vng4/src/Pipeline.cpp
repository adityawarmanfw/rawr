#include "vng4/Pipeline.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
namespace vng4 {
namespace {
void check(VkResult r, const char* w) {
    if (r != VK_SUCCESS) throw std::runtime_error(w);
}
uint32_t memType(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties m{};
    vkGetPhysicalDeviceMemoryProperties(pd, &m);
    for (uint32_t i = 0; i < m.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (m.memoryTypes[i].propertyFlags & flags) == flags) return i;
    throw std::runtime_error("VNG4: no compatible memory type");
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
    uint64_t bytes = 0;
};
}  // namespace
struct Vng4Pipeline::Impl {
    VulkanContext ctx{};
    ShaderProvider shaders;
    PipelineConfig cfg{};
    std::function<void()> passBoundary;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    // The blended export has its own set so a plain record and a blended record can
    // share one command buffer (Dual's diagnostic branch outputs do).
    VkDescriptorSet blendSet = VK_NULL_HANDLE;
    std::array<VkShaderModule, 5> modules{};
    std::array<VkPipeline, 5> pipelines{};
    VkSampler sampler = VK_NULL_HANDLE;
    VkQueryPool queries = VK_NULL_HANDLE;
    float timestampNs = 0;
    uint32_t timestampBits = 0;
    Img working4{}, green{}, dummyR16{}, dummyR32{}, dummyPacked{};
    Buf dummyBuf{};
    bool internalInit = false;
    uint64_t live = 0, peak = 0, allocs = 0;
    FrameTelemetry last{};
    struct Push {
        uint32_t width, height, pattern, inputMode;
        float black[4];
        float invRange[4];
        float outputFactor;
        float outputAlpha;
    };
    static_assert(sizeof(Push) == 56);
    Impl(VulkanContext c, ShaderProvider s, PipelineConfig p, PipelineAssets) : ctx(c), shaders(std::move(s)), cfg(p) {
        const char* why = nullptr;
        if (!Vng4Pipeline::validateConfig(cfg, {}, &why))
            throw std::runtime_error(std::string("VNG4 invalid config: ") + (why ? why : "unknown"));
        if (!ctx.device || !ctx.physicalDevice) throw std::runtime_error("VNG4 requires Vulkan device");
        if (!shaders) throw std::runtime_error("VNG4 ShaderProvider empty");
        try {
            create();
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
    Img makeImg(uint32_t w, uint32_t h, VkFormat f, VkImageUsageFlags u) {
        Img o;
        VkImageCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = f;
        ci.extent = {w, h, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = u;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateImage(ctx.device, &ci, ctx.allocator, &o.image), "VNG4 vkCreateImage");
        VkMemoryRequirements mr{};
        vkGetImageMemoryRequirements(ctx.device, o.image, &mr);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = memType(ctx.physicalDevice, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(ctx.device, &ai, ctx.allocator, &o.memory), "VNG4 vkAllocateMemory");
        check(vkBindImageMemory(ctx.device, o.image, o.memory, 0), "VNG4 vkBindImageMemory");
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = o.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = f;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        check(vkCreateImageView(ctx.device, &vi, ctx.allocator, &o.view), "VNG4 vkCreateImageView");
        o.bytes = mr.size;
        add(o.bytes);
        return o;
    }
    Buf makeBuf(VkDeviceSize n) {
        Buf o;
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = n;
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(ctx.device, &bi, ctx.allocator, &o.buffer), "VNG4 vkCreateBuffer");
        VkMemoryRequirements mr{};
        vkGetBufferMemoryRequirements(ctx.device, o.buffer, &mr);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = memType(ctx.physicalDevice, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(ctx.device, &ai, ctx.allocator, &o.memory), "VNG4 vkAllocateMemory(buffer)");
        check(vkBindBufferMemory(ctx.device, o.buffer, o.memory, 0), "VNG4 vkBindBufferMemory");
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
    void destroy(Buf& o) {
        if (o.buffer) vkDestroyBuffer(ctx.device, o.buffer, ctx.allocator);
        if (o.memory) vkFreeMemory(ctx.device, o.memory, ctx.allocator);
        if (o.bytes) sub(o.bytes);
        o = {};
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
            throw std::runtime_error("VNG4 timestamps unsupported");
        timestampBits = q[ctx.queueFamilyIndex].timestampValidBits;
        VkQueryPoolCreateInfo qi{};
        qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 6;
        check(vkCreateQueryPool(ctx.device, &qi, ctx.allocator, &queries), "VNG4 query pool");
    }
    void create() {
        working4 = makeImg(cfg.width, cfg.height, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT);
        green = makeImg(cfg.width, cfg.height, VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT);
        dummyR16 = makeImg(1, 1, VK_FORMAT_R16_UINT, VK_IMAGE_USAGE_STORAGE_BIT);
        dummyR32 = makeImg(1, 1, VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT);
        dummyPacked = makeImg(1, 1, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT);
        dummyBuf = makeBuf(sizeof(float));
        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = si.minFilter = VK_FILTER_NEAREST;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        check(vkCreateSampler(ctx.device, &si, ctx.allocator, &sampler), "VNG4 sampler");
        std::array<VkDescriptorSetLayoutBinding, 9> b{};
        b[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        for (uint32_t i = 1; i <= 2; ++i)
            b[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        b[3] = {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        for (uint32_t i = 4; i <= 6; ++i)
            b[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        b[7] = {7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        b[8] = {8, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo sl{};
        sl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        sl.bindingCount = b.size();
        sl.pBindings = b.data();
        check(vkCreateDescriptorSetLayout(ctx.device, &sl, ctx.allocator, &setLayout), "VNG4 set layout");
        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
        VkPipelineLayoutCreateInfo pl{};
        pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &setLayout;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &pr;
        check(vkCreatePipelineLayout(ctx.device, &pl, ctx.allocator, &pipelineLayout), "VNG4 pipeline layout");
        std::array<VkDescriptorPoolSize, 3> ps{{{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4},
                                                {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 12},
                                                {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2}}};
        VkDescriptorPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pi.maxSets = 2;
        pi.poolSizeCount = ps.size();
        pi.pPoolSizes = ps.data();
        check(vkCreateDescriptorPool(ctx.device, &pi, ctx.allocator, &pool), "VNG4 descriptor pool");
        VkDescriptorSetAllocateInfo da{};
        da.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        da.descriptorPool = pool;
        da.descriptorSetCount = 1;
        da.pSetLayouts = &setLayout;
        check(vkAllocateDescriptorSets(ctx.device, &da, &set), "VNG4 descriptor set");
        check(vkAllocateDescriptorSets(ctx.device, &da, &blendSet), "VNG4 blend descriptor set");
        const char* names[5] = {"vng4_linear.comp", "vng4_green.comp", "vng4_export.comp", "vng4_green_diagnostic.comp",
                                "vng4_export_blend.comp"};
        constexpr size_t kDiagnosticShader = 3;
        for (size_t i = 0; i < 5; ++i) {
            std::vector<uint32_t> w;
            if (i == kDiagnosticShader) {
                // Optional: production builds omit the single-pixel diagnostic;
                // recordGreenDiagnostic() then reports that it is unavailable.
                try {
                    w = shaders(names[i]);
                } catch (const std::exception&) {
                }
                if (w.empty()) continue;
            } else {
                w = shaders(names[i]);
                if (w.empty()) throw std::runtime_error(std::string("VNG4 empty SPIR-V: ") + names[i]);
            }
            VkShaderModuleCreateInfo sm{};
            sm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            sm.codeSize = w.size() * 4;
            sm.pCode = w.data();
            check(vkCreateShaderModule(ctx.device, &sm, ctx.allocator, &modules[i]), "VNG4 shader module");
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
                  "VNG4 compute pipeline");
        }
    }
    void cleanup() noexcept {
        if (!ctx.device) return;
        for (auto& p : pipelines)
            if (p) vkDestroyPipeline(ctx.device, p, ctx.allocator);
        for (auto& m : modules)
            if (m) vkDestroyShaderModule(ctx.device, m, ctx.allocator);
        if (pool) vkDestroyDescriptorPool(ctx.device, pool, ctx.allocator);
        if (pipelineLayout) vkDestroyPipelineLayout(ctx.device, pipelineLayout, ctx.allocator);
        if (setLayout) vkDestroyDescriptorSetLayout(ctx.device, setLayout, ctx.allocator);
        if (queries) vkDestroyQueryPool(ctx.device, queries, ctx.allocator);
        if (sampler) vkDestroySampler(ctx.device, sampler, ctx.allocator);
        destroy(dummyBuf);
        destroy(dummyPacked);
        destroy(dummyR32);
        destroy(dummyR16);
        destroy(green);
        destroy(working4);
    }
    static VkImageMemoryBarrier barrier(VkImage im, VkAccessFlags s, VkAccessFlags d, VkImageLayout oldL,
                                        VkImageLayout newL) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = s;
        b.dstAccessMask = d;
        b.oldLayout = oldL;
        b.newLayout = newL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = im;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        return b;
    }
    void init(VkCommandBuffer cmd) {
        if (internalInit) return;
        std::array<VkImageMemoryBarrier, 5> bs{
            {barrier(working4.image, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL),
             barrier(green.image, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL),
             barrier(dummyR16.image, 0, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL),
             barrier(dummyR32.image, 0, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL),
             barrier(dummyPacked.image, 0, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_GENERAL)}};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, bs.size(), bs.data());
        internalInit = true;
    }
    void compBarrier(VkCommandBuffer cmd, VkImage im) {
        auto b = barrier(im, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                         VK_IMAGE_LAYOUT_GENERAL);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &b);
    }
    void validateOut(const LinearRgbImage& o) {
        if (!o.image || !o.view) throw std::runtime_error("VNG4 output null");
        if (o.width != cfg.width || o.height != cfg.height) throw std::runtime_error("VNG4 output dimensions mismatch");
        if (o.format != VK_FORMAT_R16G16B16A16_SFLOAT || o.layout != VK_IMAGE_LAYOUT_GENERAL)
            throw std::runtime_error("VNG4 output must be RGBA16F GENERAL");
    }
    void descriptors(VkBuffer nb, VkDeviceSize nr, VkImageView r16, VkImageView r32, VkImageView packed,
                     VkImageLayout packedLayout, VkImageView out, VkBuffer diag = VK_NULL_HANDLE,
                     VkDeviceSize diagRange = 0, VkImageView blendMask = VK_NULL_HANDLE) {
        VkDescriptorBufferInfo bi{nb ? nb : dummyBuf.buffer, 0, nb ? nr : sizeof(float)};
        VkDescriptorImageInfo i1{VK_NULL_HANDLE, r16 ? r16 : dummyR16.view, VK_IMAGE_LAYOUT_GENERAL},
            i2{VK_NULL_HANDLE, r32 ? r32 : dummyR32.view, VK_IMAGE_LAYOUT_GENERAL},
            i3{sampler, packed ? packed : dummyPacked.view, packed ? packedLayout : VK_IMAGE_LAYOUT_GENERAL},
            i4{VK_NULL_HANDLE, working4.view, VK_IMAGE_LAYOUT_GENERAL},
            i5{VK_NULL_HANDLE, green.view, VK_IMAGE_LAYOUT_GENERAL}, i6{VK_NULL_HANDLE, out, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorBufferInfo dbi{diag ? diag : dummyBuf.buffer, 0, diag ? diagRange : sizeof(float)};
        VkDescriptorImageInfo bmi{VK_NULL_HANDLE, blendMask ? blendMask : dummyR32.view, VK_IMAGE_LAYOUT_GENERAL};
        std::array<VkWriteDescriptorSet, 9> w{};
        for (uint32_t i = 0; i < 9; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = blendMask ? blendSet : set;
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
        }
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[0].pBufferInfo = &bi;
        VkDescriptorImageInfo* infos[] = {&i1, &i2, &i3, &i4, &i5, &i6};
        for (uint32_t i = 1; i < 7; ++i) {
            w[i].descriptorType =
                (i == 3) ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w[i].pImageInfo = infos[i - 1];
        }
        w[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[7].pBufferInfo = &dbi;
        w[8].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[8].pImageInfo = &bmi;
        vkUpdateDescriptorSets(ctx.device, w.size(), w.data(), 0, nullptr);
    }
    Push push(InputMode m, const float* b, float white) {
        Push p{};
        p.width = cfg.width;
        p.height = cfg.height;
        p.pattern = uint32_t(cfg.pattern);
        p.inputMode = uint32_t(m);
        for (int i = 0; i < 4; ++i) {
            p.black[i] = b ? b[i] : 0;
            p.invRange[i] = b ? 1.f / (white - b[i]) : 1.f;
        }
        p.outputFactor = cfg.outputScale * 255.f;
        p.outputAlpha = cfg.outputAlpha;
        return p;
    }
    void dispatchAll(VkCommandBuffer cmd, const Push& pc, bool blendedExport = false) {
        init(cmd);
        // working4/green may still be in use by an earlier record in this command buffer.
        VkMemoryBarrier mb{};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb,
                             0, nullptr, 0, nullptr);
        uint32_t wx[3] = {cfg.linearWorkgroupX, cfg.greenWorkgroupX, cfg.exportWorkgroupX},
                 wy[3] = {cfg.linearWorkgroupY, cfg.greenWorkgroupY, cfg.exportWorkgroupY};
        if (queries) vkCmdResetQueryPool(cmd, queries, 0, 6);
        for (uint32_t i = 0; i < 3; ++i) {
            if (queries) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, queries, i * 2);
            const uint32_t pipeIndex = (i == 2 && blendedExport) ? 4u : i;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[pipeIndex]);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1,
                                    blendedExport ? &blendSet : &set, 0, nullptr);
            vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push), &pc);
            vkCmdDispatch(cmd, (cfg.width + wx[i] - 1) / wx[i], (cfg.height + wy[i] - 1) / wy[i], 1);
            if (queries) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, queries, i * 2 + 1);
            if (i == 0)
                compBarrier(cmd, working4.image);
            else if (i == 1)
                compBarrier(cmd, green.image);
            if (i < 2 && passBoundary) passBoundary();
        }
        last = {};
        last.peakAllocatedBytes = peak;
        last.liveAllocatedBytes = live;
        last.bufferAllocationsTotal = allocs;
    }
    bool telemetry(FrameTelemetry& o) {
        o = last;
        if (!queries) return true;
        std::array<uint64_t, 6> t{};
        auto r = vkGetQueryPoolResults(ctx.device, queries, 0, 6, sizeof(t), t.data(), sizeof(uint64_t),
                                       VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        if (r != VK_SUCCESS) return false;
        uint64_t mask = timestampBits >= 64 ? ~0ull : ((1ull << timestampBits) - 1);
        auto d = [&](uint64_t a, uint64_t b) { return (b - a) & mask; };
        const char* n[3] = {"vng4.linear", "vng4.green", "vng4.export"};
        o.gpuEvents.clear();
        for (int i = 0; i < 3; ++i)
            o.gpuEvents.push_back({n[i], double(d(t[i * 2], t[i * 2 + 1])) * timestampNs * 1e-6});
        o.gpuEvents.push_back({"vng4.total", double(d(t[0], t[5])) * timestampNs * 1e-6});
        last = o;
        return true;
    }
    void diag(VkCommandBuffer cmd, uint32_t x, uint32_t y, VkBuffer b, VkDeviceSize range) {
        (void)x;
        (void)y;
        if (!pipelines[3]) throw std::runtime_error("VNG4 green diagnostic shader was not provided");
        if (!b || range < 19 * sizeof(uint32_t)) throw std::runtime_error("VNG4 diagnostic buffer too small");
        // Preserve bindings 0..6 from the production record. The diagnostic reuses the
        // exact same input and intermediate images; only binding 7 is diagnostic output.
        VkDescriptorBufferInfo dbi{b, 0, range};
        VkWriteDescriptorSet dw{};
        dw.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        dw.dstSet = set;
        dw.dstBinding = 7;
        dw.descriptorCount = 1;
        dw.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        dw.pBufferInfo = &dbi;
        vkUpdateDescriptorSets(ctx.device, 1, &dw, 0, nullptr);
        VkMemoryBarrier mb{};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb,
                             0, nullptr, 0, nullptr);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[3]);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 0, nullptr);
        auto pc = push(cfg.inputMode, nullptr, 1);
        vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push), &pc);
        vkCmdDispatch(cmd, 1, 1, 1);
    }
    void raw(VkCommandBuffer cmd, const RawCfaImageView& in, const LinearRgbImage& out) {
        if (cfg.inputMode != InputMode::RawR16UintImage && cfg.inputMode != InputMode::RawR32FloatImage)
            throw std::runtime_error("VNG4 wrong input mode");
        if (in.width != cfg.width || in.height != cfg.height || in.pattern != cfg.pattern)
            throw std::runtime_error("VNG4 RAW mismatch");
        validateOut(out);
        VkFormat exp = cfg.inputMode == InputMode::RawR16UintImage ? VK_FORMAT_R16_UINT : VK_FORMAT_R32_SFLOAT;
        if (in.format != exp || in.layout != VK_IMAGE_LAYOUT_GENERAL)
            throw std::runtime_error("VNG4 RAW format/layout mismatch");
        for (float b : in.blackLevel)
            if (!std::isfinite(b) || !std::isfinite(in.whiteLevel) || !(in.whiteLevel > b))
                throw std::runtime_error("VNG4 invalid black/white");
        descriptors(VK_NULL_HANDLE, 0, cfg.inputMode == InputMode::RawR16UintImage ? in.view : VK_NULL_HANDLE,
                    cfg.inputMode == InputMode::RawR32FloatImage ? in.view : VK_NULL_HANDLE, VK_NULL_HANDLE,
                    VK_IMAGE_LAYOUT_GENERAL, out.view);
        dispatchAll(cmd, push(cfg.inputMode, in.blackLevel, in.whiteLevel));
    }
    void norm(VkCommandBuffer cmd, const NormalizedBayerBufferView& in, const LinearRgbImage& out) {
        if (cfg.inputMode != InputMode::NormalizedFloatBuffer) throw std::runtime_error("VNG4 wrong normalized mode");
        if (in.width != cfg.width || in.height != cfg.height || in.pattern != cfg.pattern || !in.buffer.buffer ||
            in.buffer.offset != 0)
            throw std::runtime_error("VNG4 normalized mismatch");
        validateOut(out);
        VkDeviceSize need = VkDeviceSize(size_t(cfg.width) * cfg.height * sizeof(float)),
                     range = in.buffer.range == VK_WHOLE_SIZE ? need : in.buffer.range;
        if (range < need) throw std::runtime_error("VNG4 normalized buffer too small");
        descriptors(in.buffer.buffer, range, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
                    out.view);
        dispatchAll(cmd, push(cfg.inputMode, nullptr, 1));
    }
    void packed(VkCommandBuffer cmd, const PackedCfaImageView& in, const LinearRgbImage& out) {
        if (cfg.inputMode != InputMode::PackedCfaRgba16fImage) throw std::runtime_error("VNG4 wrong packed mode");
        if ((cfg.width & 1u) || (cfg.height & 1u) || in.rawWidth != cfg.width || in.rawHeight != cfg.height ||
            in.width != cfg.width / 2 || in.height != cfg.height / 2 || in.pattern != cfg.pattern ||
            in.format != VK_FORMAT_R16G16B16A16_SFLOAT || in.layout != VK_IMAGE_LAYOUT_GENERAL)
            throw std::runtime_error("VNG4 packed mismatch");
        validateOut(out);
        descriptors(VK_NULL_HANDLE, 0, VK_NULL_HANDLE, VK_NULL_HANDLE, in.view, in.layout, out.view);
        dispatchAll(cmd, push(cfg.inputMode, nullptr, 1));
    }
    void rawBlended(VkCommandBuffer cmd, const RawCfaImageView& in, const LinearRgbImage& out, VkImageView mask,
                    float factor, float alpha) {
        if (!mask) throw std::runtime_error("VNG4 blend mask null");
        if (cfg.inputMode != InputMode::RawR16UintImage && cfg.inputMode != InputMode::RawR32FloatImage)
            throw std::runtime_error("VNG4 wrong input mode");
        if (in.width != cfg.width || in.height != cfg.height || in.pattern != cfg.pattern)
            throw std::runtime_error("VNG4 RAW mismatch");
        validateOut(out);
        VkFormat exp = cfg.inputMode == InputMode::RawR16UintImage ? VK_FORMAT_R16_UINT : VK_FORMAT_R32_SFLOAT;
        if (in.format != exp || in.layout != VK_IMAGE_LAYOUT_GENERAL)
            throw std::runtime_error("VNG4 RAW format/layout mismatch");
        for (float b : in.blackLevel)
            if (!std::isfinite(b) || !std::isfinite(in.whiteLevel) || !(in.whiteLevel > b))
                throw std::runtime_error("VNG4 invalid black/white");
        descriptors(VK_NULL_HANDLE, 0, cfg.inputMode == InputMode::RawR16UintImage ? in.view : VK_NULL_HANDLE,
                    cfg.inputMode == InputMode::RawR32FloatImage ? in.view : VK_NULL_HANDLE, VK_NULL_HANDLE,
                    VK_IMAGE_LAYOUT_GENERAL, out.view, VK_NULL_HANDLE, 0, mask);
        auto pc = push(cfg.inputMode, in.blackLevel, in.whiteLevel);
        pc.outputFactor = factor;
        pc.outputAlpha = alpha;
        dispatchAll(cmd, pc, true);
    }
    void normBlended(VkCommandBuffer cmd, const NormalizedBayerBufferView& in, const LinearRgbImage& out,
                     VkImageView mask, float factor, float alpha) {
        if (!mask) throw std::runtime_error("VNG4 blend mask null");
        if (cfg.inputMode != InputMode::NormalizedFloatBuffer) throw std::runtime_error("VNG4 wrong normalized mode");
        if (in.width != cfg.width || in.height != cfg.height || in.pattern != cfg.pattern || !in.buffer.buffer ||
            in.buffer.offset != 0)
            throw std::runtime_error("VNG4 normalized mismatch");
        validateOut(out);
        VkDeviceSize need = VkDeviceSize(size_t(cfg.width) * cfg.height * sizeof(float)),
                     range = in.buffer.range == VK_WHOLE_SIZE ? need : in.buffer.range;
        if (range < need) throw std::runtime_error("VNG4 normalized buffer too small");
        descriptors(in.buffer.buffer, range, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
                    out.view, VK_NULL_HANDLE, 0, mask);
        auto pc = push(cfg.inputMode, nullptr, 1);
        pc.outputFactor = factor;
        pc.outputAlpha = alpha;
        dispatchAll(cmd, pc, true);
    }
    void packedBlended(VkCommandBuffer cmd, const PackedCfaImageView& in, const LinearRgbImage& out, VkImageView mask,
                       float factor, float alpha) {
        if (!mask) throw std::runtime_error("VNG4 blend mask null");
        if (cfg.inputMode != InputMode::PackedCfaRgba16fImage) throw std::runtime_error("VNG4 wrong packed mode");
        if ((cfg.width & 1u) || (cfg.height & 1u) || in.rawWidth != cfg.width || in.rawHeight != cfg.height ||
            in.width != cfg.width / 2 || in.height != cfg.height / 2 || in.pattern != cfg.pattern ||
            in.format != VK_FORMAT_R16G16B16A16_SFLOAT || in.layout != VK_IMAGE_LAYOUT_GENERAL)
            throw std::runtime_error("VNG4 packed mismatch");
        validateOut(out);
        descriptors(VK_NULL_HANDLE, 0, VK_NULL_HANDLE, VK_NULL_HANDLE, in.view, in.layout, out.view, VK_NULL_HANDLE, 0,
                    mask);
        auto pc = push(cfg.inputMode, nullptr, 1);
        pc.outputFactor = factor;
        pc.outputAlpha = alpha;
        dispatchAll(cmd, pc, true);
    }
};
bool Vng4Pipeline::validateConfig(const PipelineConfig& c, const PipelineAssets&, const char** reason) noexcept {
    const char* w = nullptr;
    if (c.width < 7 || c.height < 7)
        w = "VNG4 requires width,height >= 7";
    else if (c.inputMode == InputMode::PackedCfaRgba16fImage && ((c.width & 1) || (c.height & 1)))
        w = "packed CFA requires even dimensions";
    else if (!std::isfinite(c.outputScale) || !std::isfinite(c.outputAlpha))
        w = "output scale/alpha must be finite";
    else if (!c.linearWorkgroupX || !c.linearWorkgroupY || !c.greenWorkgroupX || !c.greenWorkgroupY ||
             !c.exportWorkgroupX || !c.exportWorkgroupY)
        w = "workgroups must be nonzero";
    if (reason) *reason = w;
    return !w;
}
Vng4Pipeline::Vng4Pipeline(VulkanContext c, ShaderProvider s, PipelineConfig p, PipelineAssets a)
    : impl_(std::make_unique<Impl>(c, std::move(s), p, a)) {}
Vng4Pipeline::~Vng4Pipeline() = default;
void Vng4Pipeline::record(VkCommandBuffer c, const RawCfaImageView& i, const LinearRgbImage& o) { impl_->raw(c, i, o); }
void Vng4Pipeline::record(VkCommandBuffer c, const NormalizedBayerBufferView& i, const LinearRgbImage& o) {
    impl_->norm(c, i, o);
}
void Vng4Pipeline::record(VkCommandBuffer c, const PackedCfaImageView& i, const LinearRgbImage& o) {
    impl_->packed(c, i, o);
}
void Vng4Pipeline::recordGreenDiagnostic(VkCommandBuffer c, uint32_t x, uint32_t y, VkBuffer b, VkDeviceSize r) {
    impl_->diag(c, x, y, b, r);
}
void Vng4Pipeline::recordBlended(VkCommandBuffer c, const RawCfaImageView& i, const LinearRgbImage& o, VkImageView m,
                                 float f, float a) {
    impl_->rawBlended(c, i, o, m, f, a);
}
void Vng4Pipeline::recordBlended(VkCommandBuffer c, const NormalizedBayerBufferView& i, const LinearRgbImage& o,
                                 VkImageView m, float f, float a) {
    impl_->normBlended(c, i, o, m, f, a);
}
void Vng4Pipeline::recordBlended(VkCommandBuffer c, const PackedCfaImageView& i, const LinearRgbImage& o, VkImageView m,
                                 float f, float a) {
    impl_->packedBlended(c, i, o, m, f, a);
}
void Vng4Pipeline::setPassBoundary(std::function<void()> boundary) { impl_->passBoundary = std::move(boundary); }
const PipelineConfig& Vng4Pipeline::config() const { return impl_->cfg; }
bool Vng4Pipeline::collectTelemetry(FrameTelemetry& o) { return impl_->telemetry(o); }
uint64_t Vng4Pipeline::currentAllocatedBytes() const { return impl_->live; }
uint64_t Vng4Pipeline::peakAllocatedBytes() const { return impl_->peak; }
}  // namespace vng4
