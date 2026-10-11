#pragma once
#include "Config.hpp"
#include "ShaderProvider.hpp"
#include "Telemetry.hpp"
#include "Types.hpp"
#include <array>
#include <functional>
#include <memory>
namespace dual {
class DualDemosaicPipeline {
public:
    static bool validateConfig(const PipelineConfig&,const PipelineAssets& assets={},const char** reason=nullptr) noexcept;
    DualDemosaicPipeline(VulkanContext,ShaderProvider,PipelineConfig,PipelineAssets={});
    ~DualDemosaicPipeline();
    DualDemosaicPipeline(const DualDemosaicPipeline&)=delete;
    DualDemosaicPipeline& operator=(const DualDemosaicPipeline&)=delete;
    void record(VkCommandBuffer,const RawCfaImageView&,const LinearRgbImage&);
    void record(VkCommandBuffer,const NormalizedBayerBufferView&,const LinearRgbImage&);
    void record(VkCommandBuffer,const PackedCfaImageView&,const LinearRgbImage&);
    const PipelineConfig& config() const;
    // Optional hook run between recorded passes (including the RCD and VNG4 sub-pipeline
    // passes); the caller may submit and re-begin the same command buffer to bound one GPU
    // submission. See rcd::RcdPipeline::setPassBoundary.
    void setPassBoundary(std::function<void()> boundary);
    // Double-buffered alternative for the AMaZE and blend passes: returns the
    // command buffer recording continues in. RCD/VNG4 sub-pipelines keep using
    // setPassBoundary (same buffer).
    void setCommandBoundary(std::function<VkCommandBuffer()> boundary);
    // Primary::Amaze: camera RGB gains applied before AMaZE (green normally 1).
    void setWhiteBalance(const std::array<float,3>& rgb);
    // Runtime setting; call only between record() submissions for this single-flight pipeline.
    // Uses RawTherapee slider units (0..100); 0 is exact RCD-only.
    void setContrastPercent(float contrastPercent);
    float contrastPercent() const;
    void setAutoContrast(bool enabled);
    bool autoContrast() const;
    // Valid after the recorded command buffer has completed. In manual mode this equals
    // contrastPercent(); in auto mode it is the RawTherapee-derived threshold for that frame.
    float resolvedContrastPercent() const;
    // Valid after completion in auto mode: 80 or 40 for the RT detector path, 0 in manual mode.
    uint32_t autoDetectionTileSize() const;
    bool collectTelemetry(FrameTelemetry&);
    uint64_t currentAllocatedBytes() const;
    uint64_t peakAllocatedBytes() const;
    // Diagnostic-only accessors. Valid after completion when diagnosticBranchOutputs=true.
    VkImage diagnosticRcdImage() const noexcept;
    VkImage diagnosticVngImage() const noexcept;
    VkImage diagnosticPreBlurMaskImage() const noexcept;
    // Final blurred RCD weight mask (also retained for the existing diagnostic).
    VkImage diagnosticBlendMaskImage() const noexcept;
private: struct Impl; std::unique_ptr<Impl> impl_;
};
}
