#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "rawr/vk/OwnedImage.h"

namespace rawr::highlight {
class GuideChain;
class GuideChainPipelines;
}

namespace raw_preview {
enum class BayerPattern : uint32_t { RGGB=0, GRBG=1, GBRG=2, BGGR=3 };


// Physical CFA state. G1 and G2 remain independent here by design.
// Photographer-facing consumers should derive logical G as (G1 || G2)
// within each state class, then reason about R/G/B severity.
// Do not interpret four set CFA bits as four user-facing color channels.
enum CfaStateBit : uint16_t {
    CfaStateHighlightClippedR  = 1u << 0,
    CfaStateHighlightClippedG1 = 1u << 1,
    CfaStateHighlightClippedG2 = 1u << 2,
    CfaStateHighlightClippedB  = 1u << 3,

    CfaStateHighlightWarningR  = 1u << 4,
    CfaStateHighlightWarningG1 = 1u << 5,
    CfaStateHighlightWarningG2 = 1u << 6,
    CfaStateHighlightWarningB  = 1u << 7,

    CfaStateShadowWarningR     = 1u << 8,
    CfaStateShadowWarningG1    = 1u << 9,
    CfaStateShadowWarningG2    = 1u << 10,
    CfaStateShadowWarningB     = 1u << 11,

    CfaStateShadowClippedR     = 1u << 12,
    CfaStateShadowClippedG1    = 1u << 13,
    CfaStateShadowClippedG2    = 1u << 14,
    CfaStateShadowClippedB     = 1u << 15,
};

struct RawFrameParameters {
    float blackLevel[4] = {0,0,0,0};
    float whiteLevel = 65535.0f;
    float whiteBalance[4] = {1,1,1,1};
    BayerPattern pattern = BayerPattern::RGGB;

    // Sensor clipping is classified here, before LSC/WB. Reconstruction consumes
    // that provenance after the preview's 2x2 RGB formation and white balance.
    float clipThreshold = 0.995f;   // samples at/above this are considered unreliable/clipped
    float edgeStrength = 8.0f;      // rejects ratios across strong luminance edges
    float chromaBlend = 0.20f;      // mild R-G/B-G neighborhood stabilization
    bool highlightReconstructionEnabled = true;
    // Same selection and tuning as still/video capture: 0 colour propagation,
    // 1 Inpaint Opposed (RawTherapee Coloropp + SDR highlight compression).
    uint32_t highlightMethod = 0;
    float highlightThreshold = 1.0f;
    float highlightCompression = 100.0f;
    bool bypassHighlightTone = false;
    float highlightExposureGain = 1.0f;  // aePostGain * 2^exposureEV

    // Optional CFA-state mask warning thresholds, in normalized sensor-linear units.
    float highlightWarningThreshold = 0.98f;
    float shadowWarningThreshold = 0.01f;
};

struct RawPreviewCreateInfo {
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    std::string shaderPath;
    std::string cfaStateShaderPath; // required only when outputCfaStateR16UintView is used
    std::string highlightGuideSeedShaderPath;
    std::string highlightGuidePropagateShaderPath;
    std::string highlightGuideSmoothShaderPath;
    std::string highlightApplyShaderPath;
    // Inpaint Opposed: coloropp.comp built with RAWR_COLOROPP_POST_WB=1, and
    // coloropp_tone.comp. Empty paths default to the CFA-state shader dir.
    std::string coloroppShaderPath;
    std::string coloroppToneShaderPath;
    // Validated production workgroup. Other sizes are intentionally rejected.
    uint32_t workgroupX = 8;
    uint32_t workgroupY = 8;
};

struct RawPreviewRecordInfo {
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkImageView inputRawR16UintView = VK_NULL_HANDLE;
    VkImageView outputLinearRgba16fView = VK_NULL_HANDLE;
    VkImageView outputCfaStateR16UintView = VK_NULL_HANDLE; // optional, half-resolution VK_FORMAT_R16_UINT
    // Optional byte-addressed view of the same camera AHB (imported storage
    // buffer). When set with inputRawStridePixels, the shaders read mosaic
    // samples via y * stride + x instead of imageLoad, bypassing sampler
    // pitch granularity. VK_NULL_HANDLE keeps the legacy image path.
    VkBuffer inputRawImportBuffer = VK_NULL_HANDLE;
    uint32_t inputRawStridePixels = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    RawFrameParameters parameters{};
    uint32_t lensShadingMapWidth = 0;
    uint32_t lensShadingMapHeight = 0;
    const float* lensShadingMap = nullptr; // Camera2 interleaved [R,G_even,G_odd,B]
    size_t lensShadingMapFloatCount = 0;
};

// Image the display path (tonemap/film) should read. It differs from the
// linear output only under Inpaint Opposed, whose SDR highlight compression
// must not reach RAW-domain consumers of the linear image (overlays, scopes).
struct RawPreviewRecordResult {
    VkImage sdrImage = VK_NULL_HANDLE;    // VK_NULL_HANDLE: use the linear output
    VkImageView sdrView = VK_NULL_HANDLE;
};

class RawPreview {
public:
    explicit RawPreview(const RawPreviewCreateInfo&);
    ~RawPreview();
    RawPreview(const RawPreview&) = delete;
    RawPreview& operator=(const RawPreview&) = delete;
    // Records commands only; caller owns submission and synchronization.
    RawPreviewRecordResult record(const RawPreviewRecordInfo&);
    size_t cachedDescriptorSetCount() const { return descriptorCache_.size(); }
private:
    struct ViewPair { VkImageView input{}, output{}; VkBuffer import{}; bool operator==(const ViewPair& o) const { return input==o.input && output==o.output && import==o.import; } };
    struct ViewPairHash { size_t operator()(const ViewPair& k) const noexcept { size_t a=std::hash<VkImageView>{}(k.input), b=std::hash<VkImageView>{}(k.output), c=std::hash<VkBuffer>{}(k.import); size_t h=a^(b+size_t(0x9e3779b97f4a7c15ull)+(a<<6)+(a>>2)); h^=c+size_t(0x9e3779b97f4a7c15ull)+(h<<6)+(h>>2); return h; } };
    struct ViewTriple { VkImageView input{}, output{}, state{}; VkBuffer import{}; bool operator==(const ViewTriple& o) const { return input==o.input && output==o.output && state==o.state && import==o.import; } };
    struct ViewTripleHash { size_t operator()(const ViewTriple& k) const noexcept { size_t h=std::hash<VkImageView>{}(k.input); h^=std::hash<VkImageView>{}(k.output)+size_t(0x9e3779b97f4a7c15ull)+(h<<6)+(h>>2); h^=std::hash<VkImageView>{}(k.state)+size_t(0x9e3779b97f4a7c15ull)+(h<<6)+(h>>2); h^=std::hash<VkBuffer>{}(k.import)+size_t(0x9e3779b97f4a7c15ull)+(h<<6)+(h>>2); return h; } };
    VkDescriptorSet descriptorSetFor(VkImageView inputView, VkImageView outputView, VkBuffer lscBuffer,
                                     VkBuffer importBuffer);
    VkDescriptorSet cfaStateDescriptorSetFor(VkImageView inputView, VkImageView outputView, VkImageView stateView,
                                             VkBuffer lscBuffer, VkBuffer importBuffer);
    struct LscBuffer { VkBuffer buffer=VK_NULL_HANDLE; VkDeviceMemory memory=VK_NULL_HANDLE; void* mapped=nullptr; VkDeviceSize bytes=0; };
    LscBuffer& lscBufferFor(VkImageView outputView, size_t floatCount);
    void destroyLscBuffer(LscBuffer&) noexcept;
    struct HighlightResources {
        std::unique_ptr<rawr::highlight::GuideChain> chain;
        VkDescriptorSet applySet=VK_NULL_HANDLE;
    };
    HighlightResources& highlightResourcesFor(VkImageView outputView, uint32_t width, uint32_t height);
    struct ColoroppResources {
        rawr::vk::OwnedImage demosaiced{}, sdr{};  // cfa_state output, SDR tone output
        bool initialized=false;
        VkDescriptorSet coloroppSet=VK_NULL_HANDLE;
        VkDescriptorSet toneSet=VK_NULL_HANDLE;
    };
    ColoroppResources& coloroppResourcesFor(VkImageView outputView, uint32_t width, uint32_t height);
    void recordColoropp(const RawPreviewRecordInfo& recordInfo, ColoroppResources& resources, VkBuffer lscBuffer,
                        bool lscValid);
    VkPhysicalDevice physicalDevice_{};
    VkDevice device_{};
    VkDescriptorSetLayout dsl_{};
    VkPipelineLayout layout_{};
    VkPipeline pipeline_{};
    VkDescriptorSetLayout cfaStateDsl_{};
    VkPipelineLayout cfaStateLayout_{};
    VkPipeline cfaStatePipeline_{};
    std::unique_ptr<rawr::highlight::GuideChainPipelines> guidePipelines_;
    VkDescriptorSetLayout highlightApplyDsl_{};
    VkPipelineLayout highlightApplyLayout_{};
    VkPipeline highlightApplyPipeline_{};
    VkDescriptorSetLayout coloroppDsl_{};
    VkPipelineLayout coloroppLayout_{};
    VkPipeline coloroppPipeline_{};
    VkDescriptorSetLayout coloroppToneDsl_{};
    VkPipelineLayout coloroppToneLayout_{};
    VkPipeline coloroppTonePipeline_{};
    std::unordered_map<VkImageView,ColoroppResources> coloroppResources_;
    VkDescriptorPool pool_{};
    std::unordered_map<ViewPair,VkDescriptorSet,ViewPairHash> descriptorCache_;
    std::unordered_map<VkImageView,LscBuffer> lscBuffers_;
    std::unordered_map<VkImageView,HighlightResources> highlightResources_;
    std::unordered_map<ViewTriple,VkDescriptorSet,ViewTripleHash> cfaStateDescriptorCache_;
};
}
