#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace rawr::raw_multiframe_output {

struct CfaProjectionParameters {
    std::array<float, 4> blackByPhase{{0.0f, 0.0f, 0.0f, 0.0f}};
    float whiteLevel = 1.0f;
    std::uint32_t cfa = 0;
    // Output codes = sensor codes * codeScale (widening the range to 16 bits).
    // The DNG's black/white tags must use the same scale.
    float codeScale = 1.0f;
    // Source is a normalized single-channel R32F CFA plane (full precision)
    // instead of the RGBA16F merge output.
    bool sourceIsCfaR32f = false;
};
// Scale that maps whiteLevel to 65535, filling the 16-bit range (a
// reduced-range mode with white 8712 -> 7.52 rather than 7). Black scales by
// the same factor and is written as an exact RATIONAL, so it need not be an
// integer code; the codes below it stay available for sub-black noise.
inline float sixteenBitCodeScale(float whiteLevel) {
    if (!(whiteLevel >= 1.0f)) return 1.0f;
    const float scale = 65535.0f / whiteLevel;
    return scale >= 1.0f ? scale : 1.0f;
}

struct SensorClipParams {
    // Optional 1A reference-frame sensor evidence. Null view disables the
    // sensor OR (derived-only mask, legacy behavior).
    VkImageView refRawView = VK_NULL_HANDLE;
    std::uint32_t refWidth = 0;
    std::uint32_t refHeight = 0;
    std::uint32_t cfa = 0;
    std::array<float, 4> blackByPhase{{0.0f, 0.0f, 0.0f, 0.0f}};
    float whiteLevel = 1.0f;
    float clipThreshold = 0.995f;
};

// Native-RGB colour rebuild (prepareRgb only). A per-pixel merge (Rexture) fills red and blue at every pixel by
// interpolating that colour's own sparse samples; on fine texture near the sensor's resolution those values alias
// differently from green and show as broad false-colour bands (moire). At the colour's own CFA sites the merged value
// is reliable (the base frame's sample sits on the output grid), so red and blue are rebuilt there as white-balanced
// differences to the merged green and interpolated bilinearly from those sites; green is kept. Same principle as a
// demosaic's colour-difference step, on merged data.
struct SiteChromaParams {
    bool enabled = false;
    std::uint32_t cfa = 0;           // 0 RGGB, 1 GRBG, 2 GBRG, 3 BGGR
    float whiteBalanceR = 1.0f;      // camera-neutral gains relative to green
    float whiteBalanceB = 1.0f;
    // Noise variance of the merged image per normalised sample value x: noiseSlope * x + noiseOffset (the burst noise fit
    // divided by the merged frame count). Widens the site bound so it only clamps real outliers; 0 / 0: strict bound.
    float noiseSlope = 0.0f;
    float noiseOffset = 0.0f;
};

struct PackedRaw16 {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t rowStrideBytes = 0;
    std::vector<std::uint8_t> pixels;
};

// Output-boundary adapter for the multiframe pipeline. All reconstruction and
// CFA projection stay on the GPU; only the final packed DNG payload is read to
// host memory. Resources are allocated once and reused for base and merged DNGs.
class MultiframeOutputAdapter final {
   public:
    using Submit = std::function<void(const VkSubmitInfo&, VkFence)>;

    MultiframeOutputAdapter() = default;
    ~MultiframeOutputAdapter() { reset(); }
    MultiframeOutputAdapter(const MultiframeOutputAdapter&) = delete;
    MultiframeOutputAdapter& operator=(const MultiframeOutputAdapter&) = delete;

    void initialize(VkPhysicalDevice physical, VkDevice device, std::uint32_t queueFamily, Submit submit,
                    std::uint32_t width, std::uint32_t height);
    // Optional caller-owned pipeline cache for every pipeline this adapter creates (set before initialize; kept across
    // reset). The app needs none: its Vulkan dispatch routes VK_NULL_HANDLE to its shared disk-backed cache.
    void setPipelineCache(VkPipelineCache cache) noexcept { pipelineCache_ = cache; }
    PackedRaw16 readBase(VkImage rawR16Uint);
    // mergedView: the RGBA16F merge output, or (parameters.sourceIsCfaR32f) a
    // normalized R32F CFA plane of the same extent.
    PackedRaw16 projectMerged(VkImage mergedImage, VkImageView mergedView, const CfaProjectionParameters& parameters);
    // Copies full camera-linear RGB, applying LSC once and deriving clip evidence
    // before LSC/WB. When sensorClip.refRawView is set and geometry matches
    // (scale 1x), reference sensor bits are ORed into the mask (1A). The
    // returned images survive release of the merge arena.
    void prepareRgb(VkImageView mergedView, std::uint32_t lscWidth, std::uint32_t lscHeight,
                    const std::vector<float>& lscGains,
                    const SensorClipParams& sensorClip = SensorClipParams{},
                    const SiteChromaParams& siteChroma = SiteChromaParams{});
    // Same inputs and clip evidence as prepareRgb, but writes the still
    // demosaicer's packed CFA contract instead: (width/2 x height/2) RGBA16F
    // texels holding the merged R, G1, G2, B site values, LSC applied (see
    // prepare_packed_cfa.comp). rgbImage()/rgbView() then return that image.
    void preparePackedCfa(VkImageView mergedView, std::uint32_t lscWidth, std::uint32_t lscHeight,
                          const std::vector<float>& lscGains,
                          const SensorClipParams& sensorClip = SensorClipParams{});
    [[nodiscard]] bool sensorClipOrApplied() const noexcept { return sensorClipOrApplied_; }
    // Last prepare: wall time creating its resources and pipelines, and from submit to fence (the GPU work).
    [[nodiscard]] double lastSetupMs() const noexcept { return lastSetupMs_; }
    [[nodiscard]] double lastGpuMs() const noexcept { return lastGpuMs_; }
    [[nodiscard]] double lastPipelineMs() const noexcept { return lastPipelineMs_; }   // part of lastSetupMs: pipeline creation
    VkImage rgbImage() const noexcept { return rgb_; }
    VkImageView rgbView() const noexcept { return rgbView_; }
    VkImage clipImage() const noexcept { return clip_; }
    VkImageView clipView() const noexcept { return clipView_; }
    void releaseDngResources() noexcept;
    void reset() noexcept;

   private:
    std::uint32_t memoryType(std::uint32_t bits, VkMemoryPropertyFlags flags) const;
    void recordReadback(VkImage source, bool sourceIsProjection);
    PackedRaw16 copyMapped() const;
    void prepare(VkImageView mergedView, std::uint32_t lscWidth, std::uint32_t lscHeight,
                 const std::vector<float>& lscGains, const SensorClipParams& sensorClip, bool packedCfa,
                 const SiteChromaParams& siteChroma);

    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    std::uint32_t queueFamily_ = 0;
    Submit submit_{};
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    VkImage projection_ = VK_NULL_HANDLE;
    VkDeviceMemory projectionMemory_ = VK_NULL_HANDLE;
    VkImageView projectionView_ = VK_NULL_HANDLE;
    bool projectionInitialized_ = false;
    VkBuffer readback_ = VK_NULL_HANDLE;
    VkDeviceMemory readbackMemory_ = VK_NULL_HANDLE;
    void* mapped_ = nullptr;
    VkDeviceSize readbackBytes_ = 0;
    VkDescriptorSetLayout descriptorLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipeline pipelineF32_ = VK_NULL_HANDLE;  // R32F CFA source variant
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkImage rgb_ = VK_NULL_HANDLE, clip_ = VK_NULL_HANDLE;
    VkImageView rgbView_ = VK_NULL_HANDLE, clipView_ = VK_NULL_HANDLE;
    VkDeviceMemory rgbMemory_ = VK_NULL_HANDLE, clipMemory_ = VK_NULL_HANDLE;
    VkBuffer lscBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory lscMemory_ = VK_NULL_HANDLE;
    VkPipeline rgbPipeline_ = VK_NULL_HANDLE;
    // Site chroma: the re-estimated green (prepare_green.comp; 1 x 1 placeholder when site chroma is off).
    VkImage greenAlt_ = VK_NULL_HANDLE;
    VkImageView greenAltView_ = VK_NULL_HANDLE;
    VkDeviceMemory greenAltMemory_ = VK_NULL_HANDLE;
    VkPipeline greenPipeline_ = VK_NULL_HANDLE;
    bool sensorClipOrApplied_ = false;
    double lastSetupMs_ = 0.0, lastGpuMs_ = 0.0, lastPipelineMs_ = 0.0;
    VkPipelineCache pipelineCache_ = VK_NULL_HANDLE;
};

}  // namespace rawr::raw_multiframe_output
