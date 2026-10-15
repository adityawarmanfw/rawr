#pragma once
#include <functional>
#include <memory>
#include <string>

#include "local_tone/LocalTone.h"
#include "diagnostics/timing/GpuTimingTracker.h"
#include "metadata/CameraContextMetadata.h"
#include "pipeline/FrameSlotPool.h"
#include "pipeline/FrameSubmitCoordinator.h"
#include "pipeline/RealtimeResources.h"
#include "tonemap/ColorRenderProfile.h"
namespace rawrcam::monitoring {
class MonitoringCoordinator;
}
namespace rawrcam::diagnostics {
class RawIntegrityProbe;
class PipelineDiagnostics;
class RawCpuCopyProbe;
}  // namespace rawrcam::diagnostics
namespace rawrcam::pipeline {
class PreviewLookController;
class RealtimePipeline final : public RealtimeResources {
   public:
    using Diagnostic = std::function<void(const std::string&)>;
    using Audit = Diagnostic;
    RealtimePipeline(std::string rawShaderPath, std::string rawCfaStateShaderPath, std::string filesDir,
                     vulkan::VulkanContext& context, PreviewLookController& look,
                     monitoring::MonitoringCoordinator& monitoring, diagnostics::RawIntegrityProbe& integrity,
                     diagnostics::PipelineDiagnostics& diagnostics, presentation::SwapchainRenderer& presentation,
                     diagnostics::RawCpuCopyProbe& cpuProbe, FrameLifecyclePort& lifecycle, FrameCapturePort& capture,
                     FrameDiagnosticsPort& feedback, std::mutex& queueMutex, Diagnostic diagnostic, Audit audit);
    ~RealtimePipeline();
    void initializeDevice();
    bool configureCamera(uint64_t generation, const metadata::CameraContextMetadata& metadata);
    bool setColorRenderProfile(tonemap_integration::ColorRenderProfile profile, std::string importedProfileId);
    void setScopeDeviceRotationDegrees(int rotation);
    void ensureMultiframeBridge();
    void releaseProcessingResourcesForFrozenReplay() noexcept;
    void shutdown() noexcept;
    diagnostics::GpuTimingTracker& timing() noexcept { return performanceTracker_; }
    FrameSubmitCoordinator& coordinator() noexcept { return realtime_; }
    const FrameSubmitCoordinator& coordinator() const noexcept { return realtime_; }
    vulkan::RawAhbImporter* importer() const noexcept override { return rawAhbImporter_.get(); }
    RawDevelopRecorder* recorder() const noexcept override { return frameProcessor_.get(); }
    raw_preview::RawPreview* preview() const noexcept override { return rawPreview_.get(); }
    bool configured() const noexcept { return configured_; }
    uint64_t generation() const noexcept { return generation_; }
    uint32_t rawWidth() const noexcept { return rawWidth_; }
    uint32_t rawHeight() const noexcept { return rawHeight_; }
    uint32_t previewWidth() const noexcept { return previewWidth_; }
    uint32_t previewHeight() const noexcept { return previewHeight_; }
    int sensorOrientationDegrees() const noexcept { return sensorOrientationDegrees_; }
    int displayRotationDegrees() const noexcept { return displayRotationDegrees_; }
    void setDisplayRotationDegrees(int value) {
        displayRotationDegrees_ = value;
        realtime_.setDisplayRotationDegrees(value);
    }

   private:
    bool configureGeometry(uint64_t generation, uint32_t width, uint32_t height, uint32_t cfa,
                           const std::array<float, 4>& black, float white, int sensorOrientationDegrees);
    void createFrameProcessor();
    void destroyCameraResources(bool preserveHqStill = false, bool teardownFilm = true) noexcept;
    void emit(const std::string& line) const;
    std::string rawShaderPath_, rawCfaStateShaderPath_, filesDir_;
    vulkan::VulkanContext& vulkanContext_;
    PreviewLookController& look_;
    monitoring::MonitoringCoordinator& monitoring_;
    diagnostics::RawIntegrityProbe& rawIntegrityProbe_;
    diagnostics::PipelineDiagnostics& pipelineDiagnostics_;
    presentation::SwapchainRenderer& swapchainRenderer_;
    FrameCapturePort& capturePort_;
    diagnostics::RawCpuCopyProbe& rawCpuCopyProbe_;
    std::mutex& queueSubmitMutex_;
    Diagnostic diagnostic_;
    Audit audit_;
    FrameSlotPool frameSlots_;
    diagnostics::GpuTimingTracker performanceTracker_;
    std::unique_ptr<vulkan::RawAhbImporter> rawAhbImporter_;
    std::unique_ptr<raw_preview::RawPreview> rawPreview_;
    std::unique_ptr<RawDevelopRecorder> frameProcessor_;
    std::unique_ptr<rawr::local_tone::LocalToneProcessor> localTone_;
    FrameSubmitCoordinator realtime_;
    bool configured_ = false;
    uint64_t generation_ = 0;
    uint32_t rawWidth_ = 0, rawHeight_ = 0, previewWidth_ = 0, previewHeight_ = 0, cfa_ = 0;
    std::array<float, 4> black_{};
    float white_ = 0;
    int sensorOrientationDegrees_ = 0, displayRotationDegrees_ = 0, scopeDeviceRotationDegrees_ = 0;
};
}  // namespace rawrcam::pipeline
