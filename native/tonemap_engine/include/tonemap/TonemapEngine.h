#pragma once
#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "tonemap/TonemapMath.h"
#include "tonemap/Types.h"
#include "tonemap/Version.h"
#include "tonemap/lut/LutChain.h"

namespace tonemap {

struct TonemapCreateInfo {
    VulkanContext context{};
    const uint32_t* shaderSpirv = nullptr;
    size_t shaderSpirvBytes = 0;
    // Must match the supplied shader variant. RGBA16F preserves encoded SDR
    // precision for a later GPU 10-bit YCbCr conversion. RGBA32F is available
    // for numerical validation before quantization.
    VkFormat outputFormat = VK_FORMAT_R8G8B8A8_UNORM;
    uint32_t workgroupSizeX = 8;
    uint32_t workgroupSizeY = 8;
    uint32_t maxFramesInFlight = 3;
    TonemapConfig config = TonemapPresets::NeutralBaseline().config;

    // Optional immutable user LUT configuration for this engine instance.
    // Iteration 2 keeps LUT resources creation-time immutable so no in-flight
    // descriptor/resource replacement can race preview frames. App-level LUT
    // switching is added in the next integration iteration.
    const lut::LutChain* lutChain = nullptr;
    // The video variant takes RAW_SENSOR directly and writes recording and
    // monitor outputs in one dispatch. The ordinary RGB API stays unchanged.
    bool rawVideoInput = false;
    // Linear-RGB video variant writes the same encoded (SDR or LOG) pixels to a separate
    // 8-bit monitor image while preserving RGBA16F for the encoder.
    bool videoMonitorOutput = false;
    // Must match RAWR_TONEMAP_NEUTRAL_TEXTURE in the supplied shader. Buffer
    // storage remains the default; callers select it when capability/timing
    // checks reject textures. Upload happens once at creation, never per frame.
    bool neutralLutTexture = false;
    // Must match RAWR_TONEMAP_USER_TEXTURE; includes all chain stages.
    bool userLutTexture = false;
    // Required for textures. Caller externally synchronizes access to this
    // queue during creation; it must belong to lutUploadQueueFamily.
    VkQueue lutUploadQueue = VK_NULL_HANDLE;
    uint32_t lutUploadQueueFamily = 0;
};

struct TonemapRawFrame {
    VkImageView rawImageView = VK_NULL_HANDLE;
    VkBuffer rawBuffer = VK_NULL_HANDLE;
    uint32_t rawStridePixels = 0;
    uint32_t width = 0, height = 0, cfa = 0;
    float black[4]{};
    float white = 0.0f;
    float whiteBalance[4]{};
    const float* lensShading = nullptr;
    size_t lensShadingCount = 0;
    uint32_t lensShadingWidth = 0, lensShadingHeight = 0;
};

struct TonemapRecordInfo {
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    LinearRgbImageView input{};
    SrgbImageView output{};
    SrgbImageView monitor{};
    bool monitorEnabled = false;
    uint32_t frameSlot = 0;
    // Video monitor variants only: Inpaint Opposed SDR highlight compression
    // applied to the input (coloropp_tone.comp semantics, already mapped by
    // coloroppToneCompression/coloroppToneExposureGain). 0 disables it.
    float highlightCompression = 0.0f;
    float highlightExposureGain = 1.0f;

    // Complete transform from the exact upstream scene-linear camera-RGB
    // representation to linear ACEScg/AP1. Column-major, 9 floats. The caller
    // composes producer-specific baseline WB here when it is not already
    // represented in the RGB input. TonemapEngine never consumes CFA data.
    const float* cameraToWorkingColumnMajor3x3 = nullptr;
    // Per-frame controls. aePostGain is supplied externally by the exposure
    // controller through the application; TonemapEngine does not derive AE.
    TonemapParams params = TonemapPresets::NeutralBaseline().params;
};

// The Grade sliders packed for the UltraHDR HDR tap (gainmap.comp), std140
// vec4 x3: tone (Contrast, Highlights, Shadows, Midtones, -1..1), color
// (saturation, vibrance, vibrance strength, guard start), space (guard end,
// pivot, black, transfer id). In the grade space the engine renders with.
using GradeTapBlock = std::array<float, 12>;

struct TonemapRawRecordInfo {
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    TonemapRawFrame raw{};
    SrgbImageView output{};
    SrgbImageView monitor{};
    bool monitorEnabled = true;
    uint32_t frameSlot = 0;
    const float* cameraToWorkingColumnMajor3x3 = nullptr;
    TonemapParams params = TonemapPresets::NeutralBaseline().params;
};

class TonemapEngine {
   public:
    static bool validateCreateInfo(const TonemapCreateInfo& createInfo, const char** reason = nullptr) noexcept;
    static bool supportsNeutralLutTexture(VkPhysicalDevice physicalDevice) noexcept;
    static bool validateRecordInfo(const TonemapRecordInfo& recordInfo, uint32_t maxFramesInFlight,
                                   const char** reason = nullptr) noexcept;

    explicit TonemapEngine(const TonemapCreateInfo& createInfo);
    ~TonemapEngine();
    TonemapEngine(const TonemapEngine&) = delete;
    TonemapEngine& operator=(const TonemapEngine&) = delete;
    TonemapEngine(TonemapEngine&&) = delete;
    TonemapEngine& operator=(TonemapEngine&&) = delete;

    // Records only. No command-buffer begin/end, queue submit, fence wait,
    // device-idle call, image transition, or per-frame allocation.
    // The caller owns input/output images, synchronization and submission.
    // A frame slot must not be reused until prior GPU work using it completes.
    void record(const TonemapRecordInfo& recordInfo);
    void recordRaw(const TonemapRawRecordInfo& recordInfo);

    const TonemapConfig& config() const noexcept;
    // nullopt for AgX/LOG (no grade) or when every Grade slider is neutral.
    std::optional<GradeTapBlock> gradeTapBlock(const TonemapParams& params) const noexcept;
    uint32_t maxFramesInFlight() const noexcept;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace tonemap
