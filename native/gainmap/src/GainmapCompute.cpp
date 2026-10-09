#include "gainmap/GainmapCompute.h"

#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace gainmap {
namespace {

void check(VkResult result, const char* what) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(what) + " failed VkResult=" + std::to_string(int(result)));
}

struct Push {
    uint32_t mapWidth;
    uint32_t mapHeight;
    uint32_t hdrWidth;
    uint32_t hdrHeight;
    uint32_t sdrWidth;
    uint32_t sdrHeight;
    float minLog2;
    float maxLog2;
    float gamma;
    float offsetSdr;
    float offsetHdr;
    float hdrExposure;
    float clipBoost;
    uint32_t useClip;
    float satProtect;
    uint32_t multiChannel;
    uint32_t useGlow;
    float glowStrength;
    float glowMax;
    float hdrToLinearSrgb[9];
    uint32_t fullRes;
};

struct BlurPush {
    uint32_t width;
    uint32_t height;
    uint32_t horizontal;
    float sigma;
};

}  // namespace

struct GainmapCompute::Impl {
    VulkanContext ctx{};
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkShaderModule module = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> sets;
    // 1x1 all-clear R16UI fallback for binding 3 when the caller has no clip
    // mask (offline replay, tests). Host-visible, zero-filled once; GENERAL
    // layout, never written by the GPU. Avoids a second pipeline/mismatched
    // format bind for the no-clip path.
    VkImage fallbackClip = VK_NULL_HANDLE;
    VkDeviceMemory fallbackClipMemory = VK_NULL_HANDLE;
    VkImageView fallbackClipView = VK_NULL_HANDLE;
    // 1x1 identity RGBA16F fallback for binding 4 when the caller has no
    // glow map. Host-visible, filled once with half 1.0 (0x3C00); GENERAL
    // layout, never written by the GPU. The shader pins loads here when
    // useGlow is 0, so the tap stays pure scene.
    VkImage fallbackGlow = VK_NULL_HANDLE;
    VkDeviceMemory fallbackGlowMemory = VK_NULL_HANDLE;
    VkImageView fallbackGlowView = VK_NULL_HANDLE;
    // Blur stage (separable Gaussian over the encoded map): second pipeline
    // with its own descriptor pool/sets, plus a lazily-sized scratch image
    // (same dims/format as the map). Null when the caller passes no blur
    // SPIR-V: the blur pass is then skipped regardless of mapBlurSigma.
    VkDescriptorSetLayout blurSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout blurPipelineLayout = VK_NULL_HANDLE;
    VkPipeline blurPipeline = VK_NULL_HANDLE;
    VkShaderModule blurModule = VK_NULL_HANDLE;
    VkDescriptorPool blurPool = VK_NULL_HANDLE;
    std::vector<std::array<VkDescriptorSet, 2>> blurSets;  // per frame: {H, V}
    VkImage blurScratch = VK_NULL_HANDLE;
    VkDeviceMemory blurScratchMemory = VK_NULL_HANDLE;
    VkImageView blurScratchView = VK_NULL_HANDLE;
    uint32_t blurScratchW = 0;
    uint32_t blurScratchH = 0;
    bool blurScratchInit = false;

    Impl(const GainmapCreateInfo& ci) : ctx(ci.context) {
        if (ctx.device == VK_NULL_HANDLE) throw std::invalid_argument("gainmap: null device");
        if (ci.shaderSpirv == nullptr || ci.shaderSpirvBytes == 0 || (ci.shaderSpirvBytes % 4) != 0)
            throw std::invalid_argument("gainmap: missing shader SPIR-V");

        VkShaderModuleCreateInfo mci{};
        mci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        mci.codeSize = ci.shaderSpirvBytes;
        mci.pCode = ci.shaderSpirv;
        check(vkCreateShaderModule(ctx.device, &mci, ctx.allocator, &module), "gainmap vkCreateShaderModule");

        std::array<VkDescriptorSetLayoutBinding, 5> bindings{};
        for (uint32_t i = 0; i < 5; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = 5;
        lci.pBindings = bindings.data();
        check(vkCreateDescriptorSetLayout(ctx.device, &lci, ctx.allocator, &setLayout),
              "gainmap vkCreateDescriptorSetLayout");

        VkPushConstantRange push{};
        push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push.offset = 0;
        push.size = sizeof(Push);
        VkPipelineLayoutCreateInfo pli{};
        pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &setLayout;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &push;
        check(vkCreatePipelineLayout(ctx.device, &pli, ctx.allocator, &pipelineLayout),
              "gainmap vkCreatePipelineLayout");

        VkComputePipelineCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pci.stage.module = module;
        pci.stage.pName = "main";
        pci.layout = pipelineLayout;
        check(vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &pci, ctx.allocator, &pipeline),
              "gainmap vkCreateComputePipelines");

        const uint32_t frames = ci.maxFramesInFlight == 0 ? 1 : ci.maxFramesInFlight;
        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        poolSize.descriptorCount = frames * 5;
        VkDescriptorPoolCreateInfo pci2{};
        pci2.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci2.maxSets = frames;
        pci2.poolSizeCount = 1;
        pci2.pPoolSizes = &poolSize;
        check(vkCreateDescriptorPool(ctx.device, &pci2, ctx.allocator, &pool), "gainmap vkCreateDescriptorPool");

        sets.resize(frames);
        std::vector<VkDescriptorSetLayout> layouts(frames, setLayout);
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = pool;
        ai.descriptorSetCount = frames;
        ai.pSetLayouts = layouts.data();
        check(vkAllocateDescriptorSets(ctx.device, &ai, sets.data()), "gainmap vkAllocateDescriptorSets");

        // All-clear 1x1 R16UI fallback for the clip binding (see member doc).
        // LINEAR tiling + host-visible heap so it can be zero-filled on CPU
        // with no queue. Fail fast here if the format lacks linear storage.
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(ctx.physicalDevice, VK_FORMAT_R16_UINT, &fp);
        if (!(fp.linearTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))
            throw std::runtime_error("gainmap: R16_UINT lacks linear storage support");
        VkImageCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        fci.imageType = VK_IMAGE_TYPE_2D;
        fci.format = VK_FORMAT_R16_UINT;
        fci.extent = {1, 1, 1};
        fci.mipLevels = 1;
        fci.arrayLayers = 1;
        fci.samples = VK_SAMPLE_COUNT_1_BIT;
        fci.tiling = VK_IMAGE_TILING_LINEAR;
        fci.usage = VK_IMAGE_USAGE_STORAGE_BIT;
        fci.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
        check(vkCreateImage(ctx.device, &fci, ctx.allocator, &fallbackClip), "gainmap fallback clip image");
        VkMemoryRequirements mr{};
        vkGetImageMemoryRequirements(ctx.device, fallbackClip, &mr);
        VkPhysicalDeviceMemoryProperties mp{};
        vkGetPhysicalDeviceMemoryProperties(ctx.physicalDevice, &mp);
        uint32_t memType = ~0u;
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
            if ((mr.memoryTypeBits & (1u << i)) &&
                (mp.memoryTypes[i].propertyFlags &
                 (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                memType = i;
                break;
            }
        }
        if (memType == ~0u) throw std::runtime_error("gainmap: no host-visible memory for fallback clip");
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = memType;
        check(vkAllocateMemory(ctx.device, &mai, ctx.allocator, &fallbackClipMemory),
              "gainmap fallback clip memory");
        check(vkBindImageMemory(ctx.device, fallbackClip, fallbackClipMemory, 0), "gainmap fallback clip bind");
        void* mapped = nullptr;
        check(vkMapMemory(ctx.device, fallbackClipMemory, 0, mr.size, 0, &mapped), "gainmap fallback clip map");
        std::memset(mapped, 0, size_t(mr.size));
        vkUnmapMemory(ctx.device, fallbackClipMemory);
        VkImageViewCreateInfo fvi{};
        fvi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        fvi.image = fallbackClip;
        fvi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        fvi.format = VK_FORMAT_R16_UINT;
        fvi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        check(vkCreateImageView(ctx.device, &fvi, ctx.allocator, &fallbackClipView),
              "gainmap fallback clip view");

        // Identity 1x1 RGBA16F fallback for the glow binding (see member
        // doc). Half 1.0 is 0x3C00; fail fast if the format lacks linear
        // storage like the clip path above.
        VkFormatProperties gfp{};
        vkGetPhysicalDeviceFormatProperties(ctx.physicalDevice, VK_FORMAT_R16G16B16A16_SFLOAT, &gfp);
        if (!(gfp.linearTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))
            throw std::runtime_error("gainmap: RGBA16F lacks linear storage support");
        VkImageCreateInfo gci{};
        gci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        gci.imageType = VK_IMAGE_TYPE_2D;
        gci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        gci.extent = {1, 1, 1};
        gci.mipLevels = 1;
        gci.arrayLayers = 1;
        gci.samples = VK_SAMPLE_COUNT_1_BIT;
        gci.tiling = VK_IMAGE_TILING_LINEAR;
        gci.usage = VK_IMAGE_USAGE_STORAGE_BIT;
        gci.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
        check(vkCreateImage(ctx.device, &gci, ctx.allocator, &fallbackGlow), "gainmap fallback glow image");
        VkMemoryRequirements gmr{};
        vkGetImageMemoryRequirements(ctx.device, fallbackGlow, &gmr);
        uint32_t gmemType = ~0u;
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
            if ((gmr.memoryTypeBits & (1u << i)) &&
                (mp.memoryTypes[i].propertyFlags &
                 (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                gmemType = i;
                break;
            }
        }
        if (gmemType == ~0u) throw std::runtime_error("gainmap: no host-visible memory for fallback glow");
        VkMemoryAllocateInfo gmai{};
        gmai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        gmai.allocationSize = gmr.size;
        gmai.memoryTypeIndex = gmemType;
        check(vkAllocateMemory(ctx.device, &gmai, ctx.allocator, &fallbackGlowMemory),
              "gainmap fallback glow memory");
        check(vkBindImageMemory(ctx.device, fallbackGlow, fallbackGlowMemory, 0), "gainmap fallback glow bind");
        void* gmapped = nullptr;
        check(vkMapMemory(ctx.device, fallbackGlowMemory, 0, gmr.size, 0, &gmapped), "gainmap fallback glow map");
        {
            // Half 1.0 (0x3C00) in the first row's RGBA texel; zero the rest
            // (linear row pitch may exceed 8 bytes).
            constexpr uint16_t kHalfOne = 0x3C00u;
            uint16_t* words = static_cast<uint16_t*>(gmapped);
            const size_t count = gmr.size / sizeof(uint16_t);
            for (size_t i = 0; i < count; ++i) words[i] = i < 4u ? kHalfOne : 0u;
        }
        vkUnmapMemory(ctx.device, fallbackGlowMemory);
        VkImageViewCreateInfo gvi{};
        gvi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        gvi.image = fallbackGlow;
        gvi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        gvi.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        gvi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        check(vkCreateImageView(ctx.device, &gvi, ctx.allocator, &fallbackGlowView),
              "gainmap fallback glow view");

        // Blur stage: optional second pipeline (skipped entirely without
        // blur SPIR-V; record() then ignores mapBlurSigma).
        if (ci.blurShaderSpirv != nullptr && ci.blurShaderSpirvBytes != 0 &&
            (ci.blurShaderSpirvBytes % 4) == 0) {
            VkShaderModuleCreateInfo bci{};
            bci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            bci.codeSize = ci.blurShaderSpirvBytes;
            bci.pCode = ci.blurShaderSpirv;
            check(vkCreateShaderModule(ctx.device, &bci, ctx.allocator, &blurModule),
                  "gainmap blur vkCreateShaderModule");

            std::array<VkDescriptorSetLayoutBinding, 2> bb{};
            for (uint32_t i = 0; i < 2; ++i) {
                bb[i].binding = i;
                bb[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                bb[i].descriptorCount = 1;
                bb[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            }
            VkDescriptorSetLayoutCreateInfo blci{};
            blci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            blci.bindingCount = 2;
            blci.pBindings = bb.data();
            check(vkCreateDescriptorSetLayout(ctx.device, &blci, ctx.allocator, &blurSetLayout),
                  "gainmap blur vkCreateDescriptorSetLayout");

            VkPushConstantRange bpush{};
            bpush.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            bpush.offset = 0;
            bpush.size = sizeof(BlurPush);
            VkPipelineLayoutCreateInfo bpli{};
            bpli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            bpli.setLayoutCount = 1;
            bpli.pSetLayouts = &blurSetLayout;
            bpli.pushConstantRangeCount = 1;
            bpli.pPushConstantRanges = &bpush;
            check(vkCreatePipelineLayout(ctx.device, &bpli, ctx.allocator, &blurPipelineLayout),
                  "gainmap blur vkCreatePipelineLayout");

            VkComputePipelineCreateInfo bpci{};
            bpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            bpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            bpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            bpci.stage.module = blurModule;
            bpci.stage.pName = "main";
            bpci.layout = blurPipelineLayout;
            check(vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &bpci, ctx.allocator, &blurPipeline),
                  "gainmap blur vkCreateComputePipelines");

            VkDescriptorPoolSize bpoolSize{};
            bpoolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bpoolSize.descriptorCount = frames * 2 * 2;
            VkDescriptorPoolCreateInfo bpci2{};
            bpci2.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            bpci2.maxSets = frames * 2;
            bpci2.poolSizeCount = 1;
            bpci2.pPoolSizes = &bpoolSize;
            check(vkCreateDescriptorPool(ctx.device, &bpci2, ctx.allocator, &blurPool),
                  "gainmap blur vkCreateDescriptorPool");

            blurSets.resize(frames);
            std::vector<VkDescriptorSetLayout> blayouts(frames * 2, blurSetLayout);
            VkDescriptorSetAllocateInfo bai{};
            bai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            bai.descriptorPool = blurPool;
            bai.descriptorSetCount = frames * 2;
            bai.pSetLayouts = blayouts.data();
            std::vector<VkDescriptorSet> bsets(frames * 2);
            check(vkAllocateDescriptorSets(ctx.device, &bai, bsets.data()), "gainmap blur vkAllocateDescriptorSets");
            for (uint32_t f = 0; f < frames; ++f) {
                blurSets[f][0] = bsets[f * 2];
                blurSets[f][1] = bsets[f * 2 + 1];
            }
        }
    }

    ~Impl() {
        if (ctx.device == VK_NULL_HANDLE) return;
        destroyBlurScratch();
        if (blurPool) vkDestroyDescriptorPool(ctx.device, blurPool, ctx.allocator);
        if (blurPipeline) vkDestroyPipeline(ctx.device, blurPipeline, ctx.allocator);
        if (blurModule) vkDestroyShaderModule(ctx.device, blurModule, ctx.allocator);
        if (blurPipelineLayout) vkDestroyPipelineLayout(ctx.device, blurPipelineLayout, ctx.allocator);
        if (blurSetLayout) vkDestroyDescriptorSetLayout(ctx.device, blurSetLayout, ctx.allocator);
        if (fallbackClipView) vkDestroyImageView(ctx.device, fallbackClipView, ctx.allocator);
        if (fallbackClip) vkDestroyImage(ctx.device, fallbackClip, ctx.allocator);
        if (fallbackClipMemory) vkFreeMemory(ctx.device, fallbackClipMemory, ctx.allocator);
        if (fallbackGlowView) vkDestroyImageView(ctx.device, fallbackGlowView, ctx.allocator);
        if (fallbackGlow) vkDestroyImage(ctx.device, fallbackGlow, ctx.allocator);
        if (fallbackGlowMemory) vkFreeMemory(ctx.device, fallbackGlowMemory, ctx.allocator);
        if (pipeline) vkDestroyPipeline(ctx.device, pipeline, ctx.allocator);
        if (module) vkDestroyShaderModule(ctx.device, module, ctx.allocator);
        if (pool) vkDestroyDescriptorPool(ctx.device, pool, ctx.allocator);
        if (pipelineLayout) vkDestroyPipelineLayout(ctx.device, pipelineLayout, ctx.allocator);
        if (setLayout) vkDestroyDescriptorSetLayout(ctx.device, setLayout, ctx.allocator);
    }

    void destroyBlurScratch() noexcept {
        if (ctx.device == VK_NULL_HANDLE) return;
        if (blurScratchView) vkDestroyImageView(ctx.device, blurScratchView, ctx.allocator);
        if (blurScratch) vkDestroyImage(ctx.device, blurScratch, ctx.allocator);
        if (blurScratchMemory) vkFreeMemory(ctx.device, blurScratchMemory, ctx.allocator);
        blurScratchView = VK_NULL_HANDLE;
        blurScratch = VK_NULL_HANDLE;
        blurScratchMemory = VK_NULL_HANDLE;
        blurScratchW = blurScratchH = 0;
        blurScratchInit = false;
    }

    // Lazily (re)creates the blur scratch at the map dims. Emits no commands;
    // record() transitions it on first use after (re)creation.
    void ensureBlurScratch(uint32_t w, uint32_t h) {
        if (blurScratch != VK_NULL_HANDLE && blurScratchW == w && blurScratchH == h) return;
        destroyBlurScratch();
        VkImageCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {w, h, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        check(vkCreateImage(ctx.device, &ici, ctx.allocator, &blurScratch), "gainmap blur scratch image");
        VkMemoryRequirements mr{};
        vkGetImageMemoryRequirements(ctx.device, blurScratch, &mr);
        VkPhysicalDeviceMemoryProperties mp{};
        vkGetPhysicalDeviceMemoryProperties(ctx.physicalDevice, &mp);
        uint32_t memType = ~0u;
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
            if ((mr.memoryTypeBits & (1u << i)) &&
                (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                memType = i;
                break;
            }
        }
        if (memType == ~0u) {
            for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
                if (mr.memoryTypeBits & (1u << i)) {
                    memType = i;
                    break;
                }
            }
        }
        if (memType == ~0u) throw std::runtime_error("gainmap: no memory for blur scratch");
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = memType;
        check(vkAllocateMemory(ctx.device, &mai, ctx.allocator, &blurScratchMemory),
              "gainmap blur scratch memory");
        check(vkBindImageMemory(ctx.device, blurScratch, blurScratchMemory, 0), "gainmap blur scratch bind");
        VkImageViewCreateInfo ivi{};
        ivi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ivi.image = blurScratch;
        ivi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivi.format = VK_FORMAT_R8G8B8A8_UNORM;
        ivi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        check(vkCreateImageView(ctx.device, &ivi, ctx.allocator, &blurScratchView),
              "gainmap blur scratch view");
        blurScratchW = w;
        blurScratchH = h;
        blurScratchInit = false;
    }
};


bool GainmapCompute::validateParams(const GainmapParams& p, const char** reason) noexcept {
    auto fail = [&](const char* msg) {
        if (reason) *reason = msg;
        return false;
    };
    if (!(p.maxLog2 > p.minLog2)) return fail("gainmap: maxLog2 must exceed minLog2");
    if (!(p.gamma > 0.0f) || !std::isfinite(p.gamma)) return fail("gainmap: gamma must be finite and > 0");
    // Strictly > 0 on the encode side: the shader divides by
    // (sdrLin + offsetSdr), so a zero SDR offset turns black texels into
    // Inf/NaN. (The mux metadata path tolerates 0; this is encode only.)
    if (!(p.offsetSdr > 0.0f) || !std::isfinite(p.offsetSdr)) return fail("gainmap: offsetSdr must be finite and > 0");
    if (!(p.offsetHdr > 0.0f) || !std::isfinite(p.offsetHdr)) return fail("gainmap: offsetHdr must be finite and > 0");
    if (!(p.hdrExposure > 0.0f) || !std::isfinite(p.hdrExposure))
        return fail("gainmap: hdrExposure must be finite and > 0");
    // 0 disables the specular boost (legacy exact); otherwise a linear gain.
    if (!(p.clipBoost >= 0.0f) || !std::isfinite(p.clipBoost))
        return fail("gainmap: clipBoost must be finite and >= 0");
    // Chroma protection: 0 = off, 1 = saturated brights pinned near SDR.
    if (!(p.satProtect >= 0.0f) || !(p.satProtect <= 1.0f) || !std::isfinite(p.satProtect))
        return fail("gainmap: satProtect must be finite and in [0, 1]");
    // Blur sigma: 0 = off, otherwise map texels (validated 0..8).
    if (!(p.mapBlurSigma >= 0.0f) || !(p.mapBlurSigma <= 8.0f) || !std::isfinite(p.mapBlurSigma))
        return fail("gainmap: mapBlurSigma must be finite and in [0, 8]");
    // Film glow: 0 = off (pure scene tap), otherwise scales the glow factor
    // toward the clamped quotient. Upper clamp must admit at least unity.
    if (!(p.glowStrength >= 0.0f) || !(p.glowStrength <= 1.0f) || !std::isfinite(p.glowStrength))
        return fail("gainmap: glowStrength must be finite and in [0, 1]");
    if (!(p.glowMax >= 1.0f) || !std::isfinite(p.glowMax))
        return fail("gainmap: glowMax must be finite and >= 1");
    if (!(p.hdrCapacityMax > p.hdrCapacityMin)) return fail("gainmap: hdrCapacityMax must exceed hdrCapacityMin");
    if (!(p.hdrCapacityMin >= 0.0f)) return fail("gainmap: hdrCapacityMin must be >= 0");
    for (int i = 0; i < 9; ++i)
        if (!std::isfinite(p.hdrToLinearSrgbRowMajor[i])) return fail("gainmap: hdrToLinearSrgb must be finite");
    return true;
}

GainmapCompute::GainmapCompute(const GainmapCreateInfo& ci) : impl_(new Impl(ci)) {}
GainmapCompute::~GainmapCompute() { delete impl_; }

void GainmapCompute::record(const GainmapRecordInfo& ri) {
    const char* reason = nullptr;
    if (!validateParams(ri.params, &reason)) throw std::invalid_argument(reason ? reason : "invalid gainmap params");
    if (ri.commandBuffer == VK_NULL_HANDLE) throw std::invalid_argument("gainmap: null command buffer");
    if (ri.hdr.view == VK_NULL_HANDLE || ri.sdr.view == VK_NULL_HANDLE || ri.map.view == VK_NULL_HANDLE)
        throw std::invalid_argument("gainmap: null image view");
    if (ri.hdr.width == 0 || ri.hdr.height == 0 || ri.sdr.width == 0 || ri.sdr.height == 0 || ri.map.width == 0 ||
        ri.map.height == 0)
        throw std::invalid_argument("gainmap: zero extent");
    if (ri.frameSlot >= impl_->sets.size()) throw std::out_of_range("gainmap: frameSlot out of range");
    // Clip mask is optional: without one the boost is off and binding 3 gets
    // the internal all-clear texel (the shader skips the load via useClip).
    const bool useClip = ri.clip.view != VK_NULL_HANDLE && ri.params.clipBoost > 0.0f;
    const VkImageView clipView = useClip ? ri.clip.view : impl_->fallbackClipView;
    // Glow map is optional: without one (or with strength 0) binding 4 gets
    // the internal 1.0 texel and the tap stays pure scene. A provided map
    // must sit on the HDR-tap grid or the bilinear footprint is wrong.
    if (ri.glow.view != VK_NULL_HANDLE &&
        (ri.glow.width != ri.hdr.width || ri.glow.height != ri.hdr.height))
        throw std::invalid_argument("gainmap: glow dimensions must match hdr (full-res tap grid)");
    const bool useGlow = ri.glow.view != VK_NULL_HANDLE && ri.params.glowStrength > 0.0f;
    const VkImageView glowView = useGlow ? ri.glow.view : impl_->fallbackGlowView;

    VkDescriptorImageInfo hdrInfo{};
    hdrInfo.imageView = ri.hdr.view;
    hdrInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo sdrInfo{};
    sdrInfo.imageView = ri.sdr.view;
    sdrInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo mapInfo{};
    mapInfo.imageView = ri.map.view;
    mapInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo clipInfo{};
    clipInfo.imageView = clipView;
    clipInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo glowInfo{};
    glowInfo.imageView = glowView;
    glowInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    std::array<VkWriteDescriptorSet, 5> writes{};
    for (int i = 0; i < 5; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = impl_->sets[ri.frameSlot];
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
    writes[0].pImageInfo = &hdrInfo;
    writes[1].pImageInfo = &sdrInfo;
    writes[2].pImageInfo = &mapInfo;
    writes[3].pImageInfo = &clipInfo;
    writes[4].pImageInfo = &glowInfo;
    vkUpdateDescriptorSets(impl_->ctx.device, 5, writes.data(), 0, nullptr);

    Push push{};
    push.mapWidth = ri.map.width;
    push.mapHeight = ri.map.height;
    push.hdrWidth = ri.hdr.width;
    push.hdrHeight = ri.hdr.height;
    push.sdrWidth = ri.sdr.width;
    push.sdrHeight = ri.sdr.height;
    push.minLog2 = ri.params.minLog2;
    push.maxLog2 = ri.params.maxLog2;
    push.gamma = ri.params.gamma;
    push.offsetSdr = ri.params.offsetSdr;
    push.offsetHdr = ri.params.offsetHdr;
    push.hdrExposure = ri.params.hdrExposure;
    push.clipBoost = ri.params.clipBoost;
    push.useClip = useClip ? 1u : 0u;
    push.satProtect = ri.params.satProtect;
    push.multiChannel = ri.params.multiChannelMap ? 1u : 0u;
    push.useGlow = useGlow ? 1u : 0u;
    push.glowStrength = ri.params.glowStrength;
    push.glowMax = ri.params.glowMax;
    push.fullRes = ri.params.fullResMap ? 1u : 0u;
    const float* m = ri.hdrToLinearSrgbRowMajor3x3 != nullptr ? ri.hdrToLinearSrgbRowMajor3x3
                                                              : ri.params.hdrToLinearSrgbRowMajor;
    std::memcpy(push.hdrToLinearSrgb, m, sizeof(push.hdrToLinearSrgb));

    // Blur stage resources up front: allocation may throw, and must never
    // run after GPU commands are already recorded into the caller buffer.
    const float sigma = ri.params.mapBlurSigma;
    const bool wantBlur = sigma > 0.0f && impl_->blurPipeline != VK_NULL_HANDLE;
    if (wantBlur) impl_->ensureBlurScratch(ri.map.width, ri.map.height);

    vkCmdBindPipeline(ri.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, impl_->pipeline);
    vkCmdBindDescriptorSets(ri.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, impl_->pipelineLayout, 0, 1,
                            &impl_->sets[ri.frameSlot], 0, nullptr);
    vkCmdPushConstants(ri.commandBuffer, impl_->pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    const uint32_t gx = (ri.map.width + 15) / 16;
    const uint32_t gy = (ri.map.height + 15) / 16;
    vkCmdDispatch(ri.commandBuffer, gx, gy, 1);

    // Blur stage: H pass (map -> scratch) then V pass (scratch -> map), so
    // the caller's map image holds the final smoothed result. Layouts stay
    // GENERAL throughout; the caller-owned map image is ordered with global
    // memory barriers (no image handle available), the internal scratch with
    // image barriers. Skipped when sigma is 0 or no blur SPIR-V was given.
    if (wantBlur) {
        if (!impl_->blurScratchInit) {
            // First use after (re)creation: UNDEFINED -> GENERAL plus
            // encode-write -> blur-read ordering for the map.
            VkMemoryBarrier encodeDone{};
            encodeDone.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            encodeDone.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            encodeDone.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            VkImageMemoryBarrier scratchInit{};
            scratchInit.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            scratchInit.srcAccessMask = 0;
            scratchInit.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            scratchInit.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            scratchInit.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            scratchInit.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            scratchInit.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            scratchInit.image = impl_->blurScratch;
            scratchInit.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(ri.commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &encodeDone, 0, nullptr, 1,
                                 &scratchInit);
            impl_->blurScratchInit = true;
        } else {
            // Steady state superset: orders the encode write and the
            // previous record's reads/writes against both blur passes.
            VkMemoryBarrier all{};
            all.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            all.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            all.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(ri.commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &all, 0, nullptr, 0, nullptr);
        }

        VkDescriptorImageInfo blurSrc{};
        blurSrc.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkDescriptorImageInfo blurDst{};
        blurDst.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        std::array<VkWriteDescriptorSet, 2> bwrites{};
        for (int i = 0; i < 2; ++i) {
            bwrites[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            bwrites[i].descriptorCount = 1;
            bwrites[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        }
        BlurPush bpush{};
        bpush.width = ri.map.width;
        bpush.height = ri.map.height;
        bpush.sigma = sigma;
        for (uint32_t pass = 0; pass < 2; ++pass) {
            const bool horiz = pass == 0;
            // H: map -> scratch. V: scratch -> map.
            blurSrc.imageView = horiz ? ri.map.view : impl_->blurScratchView;
            blurDst.imageView = horiz ? impl_->blurScratchView : ri.map.view;
            bwrites[0].dstSet = impl_->blurSets[ri.frameSlot][pass];
            bwrites[0].dstBinding = 0;
            bwrites[0].pImageInfo = &blurSrc;
            bwrites[1].dstSet = impl_->blurSets[ri.frameSlot][pass];
            bwrites[1].dstBinding = 1;
            bwrites[1].pImageInfo = &blurDst;
            vkUpdateDescriptorSets(impl_->ctx.device, 2, bwrites.data(), 0, nullptr);
            if (pass == 1) {
                // H-write -> V-read on scratch, plus H-read -> V-write on
                // the caller map (WAR needs explicit ordering too).
                VkMemoryBarrier mapWar{};
                mapWar.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                mapWar.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
                mapWar.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                VkImageMemoryBarrier hv{};
                hv.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                hv.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                hv.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                hv.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
                hv.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                hv.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                hv.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                hv.image = impl_->blurScratch;
                hv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vkCmdPipelineBarrier(ri.commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mapWar, 0, nullptr, 1, &hv);
            }
            vkCmdBindPipeline(ri.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, impl_->blurPipeline);
            vkCmdBindDescriptorSets(ri.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, impl_->blurPipelineLayout, 0,
                                    1, &impl_->blurSets[ri.frameSlot][pass], 0, nullptr);
            bpush.horizontal = horiz ? 1u : 0u;
            vkCmdPushConstants(ri.commandBuffer, impl_->blurPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               sizeof(bpush), &bpush);
            vkCmdDispatch(ri.commandBuffer, gx, gy, 1);
        }
        // V pass leaves the map written (SHADER_WRITE); the caller's
        // copy/read barrier orders the transfer, as with the encode path.
    }
}

}  // namespace gainmap
