#pragma once
#include <android/hardware_buffer.h>
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
// quad remosaic (a 25 MP output then resamples the half-size result), Bayer through the linear bins
// (raw_ingress BayerBinner: 2x, or 3x for the smaller size). width x height is the source, the output
// size comes from reducedRawSize().
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
    // The slot's mapped upload buffer (width * height packed RAW16), to decode a frame straight into it;
    // then submitUploaded with an upper bound of its codes (1023 for RAW10). Throws while the slot is busy.
    uint16_t* uploadBuffer(uint32_t slot);
    void submitUploaded(uint32_t slot, const metadata::FrameMetadataSnapshot&, uint16_t sourceMaximum);
    // Zero copy: imports the camera buffer (packed RAW10, rowStrideBytes apart, no pending fence) as a storage
    // buffer and reduces it in place. The caller keeps the camera frame leased until collect(slot). False, with
    // the reason in error and nothing left behind, when the buffer cannot be imported: use the CPU paths.
    bool submitImported(uint32_t slot, AHardwareBuffer* ahb, uint32_t rowStrideBytes,
                        const metadata::FrameMetadataSnapshot&, std::string* error);
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
        // Camera buffer imported for the submission in flight (zero copy); released at collect.
        VkBuffer imported{};
        VkDeviceMemory importedMemory{};
    };
    void initialize(uint32_t slots);
    float sourceCodeLimit(const Slot&, const metadata::FrameMetadataSnapshot&) const;
    void record(Slot&, uint32_t slot, const metadata::FrameMetadataSnapshot&, VkBuffer source, uint32_t rowStrideBytes,
                bool raw10);
    // The 400 MB upload buffer is made on first CPU use: a zero-copy burst never needs it.
    void ensureSource(Slot&);
    void releaseImport(Slot&) noexcept;
    void destroy() noexcept;
    void allocate(Buffer&, size_t, VkBufferUsageFlags);
    const vulkan::VulkanContext& vk_;
    uint32_t width_, height_, outWidth_, outHeight_;
    bool direct3x_ = false;  // Bayer at the smaller size: the direct 3x bin, no resample
    size_t bytes_;
    VkCommandPool pool_{};
    std::array<Slot, 2> slots_{};
    std::unique_ptr<rawr::raw_ingress::RawRemosaicker> remosaic_;
    std::unique_ptr<rawr::raw_ingress::BayerBinner> binner_;
};
}  // namespace rawrcam::capture
