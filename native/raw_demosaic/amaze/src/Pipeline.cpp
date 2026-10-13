// SPDX-License-Identifier: GPL-3.0-or-later
// Vulkan host for the AMaZE GPU port (Emil Martinec; optimizations Ingo Weyrich).
// Provenance and credits: shaders/amaze.wgsl and ../NOTICE.md.
#include "amaze/Amaze.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace amaze {
namespace {

void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(what);
}

uint32_t memoryType(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties m{};
    vkGetPhysicalDeviceMemoryProperties(pd, &m);
    for (uint32_t i = 0; i < m.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (m.memoryTypes[i].propertyFlags & flags) == flags) return i;
    throw std::runtime_error("AMaZE: no compatible Vulkan memory type");
}

// librtprocess amaze.cc tiling: ts = 160 with a 16 px margin, stepping ts - 32.
constexpr int kTile = 160;
constexpr int kMargin = 16;
constexpr int kStep = kTile - 2 * kMargin;
constexpr uint32_t kLocal = 16;   // @workgroup_size(16,16) everywhere
// Per-tile slot sizes and scalar scratch channels; must match amaze.wgsl
// (SRC_PIX, OUT_PIX, CHANNELS) and amaze_tile.glsl.
constexpr VkDeviceSize kSrcPix = VkDeviceSize(kTile) * kTile;
constexpr VkDeviceSize kOutPix = VkDeviceSize(kStep) * kStep;
constexpr VkDeviceSize kChannels = 34;

// Recorded in this order; must match tools/build_spv.sh. Import first, export last.
constexpr std::array<const char*, 19> kPasses = {
    "amaze_import",   "amaze_gradients", "amaze_candidates", "amaze_choose",       "amaze_cddiff",
    "amaze_weights",  "amaze_nyquist",   "amaze_nyquist_vote", "amaze_area",       "amaze_hvwt_seq",
    "amaze_green",    "amaze_refine",    "amaze_diagonal",   "amaze_pmwt_seq",     "amaze_rbint",
    "amaze_correct",  "amaze_chroma",    "amaze_finish",     "amaze_export"};
// Serial-chain passes: choose runs one invocation per (row|column, parity) in
// workgroups of 64; the row-sequential refinements run as one workgroup per tile.
// Every pass is one dispatch per tile batch, with workgroup z selecting the tile.
constexpr size_t kChoosePass = 3, kHvwtPass = 9, kPmwtPass = 13;
constexpr uint32_t kChooseLocal = 64;
static_assert(kTile <= 256, "row-sequential passes use one lane per tile column");
// GLSL passes use "main"; naga keeps the WGSL entry point name.
bool isGlsl(size_t pass) { return pass == 0 || pass + 1 == kPasses.size(); }
// amaze_finish and amaze_export cover the tile's output region, the rest its source.
bool coversOutput(size_t pass) { return pass + 2 >= kPasses.size(); }

// Mirrors amaze.wgsl Params / amaze_tile.glsl TileParams.
struct TileParams {
    uint32_t srcOriginX, srcOriginY, srcWidth, srcHeight;
    uint32_t outOriginX, outOriginY, outWidth, outHeight;
    uint32_t cfa00, cfa10, cfa01, cfa11;
    float blackR, blackG0, blackG1, blackB;
    float white, wbR, wbG, wbB;
    float color[9];
    float sourceIsFloat, outputScale, outputAlpha;
    float inputMode, inputWidth, pattern, reserved;
};
static_assert(sizeof(TileParams) == 144, "AMaZE tile ABI changed");

struct Tile {
    uint32_t srcX, srcY, srcW, srcH, outX, outY, outW, outH;
};

std::vector<Tile> tileGrid(uint32_t width, uint32_t height) {
    auto axis = [](uint32_t n) {
        std::vector<std::array<uint32_t, 4>> spans;  // src start, src size, out start, out size
        for (int t = -kMargin; t + kMargin < int(n); t += kStep) {
            const int s0 = std::max(t, 0), s1 = std::min(t + kTile, int(n));
            const int o0 = std::max(t + kMargin, 0), o1 = std::min(t + kTile - kMargin, int(n));
            spans.push_back({uint32_t(s0), uint32_t(s1 - s0), uint32_t(o0), uint32_t(o1 - o0)});
        }
        return spans;
    };
    std::vector<Tile> tiles;
    for (const auto& y : axis(height))
        for (const auto& x : axis(width)) tiles.push_back({x[0], y[0], x[1], y[1], x[2], y[2], x[3], y[3]});
    return tiles;
}

// Colour (0 R, 1 G, 2 B) at (x&1, y&1) for a BayerPattern code.
std::array<uint32_t, 4> cfaColours(BayerPattern p) {
    switch (p) {
        case BayerPattern::RGGB: return {0, 1, 1, 2};
        case BayerPattern::GRBG: return {1, 0, 2, 1};
        case BayerPattern::GBRG: return {1, 2, 0, 1};
        default: return {2, 1, 1, 0};
    }
}

struct OwnedBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    uint64_t allocationBytes = 0;
};
struct OwnedImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint64_t allocationBytes = 0;
};

}  // namespace

struct AmazePipeline::Impl {
    VulkanContext ctx{};
    ShaderProvider shaders;
    PipelineConfig cfg{};
    std::function<void()> passBoundary;
    std::function<VkCommandBuffer()> commandBoundary;

    std::vector<Tile> tiles;
    uint32_t batchTiles = 0;          // tiles per dispatch (slots)
    VkDeviceSize batchParamStride = 0;  // aligned bytes per batch of TileParams

    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    std::array<VkShaderModule, kPasses.size()> modules{};
    std::array<VkPipeline, kPasses.size()> pipelines{};
    VkSampler packedSampler = VK_NULL_HANDLE;

    // Slot-strided tile buffers shared by one batch (the bulk of the footprint).
    OwnedBuffer source, result, scratch;
    VkDescriptorSet set = VK_NULL_HANDLE;
    OwnedBuffer params;
    void* paramsMapped = nullptr;
    // Binding placeholders for the input the configured mode does not read.
    OwnedImage dummyPacked;
    OwnedBuffer dummyNormalized;
    bool dummyLayoutReady = false;

    uint64_t liveBytes = 0;
    uint64_t peakBytes = 0;

    Impl(VulkanContext c, ShaderProvider sp, PipelineConfig pc) : ctx(c), shaders(std::move(sp)), cfg(pc) {
        const char* why = nullptr;
        if (!AmazePipeline::validateConfig(cfg, &why))
            throw std::runtime_error(std::string("AMaZE configuration invalid: ") + (why ? why : "unknown"));
        if (ctx.physicalDevice == VK_NULL_HANDLE || ctx.device == VK_NULL_HANDLE)
            throw std::runtime_error("AMaZE VulkanContext requires physicalDevice and device");
        if (!shaders) throw std::runtime_error("AMaZE ShaderProvider is empty");
        tiles = tileGrid(cfg.width, cfg.height);
        try {
            createResources();
        } catch (...) {
            cleanup();
            throw;
        }
    }
    ~Impl() { cleanup(); }

    OwnedBuffer makeBuffer(VkDeviceSize bytes, VkBufferUsageFlags usage, VkMemoryPropertyFlags flags) {
        OwnedBuffer o{};
        o.size = bytes;
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = bytes;
        bi.usage = usage;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(ctx.device, &bi, ctx.allocator, &o.buffer), "AMaZE vkCreateBuffer failed");
        VkMemoryRequirements mr{};
        vkGetBufferMemoryRequirements(ctx.device, o.buffer, &mr);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = memoryType(ctx.physicalDevice, mr.memoryTypeBits, flags);
        check(vkAllocateMemory(ctx.device, &ai, ctx.allocator, &o.memory), "AMaZE vkAllocateMemory failed");
        check(vkBindBufferMemory(ctx.device, o.buffer, o.memory, 0), "AMaZE vkBindBufferMemory failed");
        o.allocationBytes = mr.size;
        liveBytes += mr.size;
        peakBytes = std::max(peakBytes, liveBytes);
        return o;
    }
    OwnedImage makeDummyImage() {
        OwnedImage o{};
        VkImageCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        ci.extent = {1, 1, 1};
        ci.mipLevels = ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        check(vkCreateImage(ctx.device, &ci, ctx.allocator, &o.image), "AMaZE vkCreateImage failed");
        VkMemoryRequirements mr{};
        vkGetImageMemoryRequirements(ctx.device, o.image, &mr);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = memoryType(ctx.physicalDevice, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(ctx.device, &ai, ctx.allocator, &o.memory), "AMaZE vkAllocateMemory(image) failed");
        check(vkBindImageMemory(ctx.device, o.image, o.memory, 0), "AMaZE vkBindImageMemory failed");
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = o.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = ci.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        check(vkCreateImageView(ctx.device, &vi, ctx.allocator, &o.view), "AMaZE vkCreateImageView failed");
        o.allocationBytes = mr.size;
        liveBytes += mr.size;
        peakBytes = std::max(peakBytes, liveBytes);
        return o;
    }
    void destroy(OwnedImage& o) {
        if (o.view) vkDestroyImageView(ctx.device, o.view, ctx.allocator);
        if (o.image) vkDestroyImage(ctx.device, o.image, ctx.allocator);
        if (o.memory) vkFreeMemory(ctx.device, o.memory, ctx.allocator);
        liveBytes -= o.allocationBytes;
        o = {};
    }
    void destroy(OwnedBuffer& o) {
        if (o.buffer) vkDestroyBuffer(ctx.device, o.buffer, ctx.allocator);
        if (o.memory) vkFreeMemory(ctx.device, o.memory, ctx.allocator);
        liveBytes -= o.allocationBytes;
        o = {};
    }

    void createResources() {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(ctx.physicalDevice, &props);
        batchTiles = std::min<uint32_t>(cfg.tileSlots, uint32_t(tiles.size()));
        const VkDeviceSize align = std::max<VkDeviceSize>(props.limits.minStorageBufferOffsetAlignment, 16);
        batchParamStride = (sizeof(TileParams) * batchTiles + align - 1) / align * align;
        const VkDeviceSize batches = (tiles.size() + batchTiles - 1) / batchTiles;
        params = makeBuffer(batchParamStride * batches, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(vkMapMemory(ctx.device, params.memory, 0, VK_WHOLE_SIZE, 0, &paramsMapped), "AMaZE map params failed");

        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = si.minFilter = VK_FILTER_NEAREST;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        check(vkCreateSampler(ctx.device, &si, ctx.allocator, &packedSampler), "AMaZE vkCreateSampler failed");
        if (cfg.inputMode == InputMode::NormalizedFloatBuffer)
            dummyPacked = makeDummyImage();
        else
            dummyNormalized = makeBuffer(16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        // One layout for every pass; each module declares the subset it uses.
        std::array<VkDescriptorSetLayoutBinding, 7> b{};
        b[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};          // source
        b[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};  // batch params
        b[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};          // tile results
        b[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};          // scratch
        b[4] = {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};  // packed CFA
        b[5] = {6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};           // output
        b[6] = {7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};          // float CFA
        VkDescriptorSetLayoutCreateInfo sl{};
        sl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        sl.bindingCount = uint32_t(b.size());
        sl.pBindings = b.data();
        check(vkCreateDescriptorSetLayout(ctx.device, &sl, ctx.allocator, &setLayout), "AMaZE set layout failed");
        VkPipelineLayoutCreateInfo pl{};
        pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &setLayout;
        check(vkCreatePipelineLayout(ctx.device, &pl, ctx.allocator, &pipelineLayout), "AMaZE pipeline layout failed");

        std::array<VkDescriptorPoolSize, 4> ps{{{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4},
                                                {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1},
                                                {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
                                                {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}}};
        VkDescriptorPoolCreateInfo dpi{};
        dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpi.maxSets = 1;
        dpi.poolSizeCount = uint32_t(ps.size());
        dpi.pPoolSizes = ps.data();
        check(vkCreateDescriptorPool(ctx.device, &dpi, ctx.allocator, &descriptorPool), "AMaZE descriptor pool failed");

        ensureScratch();
        VkDescriptorSetAllocateInfo dai{};
        dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dai.descriptorPool = descriptorPool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &setLayout;
        check(vkAllocateDescriptorSets(ctx.device, &dai, &set), "AMaZE descriptor set allocation failed");

        for (size_t i = 0; i < kPasses.size(); ++i) {
            const std::string name = std::string(kPasses[i]) + (isGlsl(i) ? ".comp" : ".wgsl");
            auto words = shaders(name);
            if (words.empty()) throw std::runtime_error("AMaZE empty SPIR-V: " + name);
            VkShaderModuleCreateInfo sm{};
            sm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            sm.codeSize = words.size() * sizeof(uint32_t);
            sm.pCode = words.data();
            check(vkCreateShaderModule(ctx.device, &sm, ctx.allocator, &modules[i]), "AMaZE shader module failed");
            VkComputePipelineCreateInfo ci{};
            ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            ci.stage.module = modules[i];
            ci.stage.pName = isGlsl(i) ? "main" : kPasses[i];
            ci.layout = pipelineLayout;
            check(vkCreateComputePipelines(ctx.device, ctx.pipelineCache, 1, &ci, ctx.allocator, &pipelines[i]),
                  ("AMaZE compute pipeline failed: " + name).c_str());
        }
    }

    // Slot-strided tile buffers for one batch (the bulk of the footprint).
    void ensureScratch() {
        if (source.buffer) return;
        const VkDeviceSize n = batchTiles;
        source = makeBuffer(n * kSrcPix * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        result = makeBuffer(n * kOutPix * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        scratch = makeBuffer(n * kChannels * kSrcPix * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }
    void releaseScratch() noexcept {
        destroy(scratch);
        destroy(result);
        destroy(source);
    }

    void cleanup() noexcept {
        if (ctx.device == VK_NULL_HANDLE) return;
        for (auto& p : pipelines)
            if (p) vkDestroyPipeline(ctx.device, p, ctx.allocator), p = VK_NULL_HANDLE;
        for (auto& m : modules)
            if (m) vkDestroyShaderModule(ctx.device, m, ctx.allocator), m = VK_NULL_HANDLE;
        if (descriptorPool)
            vkDestroyDescriptorPool(ctx.device, descriptorPool, ctx.allocator), descriptorPool = VK_NULL_HANDLE;
        if (pipelineLayout)
            vkDestroyPipelineLayout(ctx.device, pipelineLayout, ctx.allocator), pipelineLayout = VK_NULL_HANDLE;
        if (setLayout) vkDestroyDescriptorSetLayout(ctx.device, setLayout, ctx.allocator), setLayout = VK_NULL_HANDLE;
        if (packedSampler) vkDestroySampler(ctx.device, packedSampler, ctx.allocator), packedSampler = VK_NULL_HANDLE;
        releaseScratch();
        set = VK_NULL_HANDLE;
        destroy(dummyPacked);
        destroy(dummyNormalized);
        if (paramsMapped) vkUnmapMemory(ctx.device, params.memory), paramsMapped = nullptr;
        destroy(params);
    }

    void writeParams(const std::array<float, 3>& wb) {
        const float inputMode = float(static_cast<uint32_t>(cfg.inputMode));
        const auto colours = cfaColours(cfg.pattern);
        for (size_t i = 0; i < tiles.size(); ++i) {
            const Tile& t = tiles[i];
            TileParams p{};
            p.srcOriginX = t.srcX, p.srcOriginY = t.srcY, p.srcWidth = t.srcW, p.srcHeight = t.srcH;
            p.outOriginX = t.outX, p.outOriginY = t.outY, p.outWidth = t.outW, p.outHeight = t.outH;
            p.cfa00 = colours[0], p.cfa10 = colours[1], p.cfa01 = colours[2], p.cfa11 = colours[3];
            p.white = 1.0f;
            p.wbR = wb[0], p.wbG = wb[1], p.wbB = wb[2];
            p.color[0] = p.color[4] = p.color[8] = 1.0f;
            p.sourceIsFloat = 1.0f;
            p.outputScale = cfg.outputScale;
            p.outputAlpha = cfg.outputAlpha;
            p.inputMode = inputMode;
            p.inputWidth = float(cfg.width);
            p.pattern = float(static_cast<uint32_t>(cfg.pattern));
            const VkDeviceSize offset = (i / batchTiles) * batchParamStride + (i % batchTiles) * sizeof(TileParams);
            std::memcpy(static_cast<char*>(paramsMapped) + offset, &p, sizeof(p));
        }
    }

    // packedView == VK_NULL_HANDLE selects the float buffer input.
    void bindBatch(VkImageView packedView, VkImageLayout packedLayout, const BufferView& floatInput,
                   const LinearRgbImage& out) {
        VkDescriptorBufferInfo src{source.buffer, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo prm{params.buffer, 0, sizeof(TileParams) * batchTiles};
        VkDescriptorBufferInfo res{result.buffer, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo scr{scratch.buffer, 0, VK_WHOLE_SIZE};
        VkDescriptorImageInfo packed{packedSampler, packedView ? packedView : dummyPacked.view,
                                     packedView ? packedLayout : VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorBufferInfo cfa{floatInput.buffer ? floatInput.buffer : dummyNormalized.buffer,
                                   floatInput.buffer ? floatInput.offset : 0,
                                   floatInput.buffer ? floatInput.range : VK_WHOLE_SIZE};
        VkDescriptorImageInfo target{VK_NULL_HANDLE, out.view, VK_IMAGE_LAYOUT_GENERAL};
        std::array<VkWriteDescriptorSet, 7> w{};
        const std::array<uint32_t, 7> bindings{0, 1, 2, 3, 5, 6, 7};
        for (uint32_t i = 0; i < w.size(); ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = set;
            w[i].dstBinding = bindings[i];
            w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }
        w[0].pBufferInfo = &src;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        w[1].pBufferInfo = &prm;
        w[2].pBufferInfo = &res;
        w[3].pBufferInfo = &scr;
        w[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[4].pImageInfo = &packed;
        w[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[5].pImageInfo = &target;
        w[6].pBufferInfo = &cfa;
        vkUpdateDescriptorSets(ctx.device, uint32_t(w.size()), w.data(), 0, nullptr);
    }

    static void computeBarrier(VkCommandBuffer cmd) {
        VkMemoryBarrier mb{};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb,
                             0, nullptr, 0, nullptr);
    }

    void record(VkCommandBuffer cmd, const PackedCfaImageView& in, const LinearRgbImage& out,
                const std::array<float, 3>& wb) {
        if (cmd == VK_NULL_HANDLE) throw std::runtime_error("AMaZE command buffer is null");
        if (cfg.inputMode != InputMode::PackedCfaRgba16fImage)
            throw std::runtime_error("AMaZE pipeline not configured for packed CFA input");
        if (in.rawWidth != cfg.width || in.rawHeight != cfg.height || in.pattern != cfg.pattern)
            throw std::runtime_error("AMaZE packed CFA raw dimensions/pattern mismatch");
        if (in.width != cfg.width / 2u || in.height != cfg.height / 2u)
            throw std::runtime_error("AMaZE packed CFA dimensions must be rawWidth/2 x rawHeight/2");
        if (in.image == VK_NULL_HANDLE || in.view == VK_NULL_HANDLE)
            throw std::runtime_error("AMaZE packed CFA image/view is null");
        if (in.format != VK_FORMAT_R16G16B16A16_SFLOAT) throw std::runtime_error("AMaZE packed CFA must be RGBA16F");
        if (in.layout != VK_IMAGE_LAYOUT_GENERAL) throw std::runtime_error("AMaZE packed CFA must be GENERAL");
        recordTiles(cmd, in.view, in.layout, {}, out, wb);
    }

    void record(VkCommandBuffer cmd, const NormalizedBayerBufferView& in, const LinearRgbImage& out,
                const std::array<float, 3>& wb) {
        if (cmd == VK_NULL_HANDLE) throw std::runtime_error("AMaZE command buffer is null");
        if (cfg.inputMode != InputMode::NormalizedFloatBuffer)
            throw std::runtime_error("AMaZE pipeline not configured for normalized buffer input");
        if (in.width != cfg.width || in.height != cfg.height || in.pattern != cfg.pattern)
            throw std::runtime_error("AMaZE normalized input mismatch");
        if (in.buffer.buffer == VK_NULL_HANDLE) throw std::runtime_error("AMaZE normalized input buffer is null");
        const VkDeviceSize needed = VkDeviceSize(cfg.width) * cfg.height * sizeof(float);
        BufferView view = in.buffer;
        if (view.range == VK_WHOLE_SIZE) view.range = needed;
        if (view.range < needed) throw std::runtime_error("AMaZE normalized input buffer is too small");
        recordTiles(cmd, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, view, out, wb);
    }

    void recordTiles(VkCommandBuffer cmd, VkImageView packedView, VkImageLayout packedLayout, const BufferView& floatInput,
                     const LinearRgbImage& out, const std::array<float, 3>& wb) {
        if (out.image == VK_NULL_HANDLE || out.view == VK_NULL_HANDLE)
            throw std::runtime_error("AMaZE output image/view is null");
        if (out.width != cfg.width || out.height != cfg.height)
            throw std::runtime_error("AMaZE output dimensions do not match pipeline");
        if (out.format != VK_FORMAT_R16G16B16A16_SFLOAT) throw std::runtime_error("AMaZE output must be RGBA16F");
        if (out.layout != VK_IMAGE_LAYOUT_GENERAL) throw std::runtime_error("AMaZE output must be GENERAL");
        for (float g : wb)
            if (!std::isfinite(g) || !(g > 0.0f)) throw std::runtime_error("AMaZE white balance must be finite > 0");
        if (dummyPacked.image && !dummyLayoutReady) {
            VkImageMemoryBarrier b{};
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = dummyPacked.image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &b);
            dummyLayoutReady = true;
        }
        writeParams(wb);
        bindBatch(packedView, packedLayout, floatInput, out);
        computeBarrier(cmd);  // covers scratch reuse after a preceding frame
        for (size_t first = 0; first < tiles.size(); first += batchTiles) {
            const uint32_t count = uint32_t(std::min<size_t>(batchTiles, tiles.size() - first));
            // Each dispatch covers the batch's largest tile; shaders bound-check per tile.
            uint32_t srcW = 0, srcH = 0, outW = 0, outH = 0;
            for (size_t k = 0; k < count; ++k) {
                const Tile& t = tiles[first + k];
                srcW = std::max(srcW, t.srcW), srcH = std::max(srcH, t.srcH);
                outW = std::max(outW, t.outW), outH = std::max(outH, t.outH);
            }
            const uint32_t offset = uint32_t((first / batchTiles) * batchParamStride);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 1, &offset);
            for (size_t pass = 0; pass < kPasses.size(); ++pass) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[pass]);
                if (pass == kChoosePass) {
                    vkCmdDispatch(cmd, (2 * (srcW + srcH) + kChooseLocal - 1) / kChooseLocal, 1, count);
                } else if (pass == kHvwtPass || pass == kPmwtPass) {
                    vkCmdDispatch(cmd, 1, 1, count);
                } else {
                    const uint32_t w = coversOutput(pass) ? outW : srcW;
                    const uint32_t h = coversOutput(pass) ? outH : srcH;
                    vkCmdDispatch(cmd, (w + kLocal - 1) / kLocal, (h + kLocal - 1) / kLocal, count);
                }
                computeBarrier(cmd);
            }
            if (commandBoundary)
                cmd = commandBoundary();
            else if (passBoundary)
                passBoundary();
        }
    }
};

bool AmazePipeline::validateConfig(const PipelineConfig& c, const char** reason) noexcept {
    const char* why = nullptr;
    if (c.width < 32u || c.height < 32u)
        why = "AMaZE requires width and height >= 32";
    else if (c.inputMode == InputMode::PackedCfaRgba16fImage && ((c.width & 1u) || (c.height & 1u)))
        why = "AMaZE packed CFA input requires even raw dimensions";
    else if (c.tileSlots == 0u)
        why = "AMaZE requires at least one tile slot";
    else if (!std::isfinite(c.outputScale) || !std::isfinite(c.outputAlpha))
        why = "outputScale and outputAlpha must be finite";
    if (reason) *reason = why;
    return why == nullptr;
}

AmazePipeline::AmazePipeline(VulkanContext c, ShaderProvider s, PipelineConfig p)
    : impl_(std::make_unique<Impl>(c, std::move(s), p)) {}
AmazePipeline::~AmazePipeline() = default;
void AmazePipeline::record(VkCommandBuffer c, const PackedCfaImageView& i, const LinearRgbImage& o,
                           const std::array<float, 3>& wb) {
    impl_->record(c, i, o, wb);
}
void AmazePipeline::record(VkCommandBuffer c, const NormalizedBayerBufferView& i, const LinearRgbImage& o,
                           const std::array<float, 3>& wb) {
    impl_->record(c, i, o, wb);
}
void AmazePipeline::setPassBoundary(std::function<void()> boundary) { impl_->passBoundary = std::move(boundary); }
void AmazePipeline::setCommandBoundary(std::function<VkCommandBuffer()> boundary) {
    impl_->commandBoundary = std::move(boundary);
}
const PipelineConfig& AmazePipeline::config() const { return impl_->cfg; }
uint64_t AmazePipeline::currentAllocatedBytes() const { return impl_->liveBytes; }
uint64_t AmazePipeline::peakAllocatedBytes() const { return impl_->peakBytes; }

}  // namespace amaze
