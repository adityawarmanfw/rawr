#pragma once
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>

#include "rawr/vk/GpuContext.h"
#include "rawr/vk/OwnedImage.h"

namespace rawr::raw_ingress {

// GPU remosaic of a block CFA (2x2 quad or 4x4 same-colour blocks) to Bayer,
// read straight from the camera buffer (RAW16 rows, or MIPI RAW10) into an
// R16_UINT storage image. Owns its pipeline, one descriptor set per frame
// slot and the scratch (raw and green R16 images, block colour differences),
// shared by every slot (the recorded barriers order successive frames on the
// queue). The caller owns the source buffer, the destination image, its
// layout barriers and submission. The source must allow TRANSFER_SRC reads
// (RAW16 is copied into the raw image) as well as storage reads (RAW10).
class RawRemosaicker final {
   public:
    static constexpr std::uint32_t kSlots = 3;
    // 16 surfaces x 6 quadratic terms, then 16 surfaces x 32 x 24 residual grid nodes (QuadGainField::kFloats).
    static constexpr std::uint32_t kGainFieldFloats = 96 + 16 * 32 * 24;

    struct Pipelines;
    // Immutable, device-owned programs; keep alive across captures, destroy before the device.
    static std::shared_ptr<Pipelines> prepare(const rawr::vk::GpuContext&, VkPipelineCache cache = VK_NULL_HANDLE);
    static std::unique_ptr<RawRemosaicker> create(const rawr::vk::GpuContext& context,
                                               std::shared_ptr<Pipelines> pipelines = {});
    ~RawRemosaicker();
    RawRemosaicker(const RawRemosaicker&) = delete;
    RawRemosaicker& operator=(const RawRemosaicker&) = delete;

    // Mirrors imaging::CfaRemosaicQuality. High and Detail support Quad only
    // (block 2, or the Hexadeca reduction below).
    enum class Quality : std::uint32_t { Fast = 0, Standard = 1, High = 2, Detail = 3 };
    struct Frame {
        VkBuffer source = VK_NULL_HANDLE;
        VkImageView destination = VK_NULL_HANDLE;  // GENERAL layout
        std::uint32_t width = 0, height = 0;       // even; RAW10 width a multiple of 4
        std::uint32_t rowStrideBytes = 0;
        bool raw10 = false;
        std::uint32_t block = 2;  // 2 or 4
        std::uint32_t order = 0;  // CfaPattern code of the blocks (and the Bayer out)
        Quality quality = Quality::Standard;
        // Standalone Hexadeca reduction: input dimensions above, output is
        // width/2 x height/2. Requires block=4, High or Detail, dimensions divisible by 8.
        // Source codes must be <=16383 (10-bit capture is the tested case).
        // Exact 2x2 sums encode quarter DN; destination and scratch keep these
        // units. No 200MP intermediate image. Caller scales calibration by 4.
        bool hexadecaToQuad = false;
        // Hexadeca reduction only: fit the sensor's per-position response from
        // this frame's own flat blocks and divide it out before the 2x2 sum
        // (imaging/PhaseFit.h). Samples must be <= 4095. `blackLevel` is the raw
        // black in source codes; `fitStride` (odd) thins the statistics lattice
        // (3 keeps full quality at about a ninth of the reads).
        bool phaseFit = false;
        float blackLevel = 64.0f;
        std::uint32_t fitStride = 3;
        // Quad (block 2, RAW16, no reduction) only: divide a per-position gain
        // field out while loading the raw (app imaging/QuadGainField.h:
        // kGainFieldFloats coefficients, copied at record time). `blackLevel`
        // is the raw black. fieldRow0 is this window's first row in a frame of
        // fieldHeight rows (strips); the frame's width is `width`.
        const float* gainField = nullptr;
        std::uint32_t fieldRow0 = 0, fieldHeight = 0;
        // Quad (block 2, RAW16, no reduction) only: correct the phase-detect
        // disparity of a 2x2 on-chip-lens sensor after the load (and the gain
        // field), before the remosaic (app imaging/QuadDisparity.h,
        // shaders/raw_disparity.comp). `blackLevel` is the raw black; the noise
        // model (pixel variance = noiseSlope * (level - black) + noiseFloor, from
        // QuadGainField) sets what the texture gate and confidence treat as noise.
        // Reads 29 rows around each output row: strips need margins of that plus the remosaic's
        // (and must start on a multiple of 8 rows: its 4 x 4-block cells follow the frame's grid).
        bool disparity = false;
        float noiseSlope = 0.0f, noiseFloor = 0.0f;
        // Optional: timestamps 0..3 around raw-in, green + block diff (Detail: green + block moments), Bayer out (profiling);
        // with phaseFit also 5, 6, 7 after block sums, statistics and curve (pool of 8); with disparity
        // 4 after the load and 5 after its gate pass (its correction pass ends at 1).
        VkQueryPool timestamps = VK_NULL_HANDLE;
    };
    // Records the three passes. Scratch is (re)allocated when the size
    // changes, which requires every earlier submission to have retired; the
    // slot's previous submission must have retired too (its set is rewritten
    // on every record, including when handle values have been recycled).
    void record(VkCommandBuffer command, std::uint32_t slot, const Frame& frame);

   private:
    explicit RawRemosaicker(const rawr::vk::GpuContext& context, std::shared_ptr<Pipelines> pipelines);
    void destroy() noexcept;
    void ensureScratch(std::uint32_t width, std::uint32_t height, std::uint32_t block, bool needsDiff,
                       bool needsMoments, bool needsPd);
    void releaseScratch() noexcept;

    struct Scratch {
        bool needsDiff = false, needsMoments = false, needsPd = false;
        rawr::vk::OwnedImage raw, green, moments, pd;
        VkBuffer diff = VK_NULL_HANDLE;
        VkDeviceMemory diffMemory = VK_NULL_HANDLE;
        std::uint32_t width = 0, height = 0, block = 0;
    };
    std::shared_ptr<Pipelines> pipelines_;
    rawr::vk::GpuContext context_;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipeline detailPipeline_ = VK_NULL_HANDLE;
    VkPipeline disparityPipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kSlots> sets_{};
    Scratch scratch_;
};

}  // namespace rawr::raw_ingress
