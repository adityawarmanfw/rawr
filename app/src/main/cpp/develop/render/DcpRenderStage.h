#pragma once
#include <array>

#include "color/DcpProfile.h"
#include "rawr/vk/DeviceObjects.h"
namespace rawrcam::develop::rendered {
// One instance per in-flight frame. Call only after that frame's fence retires.
// Edits white-balanced camera RGB in place, returning to the same basis so
// film, tonemap and HDR consumers keep their existing color-space contracts.
class DcpRenderStage final {
   public:
    DcpRenderStage(VkPhysicalDevice physical, VkDevice device);
    void record(VkCommandBuffer command, const color::DcpProfilePtr& profile, const std::array<float, 9>& cameraToSrgb,
                float weight1, uint32_t width, uint32_t height, VkImageView first, VkImageView second = VK_NULL_HANDLE);

   private:
    VkPhysicalDevice physical_;
    VkDevice device_;
    rawr::vk::DeviceObjects objects_, storage_;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, 2> sets_{};
    VkBuffer buffer_ = VK_NULL_HANDLE;
    color::DcpProfilePtr profile_;
};
}  // namespace rawrcam::develop::rendered
