#pragma once
#include <rawr/raw_ingress/BayerBinner.h>
#include <rawr/raw_ingress/RawRemosaicker.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "metadata/FrameMetadataSnapshot.h"
#include "vulkan/VulkanContext.h"

namespace rawrcam::capture {
// Per-capture owner. Two fence-protected upload/readback slots, one ordered queue.
// collect() must precede reuse; destruction retires outstanding submissions.
// Each frame is reduced by its context: a 4x4 block mosaic through the Hexadeca sums and
// quad remosaic, Bayer through the linear 2x bin (raw_ingress BayerBinner); a 25 MP output then resamples the half-size
// result. width x height is the source, the output size comes from reducedRawSize().
class ReducedRaw final {
   public:
    ReducedRaw(const vulkan::VulkanContext&, const metadata::CameraContextMetadata&, uint32_t width, uint32_t height,
               uint32_t slots = 2);
    ~ReducedRaw();
    ReducedRaw(const ReducedRaw&) = delete;
    ReducedRaw& operator=(const ReducedRaw&) = delete;
    // Empty when the device can reduce frames of this size (storage buffer range, image size).
    static std::string deviceUnsupportedReason(const vulkan::VulkanContext&, uint32_t width, uint32_t height);
    void submit(uint32_t slot, const std::vector<uint8_t>&, const metadata::FrameMetadataSnapshot&);
    void collect(uint32_t slot, std::vector<uint8_t>&);

   private:
    struct Buffer {
        VkBuffer buffer{};
        VkDeviceMemory memory{};
        void* mapped{};
    };
    struct Slot {
        Buffer source, result;
        rawr::vk::OwnedImage output;
        rawr::vk::OwnedImage half;  // half-size result before the 25 MP resample
        VkCommandBuffer command{};
        VkFence fence{};
        bool pending = false;
    };
    void initialize(uint32_t slots);
    void destroy() noexcept;
    void allocate(Buffer&, size_t, VkBufferUsageFlags);
    const vulkan::VulkanContext& vk_;
    uint32_t width_, height_, outWidth_, outHeight_;
    size_t bytes_;
    VkCommandPool pool_{};
    std::array<Slot, 2> slots_{};
    std::unique_ptr<rawr::raw_ingress::RawRemosaicker> remosaic_;
    std::unique_ptr<rawr::raw_ingress::BayerBinner> binner_;
};
}  // namespace rawrcam::capture
