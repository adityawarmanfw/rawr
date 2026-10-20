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

    static std::unique_ptr<RawRemosaicker> create(const rawr::vk::GpuContext& context);
    ~RawRemosaicker();
    RawRemosaicker(const RawRemosaicker&) = delete;
    RawRemosaicker& operator=(const RawRemosaicker&) = delete;

    struct Frame {
        VkBuffer source = VK_NULL_HANDLE;
        VkImageView destination = VK_NULL_HANDLE;  // GENERAL layout
        std::uint32_t width = 0, height = 0;       // even; RAW10 width a multiple of 4
        std::uint32_t rowStrideBytes = 0;
        bool raw10 = false;
        std::uint32_t block = 2;  // 2 or 4
        std::uint32_t order = 0;  // CfaPattern code of the blocks (and the Bayer out)
        // Optional: timestamps 0..3 around raw-in, green + block diff, Bayer out (profiling).
        VkQueryPool timestamps = VK_NULL_HANDLE;
    };
    // Records the three passes. Scratch is (re)allocated when the size
    // changes, which requires every earlier submission to have retired; the
    // slot's previous submission must have retired too (its set is rewritten
    // when the bindings change).
    void record(VkCommandBuffer command, std::uint32_t slot, const Frame& frame);

   private:
    explicit RawRemosaicker(const rawr::vk::GpuContext& context);
    void destroy() noexcept;
    void ensureScratch(std::uint32_t width, std::uint32_t height, std::uint32_t block);
    void releaseScratch() noexcept;

    struct Binding {
        VkBuffer source = VK_NULL_HANDLE;
        VkImageView destination = VK_NULL_HANDLE;
        VkImageView raw = VK_NULL_HANDLE;
    };
    struct Scratch {
        rawr::vk::OwnedImage raw, green;
        VkBuffer diff = VK_NULL_HANDLE;
        VkDeviceMemory diffMemory = VK_NULL_HANDLE;
        std::uint32_t width = 0, height = 0, block = 0;
    };
    rawr::vk::GpuContext context_;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kSlots> sets_{};
    std::array<Binding, kSlots> bound_{};
    Scratch scratch_;
};

}  // namespace rawr::raw_ingress
