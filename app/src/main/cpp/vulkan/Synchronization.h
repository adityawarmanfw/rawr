#pragma once
#include <vulkan/vulkan.h>

namespace rawrcam::vulkan {

void acquireForeignImage(VkCommandBuffer command, VkImage image, uint32_t destinationQueueFamily,
                         VkPipelineStageFlags destinationStage, VkAccessFlags destinationAccess);

void releaseForeignImage(VkCommandBuffer command, VkImage image, uint32_t sourceQueueFamily,
                         VkPipelineStageFlags sourceStage, VkAccessFlags sourceAccess);

// Queue-family ownership transfer of an imported camera buffer (whole range).
void acquireForeignBuffer(VkCommandBuffer command, VkBuffer buffer, uint32_t destinationQueueFamily,
                          VkPipelineStageFlags destinationStage, VkAccessFlags destinationAccess);
void releaseForeignBuffer(VkCommandBuffer command, VkBuffer buffer, uint32_t sourceQueueFamily,
                          VkPipelineStageFlags sourceStage, VkAccessFlags sourceAccess);

void computeWriteToComputeRead(VkCommandBuffer command, VkImage image);
void computeWriteToComputeWrite(VkCommandBuffer command, VkImage image);
void computeWriteToFragmentRead(VkCommandBuffer command, VkImage image);
void fragmentReadToComputeWrite(VkCommandBuffer command, VkImage image);
void transferWriteToComputeRead(VkCommandBuffer command, VkImage image);

}  // namespace rawrcam::vulkan
