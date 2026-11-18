#pragma once
#include <rawr/raw_ingress/RawRemosaicker.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

#include "capture/StripRemosaic.h"
#include "imaging/CfaRemosaic.h"
#include "vulkan/VulkanContext.h"

namespace rawrcam::capture {
// GPU remosaic of one full-size block-CFA RAW16 frame held in a CPU buffer:
// upload, remosaic to Bayer, read the result back into the same buffer.
// Same output as imaging::remosaicBlockCfa, without the CPU cost. The frame
// goes through in horizontal strips (StripRemosaic.h: bit-identical to a
// whole-frame run), which keeps the
// GPU memory at about 1/strips of a whole-frame run. Owns one strip-sized
// source/result buffer (the shader only reads it for the initial copy, so the
// Bayer result is read back into it), one output image and the remosaicker
// scratch; a run is synchronous. A run that throws before writing anything
// leaves the buffer untouched (callers fall back to the CPU path); one that
// fails after the first strip throws PartialFailure, which must not fall back.
class BlockRemosaicGpu final {
   public:
    struct PartialFailure : std::runtime_error {
        using std::runtime_error::runtime_error;
    };
    BlockRemosaicGpu(const vulkan::VulkanContext&, uint32_t width, uint32_t height, uint32_t strips = 4);
    ~BlockRemosaicGpu();
    BlockRemosaicGpu(const BlockRemosaicGpu&) = delete;
    BlockRemosaicGpu& operator=(const BlockRemosaicGpu&) = delete;
    // The shader covers Fast/Standard for any block and High/Detail for quad only.
    static bool supports(uint32_t block, imaging::CfaRemosaicQuality);
    // `black` is the mean raw black level in sample units (Detail regulariser).
    void run(std::vector<uint8_t>& pixels, uint32_t block, uint32_t order, imaging::CfaRemosaicQuality quality,
             float black);

   private:
    void runWindow(uint32_t block, uint32_t order, imaging::CfaRemosaicQuality, float black);
    void destroy() noexcept;
    const vulkan::VulkanContext& vk_;
    uint32_t width_, height_;
    StripLayout layout_;
    uint32_t windowHeight_ = 0;
    size_t bytes_;           // one strip window
    size_t frameBytes_;
    VkBuffer buffer_{};
    VkDeviceMemory memory_{};
    void* mapped_{};
    rawr::vk::OwnedImage output_;
    VkCommandPool pool_{};
    VkCommandBuffer command_{};
    VkFence fence_{};
    std::unique_ptr<rawr::raw_ingress::RawRemosaicker> remosaic_;
};
}  // namespace rawrcam::capture
