#pragma once
#include <vulkan/vulkan.h>

#include <cstdint>

namespace gainmap {

// Scene-linear HDR input only. Values >1.0 are valid. Must be the
// post-demosaic / post-WB tonemap input: white-balanced *camera* RGB
// (pre-tonemap). The caller supplies the calibrated camera->linear-sRGB
// matrix (GainmapParams::hdrToLinearSrgbRowMajor); the AP1->sRGB default
// only fits an AP1 working space and must be overridden per camera.
struct HdrImageView {
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
    uint32_t width = 0;
    uint32_t height = 0;
};

// Finished SDR base (sRGB-encoded RGBA8). This is the exact JPEG source
// (tonemap or film output image).
struct SdrImageView {
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
    uint32_t width = 0;
    uint32_t height = 0;
};

// Sensor clip-state mask (R16UI bitmask, half res on the same grid as the
// gain map: one texel per 2x2 base block). Nonzero = clipped at the sensor.
// Uniform contract across producers (sensor CFA, shared highlight runtime,
// post-demosaic derive, multiframe prepare mask).
struct ClipImageView {
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_R16_UINT;
    VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
    uint32_t width = 0;
    uint32_t height = 0;
};

// Film glow factor (RGBA16F, full res on the HDR-tap grid). Dimensionless
// post/pre scatter quotient (~1.0 where the film look adds no glow), so
// film response and exposure cancel and no per-stock scale is needed.
// Multiplies the HDR tap before the CST: halation/camera-diffusion glow
// then carries HDR headroom with hue instead of suppressing the gain.
// Optional; null view disables (pure scene tap).
struct GlowImageView {
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
    uint32_t width = 0;
    uint32_t height = 0;
};

// Gain map output. 8-bit recovery map (RGBA8, A ignored): gray (R=G=B)
// encoding the Rec.709 luminance ratio in single-channel mode, per-channel
// RGB ratios in multi-channel mode (default on; ISO flags it multi-channel
// while XMP stays channel-identical so single-channel readers keep parsing
// channel 0). Dimensions are base/scale (full res when the caller sets
// GainmapParams::fullResMap, half res legacy).
struct MapImageView {
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct VulkanContext {
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    const VkAllocationCallbacks* allocator = nullptr;
};

}  // namespace gainmap
