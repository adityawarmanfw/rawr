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
    // Output codes = sensor codes * codeScale (an integer widening the range
    // toward 16 bits). The DNG's black/white tags must use the same scale.
    float codeScale = 1.0f;
    // Source is a normalized single-channel R32F CFA plane (full precision)
    // instead of the RGBA16F merge output.
    bool sourceIsCfaR32f = false;
};
// Largest integer scale that keeps whiteLevel * scale within 16 bits, so
// black stays an integer code (14-bit white 16383 -> 4, a reduced-range mode
// with white 8712 -> 7, RAW10 white 1023 -> 64, 16-bit sensors -> 1).
inline float sixteenBitCodeScale(float whiteLevel) {
    if (!(whiteLevel >= 1.0f)) return 1.0f;
    const float scale = std::floor(65535.0f / whiteLevel);
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
                    const SensorClipParams& sensorClip = SensorClipParams{});
    // Same inputs and clip evidence as prepareRgb, but writes the still
    // demosaicer's packed CFA contract instead: (width/2 x height/2) RGBA16F
    // texels holding the merged R, G1, G2, B site values, LSC applied (see
    // prepare_packed_cfa.comp). rgbImage()/rgbView() then return that image.
    void preparePackedCfa(VkImageView mergedView, std::uint32_t lscWidth, std::uint32_t lscHeight,
                          const std::vector<float>& lscGains,
                          const SensorClipParams& sensorClip = SensorClipParams{});
    [[nodiscard]] bool sensorClipOrApplied() const noexcept { return sensorClipOrApplied_; }
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
                 const std::vector<float>& lscGains, const SensorClipParams& sensorClip, bool packedCfa);

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
    bool sensorClipOrApplied_ = false;
};

}  // namespace rawr::raw_multiframe_output
