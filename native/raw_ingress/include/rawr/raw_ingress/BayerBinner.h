#pragma once
#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "rawr/raw_ingress/BayerBinSize.h"
#include "rawr/vk/GpuContext.h"

namespace rawr::raw_ingress {

// GPU Bayer downscales for high-resolution photo frames:
//  - bin(): Bayer -> Bayer downscale by 2 (shaders/bayer_lin.comp, 12x12 kernels) or 3 per side
//    (shaders/bayer_bin3.comp, 17x17), one pass. A fixed kernel per output site on the white-balanced
//    source (fitted to an anti-aliased downscale, the sensor's 4-pixel grid lines and 8x8 diagonal grid
//    nulled, colours kept apart at low frequencies), clamped to the nearby same-colour source samples.
//    No sharpening. RAW16 rows in a storage buffer in, an R16_UINT image of outputSize() out, codes
//    scaled by codeGain (4 in capture; no dither).
//  - resample(): the DNG converter's BayerResample (same-phase Catmull-Rom, clamped) between two
//    R16_UINT images, for scales in [1, 1.5] (50 MP -> 25 MP).
// No scratch. The caller owns the source buffer, the images, their layouts (GENERAL) and submission.
class BayerBinner final {
   public:
    static constexpr std::uint32_t kSlots = 3;

    static std::unique_ptr<BayerBinner> create(const rawr::vk::GpuContext& context);
    ~BayerBinner();
    BayerBinner(const BayerBinner&) = delete;
    BayerBinner& operator=(const BayerBinner&) = delete;

    // Output size of a bin (rawr/raw_ingress/BayerBinSize.h).
    using Size = BayerBinSize;
    static Size outputSize(std::uint32_t width, std::uint32_t height, std::uint32_t ratio) {
        return bayerBinOutputSize(width, height, ratio);
    }

    struct Bin {
        VkBuffer source = VK_NULL_HANDLE;           // RAW16 rows, storage buffer
        VkImageView destination = VK_NULL_HANDLE;   // R16_UINT, outputSize(width, height, ratio), GENERAL
        std::uint32_t width = 0, height = 0;        // multiples of 4, at least 32
        std::uint32_t rowStrideBytes = 0;
        std::array<float, 4> black{};               // source codes by buffer phase 2*(y&1)+(x&1)
        std::array<float, 4> wbGains{};             // camera-neutral gains by buffer phase, any common scale
        float white = 0;                            // source white level
        std::uint32_t order = 0;                    // CfaPattern code (RGGB 0, GRBG 1, GBRG 2, BGGR 3)
        float codeGain = 4.0f;                      // white * codeGain must fit 16 bits
        std::uint32_t ratio = 2;                    // 2 or 3
        // Optional: timestamps 0 and 1 around the passes (profiling).
        VkQueryPool timestamps = VK_NULL_HANDLE;
    };
    // The slot's earlier submission must have retired.
    void bin(VkCommandBuffer command, std::uint32_t slot, const Bin& frame);

    struct Resample {
        VkImageView source = VK_NULL_HANDLE, destination = VK_NULL_HANDLE;  // R16_UINT, GENERAL
        std::uint32_t inWidth = 0, inHeight = 0, outWidth = 0, outHeight = 0;  // outputs even
        float maxCode = 65535.0f;
    };
    void resample(VkCommandBuffer command, std::uint32_t slot, const Resample& frame);

    // Noise variance of an output pixel of colour `outColour` (0 R, 1 G, 2 B) relative to one source
    // pixel's, for a flat area, with white-balance gains (R, G, B): a source of colour c carries
    // variance var_c * g_c^2 into the balanced domain, the output is divided by its own colour's gain.
    // Assumes equal source variance in every colour (the clamp is ignored: it acts at edges).
    static double noiseVarianceGain(std::uint32_t outColour, const std::array<float, 3>& gains, std::uint32_t ratio = 2);

   private:
    explicit BayerBinner(const rawr::vk::GpuContext& context);
    void destroy() noexcept;

    struct Pipeline {
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        std::array<VkDescriptorSet, kSlots> sets{};
    };
    void makeLayout(Pipeline& p, const VkDescriptorType* types, std::uint32_t count, std::uint32_t pushBytes);
    VkPipeline makeComputePipeline(VkPipelineLayout layout, const unsigned char* spirv, std::size_t spirvBytes);
    rawr::vk::GpuContext context_;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    void makeKernels(const float* weights, std::size_t bytes, VkBuffer& buffer, VkDeviceMemory& memory);
    Pipeline bin_, bin3_, resample_;
    VkBuffer kernels_ = VK_NULL_HANDLE, kernels3_ = VK_NULL_HANDLE;  // weights (uniform buffers)
    VkDeviceMemory kernelsMemory_ = VK_NULL_HANDLE, kernels3Memory_ = VK_NULL_HANDLE;
};

}  // namespace rawr::raw_ingress
