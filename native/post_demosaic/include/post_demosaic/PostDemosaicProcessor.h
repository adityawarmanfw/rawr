#pragma once
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <functional>
#include <fcc/Fcc.hpp>
#include <memory>
#include <raw_denoise/DenoisePipeline.hpp>
#include <string_view>
#include <vector>

#include "rawr/vk/OwnedImage.h"
#include "rawr/shading/LensShadingMapView.h"
#include "raw_highlight/ColoroppProcessor.hpp"

namespace rawr::highlight {
class GuideChain;
class GuideChainPipelines;
}  // namespace rawr::highlight

namespace rawr::post {

// Optional geometric undistort for the still WB pass. Coefficients follow the
// Camera2 LENS_DISTORTION / LENS_INTRINSIC_CALIBRATION convention and are
// expressed in full-pixel-array ActiveArea pixels. Gate on presence upstream;
// all-zero distortion is an identity resample.
struct StillDistortionCorrection {
    bool enabled = false;
    // [fx, fy, cx, cy, s].
    std::array<float, 5> intrinsic{0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    // [k1, k2, p1, p2, k3].
    std::array<float, 5> distortion{0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
};

// Exact still-pipeline domain bridge: pre-WB camera-linear RGB -> WB camera RGB
// -> common-ceiling clipping (HL OFF), sensor-informed recovery (HL ON),
// or preservation of a pre-demosaic reconstruction for offline replay
// -> configurable FCC (zero steps bypasses it for video).
//
// Optional profiled wavelet denoise runs FIRST on the pre-WB camera-linear
// source (own output image; downstream stages consume the denoised view).
// Disabled by strength <= 0 (legacy bit-identical).
struct DenoiseRequest {
    float strength = 0.0f;  // raw profiled multiplier (validated 1..2, to 8)
    float detail = 1.0f;    // shadow-preservation fulcrum (higher = more texture)
    float noiseA = 0.0f;    // normalized-domain S (green), variance = a*x + b
    float noiseB = 0.0f;    // normalized-domain O (green)
    // Luma-band force (Y0 threshold scale, 0..1, default 0.25 = shipped
    // gentle-luma tuning). Higher smooths luma grain harder; chroma force
    // stays fixed at 0.75. Clamped to [0,1] at record time.
    float forceY = 0.25f;
    // Wavelet band count (1..7, default 7 = full quality). Lower values drop
    // the coarsest bands first: faster, but large-scale (chroma-blotch)
    // cleanup goes first. Clamped to [1,7] at record time.
    int maxScale = 7;
};
class PostDemosaicProcessor final {
   public:
    PostDemosaicProcessor(VkPhysicalDevice physicalDevice, VkDevice device, uint32_t queueFamily, uint32_t width,
                          uint32_t height, uint32_t fccSteps, bool normalizedChroma = true, float fccEdgeSigma = 0.0f,
                          float fccChromaBound = 0.0f, float defringeStrength = 0.0f,
                          float defringeEdgeThreshold = 0.02f, float defringeLumaFloor = 0.08f);
    ~PostDemosaicProcessor();
    PostDemosaicProcessor(const PostDemosaicProcessor&) = delete;
    PostDemosaicProcessor& operator=(const PostDemosaicProcessor&) = delete;

    // timingPool (at least 18 queries) receives timestamps 0-1 colour processing
    // (WB, plus Inpaint Opposed), 1-2 highlight guide chain, 2-3 highlight
    // recovery, 4-5 FCC, 10-11 wavelet denoise and 16-17 defringe plus the
    // Inpaint Opposed tone tap. Every pair is written, adjacent when skipped.
    void record(VkCommandBuffer command, VkImage sourceImage, VkImageView sourceView, VkImage clipStateImage,
                VkImageView clipStateView, const std::array<float, 3>& whiteBalanceRgb, bool highlightRecoveryEnabled,
                const StillDistortionCorrection& distortion = StillDistortionCorrection{},
                VkQueryPool timingPool = VK_NULL_HANDLE, bool preserveReconstructedHighlights = false,
                uint32_t highlightMethod = 0, float highlightThreshold = 1.0f,
                float highlightCompression = 100.0f, float exposureGain = 1.0f,
                const rawr::shading::LensShadingMapView& shading = {}, uint32_t cfaPattern = 0,
                const DenoiseRequest& denoise = DenoiseRequest{}, rawr::highlight::ColoroppSensorGeometry geometry = {});
    [[nodiscard]] uint64_t fccAllocatedBytes() const noexcept;
    // Final tonemap/film input. The selected image depends on whether FCC and
    // defringe ran; record() sets these non-owning handles for the frame.
    [[nodiscard]] VkImage outputImage() const noexcept {
        return finalImage_ ? finalImage_ : wbImage_.image;
    }
    [[nodiscard]] VkImageView outputView() const noexcept {
        return finalView_ ? finalView_ : wbImage_.view;
    }
    [[nodiscard]] VkImageView sdrOutputView() const noexcept;
    // Image twin of sdrOutputView() for passes that record their own
    // barriers (GALOSH-YUV in-place tap). Same coloropp/output switch.
    [[nodiscard]] VkImage sdrOutputImage() const noexcept;
    // When set, record() skips the Inpaint Opposed SDR tone tap and the SDR
    // output is outputView(); the caller applies the tap itself (the video
    // tonemap does, with the same compression and exposure gain).
    void setDeferColoroppTone(bool defer) noexcept { deferColoroppTone_ = defer; }
    // Forwarded to the wavelet denoise (raw_denoise::DenoisePipeline::
    // setTileBoundary): lets the caller split the denoise into one GPU
    // submission per tile. Clear it after record() if it captures locals.
    void setDenoiseTileBoundary(std::function<void()> boundary) { denoiseTileBoundary_ = std::move(boundary); }
    // Creates the Inpaint Opposed pipelines ahead of the first record() that needs them.
    void prepareColoropp();
    // True when the last record() used Inpaint Opposed and so needs the tap.
    [[nodiscard]] bool coloroppToneRequired() const noexcept { return coloroppActive_; }

   private:
    void createWbPipeline();
    void createHighlightPipeline();
    void createHighlightGuidePipelines();
    void createDeriveClipPipeline();
    void createDefringePipeline();
    void createPredicateBuffer(VkPhysicalDevice physicalDevice);
    void destroyWbPipeline() noexcept;
    void destroyHighlightPipeline() noexcept;
    void destroyHighlightGuidePipelines() noexcept;
    void destroyDeriveClipPipeline() noexcept;
    void destroyDefringePipeline() noexcept;
    void destroyPredicateBuffer() noexcept;
    static std::vector<uint32_t> fccShader(std::string_view name);

    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    uint32_t width_ = 0, height_ = 0, fccSteps_ = 1;
    uint32_t guideWidth_ = 0, guideHeight_ = 0;
    rawr::vk::OwnedImage wbImage_{};
    rawr::vk::OwnedImage derivedClipState_{};
    bool wbImageInitialized_ = false;
    bool derivedClipInitialized_ = false;
    VkDescriptorSetLayout wbDsl_ = VK_NULL_HANDLE;
    VkPipelineLayout wbLayout_ = VK_NULL_HANDLE;
    VkPipeline wbPipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool wbPool_ = VK_NULL_HANDLE;
    VkDescriptorSet wbSet_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout hlDsl_ = VK_NULL_HANDLE;
    VkPipelineLayout hlLayout_ = VK_NULL_HANDLE;
    VkPipeline hlPipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool hlPool_ = VK_NULL_HANDLE;
    VkDescriptorSet hlSet_ = VK_NULL_HANDLE;
    std::unique_ptr<rawr::highlight::GuideChainPipelines> guidePipelines_;
    std::unique_ptr<rawr::highlight::GuideChain> guideChain_;
    VkDescriptorSetLayout deriveClipDsl_ = VK_NULL_HANDLE;
    VkPipelineLayout deriveClipLayout_ = VK_NULL_HANDLE;
    VkPipeline deriveClipPipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool deriveClipPool_ = VK_NULL_HANDLE;
    VkDescriptorSet deriveClipSet_ = VK_NULL_HANDLE;
    // Axial-defringe pass (linear RGB, after optional FCC). Final handles
    // select the output without a full-frame copy.
    VkDescriptorSetLayout defringeDsl_ = VK_NULL_HANDLE;
    VkPipelineLayout defringeLayout_ = VK_NULL_HANDLE;
    VkPipeline defringePipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool defringePool_ = VK_NULL_HANDLE;
    VkDescriptorSet defringeSet_ = VK_NULL_HANDLE;
    VkImage finalImage_ = VK_NULL_HANDLE;
    VkImageView finalView_ = VK_NULL_HANDLE;
    bool defringeEnabled_ = false;
    float defringeStrength_ = 0.0f;
    float defringeEdgeThreshold_ = 0.02f;
    float defringeLumaFloor_ = 0.08f;
    // Highlight-guide predicate: one storage buffer holding the flag word at
    // byte 0. The WB pass sets the flag when any pixel may need
    // reconstruction; the guide chain dispatches are predicated on it via
    // VK_EXT_conditional_rendering when available (otherwise they run
    // unconditionally, which is always correct).
    VkBuffer predBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory predMemory_ = VK_NULL_HANDLE;
    PFN_vkCmdBeginConditionalRenderingEXT beginConditional_ = nullptr;
    PFN_vkCmdEndConditionalRenderingEXT endConditional_ = nullptr;
    // Profiled wavelet denoise (linear RGB, pre-WB). Owns its output image;
    // when enabled, downstream stages consume the denoised view. Created
    // lazily on the first enabled record (strength <= 0 skips everything).
    std::unique_ptr<raw_denoise::DenoisePipeline> denoise_;
    std::function<void()> denoiseTileBoundary_;
    rawr::vk::OwnedImage denoiseOut_{};
    bool denoiseOutInitialized_ = false;
    std::unique_ptr<::fcc::FalseColorCorrectionPipeline> fcc_;
    std::unique_ptr<rawr::highlight::ColoroppProcessor> coloropp_;
    bool coloroppActive_ = false;
    bool deferColoroppTone_ = false;
};

}  // namespace rawr::post
