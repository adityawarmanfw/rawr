#pragma once
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>

#include "rawr/vk/GpuContext.h"
#include "rawr/vk/OwnedImage.h"

namespace rawr::raw_ingress {

// GPU Bayer downscales for high-resolution photo frames:
//  - bin(): 2x Bayer -> Bayer bin (shaders/bayer_lin.comp). A fixed 12x12 kernel per output
//    site on the white-balanced source (fitted to an anti-aliased 2x downscale, the sensor's
//    4-pixel grid lines and 8x8 diagonal grid nulled), plus a sharper kernel blended in only
//    where the detail energy is clearly above the frame's own noise floor for that brightness.
//    RAW16 rows in a storage buffer in, an R16_UINT image of half the size per side out, codes
//    scaled by codeGain (4 in capture; no dither).
//  - resample(): the DNG converter's BayerResample (same-phase Catmull-Rom, clamped) between two
//    R16_UINT images, for scales in [1, 1.5] (50 MP -> 25 MP).
// Scratch (shared by every slot, 4 bytes per output pixel): the plain result and the sharpening
// term. The caller owns the source buffer, the images, their layouts (GENERAL) and submission.
class BayerBinner final {
   public:
    static constexpr std::uint32_t kSlots = 3;

    static std::unique_ptr<BayerBinner> create(const rawr::vk::GpuContext& context);
    ~BayerBinner();
    BayerBinner(const BayerBinner&) = delete;
    BayerBinner& operator=(const BayerBinner&) = delete;

    struct Bin {
        VkBuffer source = VK_NULL_HANDLE;           // RAW16 rows, storage buffer
        VkImageView destination = VK_NULL_HANDLE;   // R16_UINT, width/2 x height/2, GENERAL
        std::uint32_t width = 0, height = 0;        // multiples of 4, at least 32
        std::uint32_t rowStrideBytes = 0;
        std::array<float, 4> black{};               // source codes by buffer phase 2*(y&1)+(x&1)
        std::array<float, 4> wbGains{};             // camera-neutral gains by buffer phase, any common scale
        float white = 0;                            // source white level
        std::uint32_t order = 0;                    // CfaPattern code (RGGB 0, GRBG 1, GBRG 2, BGGR 3)
        float codeGain = 4.0f;                      // white * codeGain must fit 16 bits
        // Optional: timestamps 0 and 1 around the passes (profiling).
        VkQueryPool timestamps = VK_NULL_HANDLE;
    };
    // Scratch is (re)allocated when the size changes, which requires every earlier submission
    // to have retired, as does the slot's.
    void bin(VkCommandBuffer command, std::uint32_t slot, const Bin& frame);

    struct Resample {
        VkImageView source = VK_NULL_HANDLE, destination = VK_NULL_HANDLE;  // R16_UINT, GENERAL
        std::uint32_t inWidth = 0, inHeight = 0, outWidth = 0, outHeight = 0;  // outputs even
        float maxCode = 65535.0f;
    };
    void resample(VkCommandBuffer command, std::uint32_t slot, const Resample& frame);

    // Scratch bytes for an output size.
    static std::uint64_t scratchBytes(std::uint32_t outWidth, std::uint32_t outHeight);
    // Noise variance of an output pixel of colour `outColour` (0 R, 1 G, 2 B) relative to one source
    // pixel's, for a flat area (the plain kernel), with white-balance gains (R, G, B): a source of
    // colour c carries variance var_c * g_c^2 into the balanced domain, the output is divided by its
    // own colour's gain. Assumes equal source variance in every colour.
    static double noiseVarianceGain(std::uint32_t outColour, const std::array<float, 3>& gains);

   private:
    explicit BayerBinner(const rawr::vk::GpuContext& context);
    void destroy() noexcept;
    void ensureScratch(std::uint32_t outWidth, std::uint32_t outHeight);
    void releaseScratch() noexcept;

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
    Pipeline bin_, resample_;
    std::array<VkPipeline, 4> binPasses_{};
    VkBuffer kernels_ = VK_NULL_HANDLE;  // plain and sharpening weights (uniform buffer)
    VkDeviceMemory kernelsMemory_ = VK_NULL_HANDLE;
    struct Scratch {
        rawr::vk::OwnedImage base, d;
        VkBuffer stats = VK_NULL_HANDLE;  // histogram, then the per-band noise floors
        VkDeviceMemory statsMemory = VK_NULL_HANDLE;
        std::uint32_t width = 0, height = 0;
    } scratch_;
};

}  // namespace rawr::raw_ingress
