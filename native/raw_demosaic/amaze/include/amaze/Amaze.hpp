// SPDX-License-Identifier: GPL-3.0-or-later
// AMaZE demosaic (Emil Martinec; optimizations Ingo Weyrich), GPU port.
// Provenance and credits: shaders/amaze.wgsl and ../NOTICE.md.
#pragma once
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace amaze {

// Same codes as rcd::BayerPattern.
enum class BayerPattern : uint32_t { RGGB = 0, GRBG = 1, GBRG = 2, BGGR = 3 };

using ShaderProvider = std::function<std::vector<uint32_t>(std::string_view shaderName)>;

struct VulkanContext {
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamilyIndex = 0;
    const VkAllocationCallbacks* allocator = nullptr;
    VkPipelineCache pipelineCache = VK_NULL_HANDLE;
};

// Half-resolution canonical physical {R,G1,G2,B}, black-subtracted and
// white-normalized (1.0 = sensor white). Same contract as rcd::PackedCfaImageView.
struct PackedCfaImageView {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t rawWidth = 0;
    uint32_t rawHeight = 0;
    BayerPattern pattern = BayerPattern::BGGR;
};

struct BufferView {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize range = VK_WHOLE_SIZE;
};

// One float per RAW pixel in the 0..255 linear working domain
// (same contract as rcd::NormalizedBayerBufferView).
struct NormalizedBayerBufferView {
    BufferView buffer{};
    uint32_t width = 0;
    uint32_t height = 0;
    BayerPattern pattern = BayerPattern::BGGR;
};

enum class InputMode : uint32_t { PackedCfaRgba16fImage = 0, NormalizedFloatBuffer = 1 };

struct LinearRgbImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct PipelineConfig {
    uint32_t width = 0;
    uint32_t height = 0;
    BayerPattern pattern = BayerPattern::BGGR;
    InputMode inputMode = InputMode::PackedCfaRgba16fImage;
    // AMaZE runs on white-balanced data whose sensor clip sits at 1.0 (its
    // 0.8/1.0 highlight thresholds assume that). Export divides the gains out,
    // so the output is camera-linear like RCD's.
    float outputScale = 1.0f;
    float outputAlpha = 1.0f;
    // Tiles recorded concurrently per pass; each owns ~3.9 MB of scratch.
    uint32_t tileSlots = 16;
};

// Records AMaZE into a caller-owned command buffer on librtprocess's tile grid
// (160 px tiles every 128 px from -16), which the in-place refinement chains
// depend on for parity. Output pixels are written once each; the caller owns
// submission, waiting and the output image.
class AmazePipeline {
   public:
    static bool validateConfig(const PipelineConfig& config, const char** reason = nullptr) noexcept;

    AmazePipeline(VulkanContext context, ShaderProvider shaderProvider, PipelineConfig config);
    ~AmazePipeline();
    AmazePipeline(const AmazePipeline&) = delete;
    AmazePipeline& operator=(const AmazePipeline&) = delete;

    // whiteBalance: camera RGB gains applied before AMaZE (green normally 1).
    void record(VkCommandBuffer commandBuffer, const PackedCfaImageView& input, const LinearRgbImage& output,
                const std::array<float, 3>& whiteBalance);
    void record(VkCommandBuffer commandBuffer, const NormalizedBayerBufferView& input, const LinearRgbImage& output,
                const std::array<float, 3>& whiteBalance);

    // Optional hook run after each tile batch's barrier; the caller may submit
    // what is recorded so far and re-begin the same command buffer.
    void setPassBoundary(std::function<void()> boundary);
    // Alternative hook for double-buffered submission: returns the command
    // buffer recording continues in (it may differ from the one passed to
    // record()). Takes precedence over setPassBoundary.
    void setCommandBoundary(std::function<VkCommandBuffer()> boundary);

    const PipelineConfig& config() const;
    uint64_t currentAllocatedBytes() const;
    uint64_t peakAllocatedBytes() const;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace amaze
