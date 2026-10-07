// Adapts narrow frame ports to session-owned domain collaborators.

#include "camera/NativeCameraController.h"
#include "color/AePostGainCurve.h"
#include "session/SessionEngine.h"

namespace rawrcam::session {

SessionFrameCallbacks::SessionFrameCallbacks(SessionEngine* engine) : engine_(engine) {}

void SessionFrameCallbacks::postDiagnostic(const std::string& line) { engine_->appendDiagnostic(line); }
void SessionFrameCallbacks::postAudit(const std::string& line) { engine_->recordPipelineAuditLine(line); }
void SessionFrameCallbacks::notifySwapchainOutOfDate() {
    {
        engine_->recreateSwapchainAfterOutOfDate();
    }
}
void SessionFrameCallbacks::finalizeDetachedStill() {
    {
        engine_->finalizeDeferredSurfaceDetach();
    }
}

float SessionFrameCallbacks::postGainFor(const rawrcam::metadata::FrameMetadataSnapshot& metadata) {
    // Camera2 boost (100 == 1x) shaped by the user cap + HighlightProtection
    // knee width. Both live on SessionEngine so preview/stills share one
    // choke point.
    return rawrcam::color::effectiveAePostGain(metadata.postRawSensitivityBoost, engine_->maxAePostGain(),
                                               engine_->postGainKneeWidthEv());
}

void SessionFrameCallbacks::advanceStill() { engine_->capture_.single().advance(); }
bool SessionFrameCallbacks::isStillCaptureRequested() { return engine_->capture_.single().captureRequested(); }
bool SessionFrameCallbacks::deferSubmittedFrame(const rawrcam::metadata::FrameMetadataSnapshot& metadata,
                                                const rawrcam::color::FrameColorTransform& colorState,
                                                const tonemap::TonemapParams& tonemapParams, float aePostGain,
                                                bool filmEnabled, const spektrafilm_native::FilmLook& filmLook) {
    // The processing worker may still hold frames that arrived before the
    // shutter was accepted. Do not let queue delay turn one into the photo.
    if (!engine_->capture_.acceptsStillTimestamp(metadata.timestampNs)) return false;
    return engine_->capture_.single().deferSubmittedFrame(metadata, colorState, tonemapParams, aePostGain, filmEnabled,
                                                          filmLook);
}
bool SessionFrameCallbacks::beginDeferredSnapshot(AImage* imageLease, AHardwareBuffer* ahb, std::uint64_t timestampNs,
                                                  int acquireFenceFd) {
    return engine_->capture_.single().beginDeferredSnapshot(imageLease, ahb, timestampNs, acquireFenceFd);
}

void SessionFrameCallbacks::discardScopesSlot(std::uint32_t slotIndex) {
    engine_->monitoringCoordinator_.scopes().discardFrameSlot(slotIndex);
}
void SessionFrameCallbacks::retireScopesSlot(std::uint32_t slotIndex) {
    engine_->monitoringCoordinator_.scopes().retireFrameSlot(slotIndex);
}
std::optional<rawrcam::pipeline::FrameDiagnosticsPort::RenderedFeedback> SessionFrameCallbacks::consumeExposureFeedback(
    std::uint32_t slotIndex) {
    const auto feedback = engine_->monitoringCoordinator_.scopes().consumeExposureFeedback(slotIndex);
    if (!feedback) return std::nullopt;
    RenderedFeedback out;
    out.lumaP50 = feedback->lumaP50;
    out.lumaP95 = feedback->lumaP95;
    out.lumaP99 = feedback->lumaP99;
    out.brightFraction90 = feedback->brightFraction90;
    out.brightFraction96 = feedback->brightFraction96;
    out.anyChannelClippedFraction = feedback->anyChannelClippedFraction;
    return out;
}
bool SessionFrameCallbacks::exposureMeterWanted() {
    const auto* camera = engine_->cameraControls().controller();
    return camera && camera->wantsExposureMeter();
}
void SessionFrameCallbacks::exposureMeter(const rawrcam::metadata::FrameMetadataSnapshot& metadata,
                                          const RenderedFeedback& rendered) {
    auto* camera = engine_->cameraControls().controller();
    if (!camera) return;
    camera->submitExposureMeter(metadata.exposureTimeNs, metadata.sensitivity, rendered.lumaP50, rendered.lumaP95,
                                rendered.anyChannelClippedFraction);
}
bool SessionFrameCallbacks::overlayNeedsRawState() { return engine_->monitoringCoordinator_.overlay().needsRawState(); }
void SessionFrameCallbacks::recordAuditFrame(const rawrcam::metadata::FrameMetadataSnapshot& metadata,
                                             const rawrcam::color::FrameColorTransform& colorState) {
    if (engine_->frameAuditWriter_) {
        engine_->frameAuditWriter_->recordCompletedFrame(metadata, colorState);
    }
}
void SessionFrameCallbacks::integrityComplete(std::uint32_t slotIndex) {
    engine_->rawIntegrityProbe_.complete(slotIndex);
}

bool SessionFrameCallbacks::cpuCopyNeedsSample() { return engine_->rawCpuCopyProbe_.needsSample(); }
rawrcam::pipeline::FrameDiagnosticsPort::CpuCopyOutcome SessionFrameCallbacks::sampleCpuCopy(
    AImage* image, AHardwareBuffer* ahb, int acquireFenceFd, std::uint64_t timestampNs) {
    int fenceFd = acquireFenceFd;
    const bool copied = engine_->rawCpuCopyProbe_.sample(image, ahb, acquireFenceFd, timestampNs, &fenceFd);
    return CpuCopyOutcome{fenceFd == -2, copied, fenceFd};
}

void SessionFrameCallbacks::configureMultiframe(uint32_t width, uint32_t height, uint32_t cfa) {
    engine_->capture_.multiframe().configure(width, height, cfa);
}
void SessionFrameCallbacks::resetMultiframe() noexcept { engine_->capture_.multiframe().reset(); }
void SessionFrameCallbacks::shutdownMultiframe() noexcept { engine_->capture_.multiframe().shutdown(); }
bool SessionFrameCallbacks::multiframeEnabled() const noexcept {
    return engine_->capture_.multiframe().experimentalMultiframeEnabled();
}
void SessionFrameCallbacks::recordMultiframeFrame(VkCommandBuffer command, VkImage rawCopy, uint64_t timestampNs,
                                                  const metadata::FrameMetadataSnapshot& metadata,
                                                  const color::FrameColorTransform& color) {
    engine_->capture_.multiframe().recordFrame(command, rawCopy, timestampNs, metadata, color);
}
void SessionFrameCallbacks::retireMultiframeFrame(uint64_t timestampNs) noexcept {
    engine_->capture_.multiframe().retireFrame(timestampNs);
}

bool SessionFrameCallbacks::detachedCaptureActive() const noexcept { return engine_->hqStillDetachedWorkActive(); }
void SessionFrameCallbacks::resetStillProcessing() noexcept { engine_->capture_.single().resetHqProcessing(); }
void SessionFrameCallbacks::configureStillSnapshot(uint32_t width, uint32_t height, uint64_t generation) {
    engine_->capture_.single().configureRawSnapshot(width, height, generation);
}
void SessionFrameCallbacks::commitMultiframeFrame(uint64_t timestampNs) noexcept {
    engine_->capture_.multiframe().commitFrame(timestampNs);
}
void SessionFrameCallbacks::discardMultiframeFrame(uint64_t timestampNs) noexcept {
    engine_->capture_.multiframe().discardFrame(timestampNs);
}
}  // namespace rawrcam::session
