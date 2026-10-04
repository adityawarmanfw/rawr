#pragma once
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>

#include "rawr/vk/GpuContext.h"

namespace rawr::raw_ingress {

// GPU RAW10 ingress: unpacks a MIPI RAW10 camera buffer (imported as a
// storage buffer) into an R16_UINT storage image, replacing the CPU
// lock/unpack/upload. Owns its pipeline and one descriptor set per frame
// slot; the caller owns the buffers, images, barriers and submission.
class Raw10Unpacker final {
   public:
    static constexpr std::uint32_t kSlots = 3;

    static std::unique_ptr<Raw10Unpacker> create(const rawr::vk::GpuContext& context);
    ~Raw10Unpacker();
    Raw10Unpacker(const Raw10Unpacker&) = delete;
    Raw10Unpacker& operator=(const Raw10Unpacker&) = delete;

    // Records the unpack of `source` into `destination` (GENERAL layout).
    // width must be a multiple of 4 and rowStrideBytes >= width / 4 * 5.
    // The slot's previous submission must have retired: its descriptor set
    // is rewritten when the source or destination changes.
    void record(VkCommandBuffer command, std::uint32_t slot, VkBuffer source, VkImageView destination,
                std::uint32_t width, std::uint32_t height, std::uint32_t rowStrideBytes);

   private:
    explicit Raw10Unpacker(const rawr::vk::GpuContext& context);
    void destroy() noexcept;

    struct Binding {
        VkBuffer source = VK_NULL_HANDLE;
        VkImageView destination = VK_NULL_HANDLE;
    };
    rawr::vk::GpuContext context_;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kSlots> sets_{};
    std::array<Binding, kSlots> bound_{};
};

}  // namespace rawr::raw_ingress
