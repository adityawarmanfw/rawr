#pragma once
#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
namespace rawr::post {
// Three GPU passes: packed chroma scores, frame reduction, Lab chroma repair.
// The caller retains source/target ownership and serializes reuse after completion.
class LabDefringe final {
   public:
    LabDefringe(VkPhysicalDevice physical, VkDevice device, uint32_t width, uint32_t height);
    ~LabDefringe();
    LabDefringe(const LabDefringe&) = delete;
    LabDefringe& operator=(const LabDefringe&) = delete;
    void record(VkCommandBuffer cmd, VkImageView source, VkImageView target, const float* cameraToSrgbRowMajor,
                float strength);
    uint64_t allocatedBytes() const noexcept;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace rawr::post
