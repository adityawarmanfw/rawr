#pragma once
#include "Config.hpp"
#include "ShaderProvider.hpp"
#include "Types.hpp"
#include <memory>
namespace fcc {
class FalseColorCorrectionPipeline {
public:
    static bool validateConfig(const PipelineConfig&,const char** reason=nullptr) noexcept;
    FalseColorCorrectionPipeline(VulkanContext,ShaderProvider,PipelineConfig);
    ~FalseColorCorrectionPipeline();
    FalseColorCorrectionPipeline(const FalseColorCorrectionPipeline&)=delete;
    FalseColorCorrectionPipeline& operator=(const FalseColorCorrectionPipeline&)=delete;

    // Input is post-white-balance camera-linear RGB, matching the RawTherapee FCC domain.
    // Output may be a different RGBA16F image. steps must be 1..config.maxSteps.
    // The caller owns input/output image layout transitions to GENERAL.
    void record(VkCommandBuffer,const LinearRgbImage& input,const LinearRgbImage& output,uint32_t steps=1);
    // Same steps without a scratch image: a -> b -> a -> ... Both images are
    // overwritten; the result is in b for odd step counts and in a for even
    // ones (the returned pointer). Pixels are identical to record().
    const LinearRgbImage* recordPingPong(VkCommandBuffer,const LinearRgbImage& a,const LinearRgbImage& b,uint32_t steps);

    const PipelineConfig& config() const noexcept;
    uint64_t currentAllocatedBytes() const noexcept;
    uint64_t peakAllocatedBytes() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
