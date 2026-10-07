#pragma once
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "diagnostics/timing/GpuTimingTracker.h"
#include "metadata/FrameMetadataSnapshot.h"
#include "pipeline/FrameSlotPool.h"
#include "pipeline/MonitorRecorder.h"

namespace raw_preview {
class RawPreview;
}
namespace tonemap {
class TonemapEngine;
struct TonemapParams;
}  // namespace tonemap
namespace spektrafilm_native {
class SpektraFilm;
}  // namespace spektrafilm_native
#include "spektrafilm/SpektraFilm.h"
namespace rawrcam::vulkan {
struct ImportedRaw;
}
namespace rawrcam::diagnostics {
class PipelineDiagnostics;
class RawIntegrityProbe;
}  // namespace rawrcam::diagnostics
namespace rawrcam::presentation {
class SwapchainRenderer;
}
namespace rawrcam::monitoring {
class MonitoringOverlayProcessor;
class ImageScopesProcessor;
}  // namespace rawrcam::monitoring

namespace rawrcam::pipeline {

struct RawDevelopRecordInput {
    pipeline::FrameSlot& slot;
    uint32_t slotIndex = 0;
    uint32_t swapImageIndex = 0;
    rawrcam::vulkan::ImportedRaw& raw;
    uint64_t generation = 0;
    uint64_t timestampNs = 0;
    uint32_t rawWidth = 0;
    uint32_t rawHeight = 0;
    uint32_t previewWidth = 0;
    uint32_t previewHeight = 0;
    uint32_t cfa = 0;
    uint32_t diagnosticMode = 0;
    bool experimentalZeroCopy = false;
    bool lensShadingCorrectionEnabled = false;
    bool highlightReconstructionEnabled = true;
    uint32_t queueFamily = 0;
    int sensorOrientationDegrees = 0;
    int displayRotationDegrees = 0;
    int scopeDeviceRotationDegrees = 0;
    std::array<float, 4> black{};
    float white = 0.0f;
    std::array<float, 4> whiteBalance{};
    std::array<float, 9> sensorToLinearSrgb{};
    const rawrcam::metadata::FrameMetadataSnapshot& metadata;
    const tonemap::TonemapParams& tonemapParams;
    bool renderedExposureFeedbackEnabled = false;
    // When true the multiframe ring consumes the bridge-owned copy, so the
    // bridge must run even though compute stages read the import buffer.
    bool multiframeEnabled = false;
    // Film simulation (spektrafilm), appended last to preserve the
    // positional aggregate-init order above. film==nullptr (recorder member)
    // disables the branch; otherwise filmEnabled selects per frame.
    // filmLook carries the full look; timeSec derives from timestampNs.
    bool filmEnabled = false;
    spektrafilm_native::FilmLook filmLook{};
    // Viewfinder look-stage divisor (1 = preview size, 2..4 = downscaled).
    // Drives the downscale blit, the tonemap/film record dims, and the
    // upscale source together. Film clamps to >= 2: its arena and the
    // scaled slot images are sized for half the preview, so no rebuild is
    // needed. Stills and video always render full-res and never read this.
    uint32_t viewfinderDivisor = 2;
    // Recording can continue when the monitor swapchain has no image.
    bool presentationEnabled = true;
    // Optional recording work runs after RAW import/copy is ready and before
    // viewfinder demosaic, tonemap, scopes, and presentation.
    std::function<void()> recordPrimaryVideo;
    // Highlight method and tuning shared with still/video capture.
    uint32_t highlightMethod = 0;
    float highlightThreshold = 1.0f;
    float highlightCompression = 163.0f;
    // Idle video-mode preview crop: recording output size (0 = full frame).
    // Appended last to preserve the positional aggregate-init order above.
    // The recorder resolves the exact record window (see VideoCrop.h) so the
    // idle preview frames what the recorder will write.
    uint32_t videoCropOutWidth = 0;
    uint32_t videoCropOutHeight = 0;
};

struct RawDevelopRecordResult {
    VkPipelineStageFlags rawWaitStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
};

class RawDevelopRecorder final {
   public:
    using Audit = std::function<void(const std::string&)>;

    RawDevelopRecorder(raw_preview::RawPreview& rawPreview, tonemap::TonemapEngine& tonemap,
                       std::function<std::shared_ptr<spektrafilm_native::SpektraFilm>()> acquireFilm,
                       rawrcam::diagnostics::PipelineDiagnostics& diagnostics,
                       rawrcam::diagnostics::RawIntegrityProbe& integrityProbe,
                       rawrcam::monitoring::MonitoringOverlayProcessor& monitoringOverlay,
                       rawrcam::monitoring::ImageScopesProcessor& imageScopes,
                       rawrcam::presentation::SwapchainRenderer& presentation,
                       rawrcam::diagnostics::GpuTimingTracker& performance, Audit audit);

    RawDevelopRecordResult record(const RawDevelopRecordInput& input, uint32_t auditSubmitCount) const;
    void recordVideoScopes(VkCommandBuffer command, uint32_t frameSlot, VkImageView monitorView, VkImage monitorImage,
                           uint32_t width, uint32_t height, int sensorOrientationDegrees,
                           int deviceRotationDegrees) const;
    void restoreVideoScopes(VkCommandBuffer command, uint32_t frameSlot) const;
    static std::array<float, 9> composeCameraToAp1(const std::array<float, 9>& matrix);

   private:
    raw_preview::RawPreview& rawPreview_;
    tonemap::TonemapEngine& tonemap_;
    // Resolves the live film engine per recorded frame under the owner's
    // film mutex. Shared ownership keeps a retired engine alive until the
    // in-flight frame finishes, so enable/disable toggles can never leave a
    // dangling pointer behind (previously a raw snapshot taken once at
    // construction went stale across destroy/recreate).
    std::function<std::shared_ptr<spektrafilm_native::SpektraFilm>()> acquireFilm_;
    rawrcam::diagnostics::PipelineDiagnostics& diagnostics_;
    rawrcam::diagnostics::RawIntegrityProbe& integrityProbe_;
    MonitorRecorder monitor_;
    rawrcam::diagnostics::GpuTimingTracker& performance_;
    Audit audit_;
};

}  // namespace rawrcam::pipeline
