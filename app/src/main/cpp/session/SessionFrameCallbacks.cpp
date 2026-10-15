// Adapts narrow frame ports to session-owned domain collaborators.

#include <algorithm>
#include <cmath>

#include "capture/multiframe/BracketExposure.h"
#include "color/AePostGainCurve.h"
#include "camera/NativeCameraController.h"
#include "color/AutoKeyExposure.h"
#include "image_scopes/types.h"
#include "session/SessionEngine.h"
#include "tonemap/TonemapMath.h"

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
    // HDR+ Bracketed headroom is lifted back here as a linear gain, so the
    // viewfinder, AutoKey and frozen still gains all see the scene at Camera2
    // AE brightness. Highlights above display white are the render's: AgX,
    // NTRL (DaVinci Intermediate), film and log LUT inputs roll them off;
    // only direct sRGB/Rec.709 clip, as for any capture.
    const float gain = rawrcam::color::effectiveAePostGain(metadata.postRawSensitivityBoost, engine_->maxAePostGain(),
                                                           engine_->postGainKneeWidthEv());
    // HDR+ Bracketed with Camera2 AE: lift by reference / actual exposure so
    // compensation steps and AE re-convergence never show (ExposureNormalizer).
    constexpr int32_t kAeModeOff = 0, kAeStateConverged = 2, kAeStateLocked = 3, kAeStateFlashRequired = 4;
    const bool tagged = metadata.optimizedStillRequestId && *metadata.optimizedStillRequestId != 0;
    if (engine_->hdrHeadroomMaxEv() < 0.0f && metadata.aeMode != kAeModeOff && !tagged) {
        const double exposure = rawrcam::capture::multiframe::linearExposure(metadata);
        if (exposure > 0.0) {
            const bool settled = metadata.aeState == kAeStateConverged || metadata.aeState == kAeStateLocked ||
                                 metadata.aeState == kAeStateFlashRequired;
            float lift;
            {
                std::lock_guard<std::mutex> lock(normalizerMutex_);
                lift = normalizer_.update(metadata.timestampNs, exposure, engine_->reportedHeadroomEvFor(metadata),
                                          settled);
            }
            return std::clamp(gain * std::exp2(lift), 1.0f / 16.0f, tonemap::kMaxAePostGain);
        }
    } else if (!tagged) {
        std::lock_guard<std::mutex> lock(normalizerMutex_);
        if (normalizer_.primed()) normalizer_.reset();
    }
    const float headroomEv = engine_->reportedHeadroomEvFor(metadata);
    return headroomEv < 0.0f ? std::min(tonemap::kMaxAePostGain, gain * std::exp2(-headroomEv)) : gain;
}

std::optional<float> SessionFrameCallbacks::normalizedLiftEv(std::uint64_t timestampNs) const {
    std::lock_guard<std::mutex> lock(normalizerMutex_);
    return normalizer_.liftFor(timestampNs);
}

void SessionFrameCallbacks::applyAutoKey(tonemap::TonemapParams& tone,
                                         const rawrcam::metadata::FrameMetadataSnapshot& metadata) {
    // The noise limit assumes the next photo's merge, so the viewfinder shows
    // the lift the photo will get.
    engine_->applyAutoKey(tone, SessionEngine::equivalentIso(metadata, tone.aePostGain, engine_->expectedMergeFrames()));
}

bool SessionFrameCallbacks::sceneKeyMeteringEnabled() {
    return engine_->hdrHeadroomMaxEv() < 0.0f || (engine_->autoKeyMode() != rawrcam::color::AutoKeyMode::Off &&
                                                   !engine_->sceneReferredRender_.load(std::memory_order_relaxed));
}

void SessionFrameCallbacks::updateHdrHeadroom(const image_scopes::SceneExposureStatsData* stats, float clipLuma,
                                              const rawrcam::metadata::FrameMetadataSnapshot& metadata,
                                              std::uint64_t timestampNs) {
    auto* controller = engine_->cameraControls_.controller();
    const float floorEv = engine_->hdrHeadroomMaxEv();
    if (!controller) return;
    if (!(floorEv < 0.0f)) {
        headroomMeter_.reset();
        return;
    }
    // Manual exposure keeps the user's shutter/ISO; one-shot frames and AE
    // transitions say nothing about the repeating exposure.
    constexpr int32_t kAeModeOff = 0, kAeStateSearching = 1;
    if (!stats || !(clipLuma > 0.0f) || metadata.aeMode == kAeModeOff || metadata.aeState == kAeStateSearching ||
        !metadata.aeCompensationSteps || (metadata.optimizedStillRequestId && *metadata.optimizedStillRequestId != 0))
        return;
    // Low light: once Camera2 AE is adding gain, every EV of headroom is an
    // EV of shadow noise; lights at night are not worth it. Judge the light
    // by the exposure Camera2 AE would use without headroom (this frame's
    // shutter is already shortened by it), and ease the floor from the full
    // underexposure up to kLowLightFloorEv over two stops, so a scene near
    // the threshold does not flip between limits.
    constexpr double kLowLightBegin = 20e6 * 1600.0;  // 1/50 s at ISO 1600, in ns x ISO
    constexpr float kLowLightFloorEv = -1.5f;
    const float appliedEv = engine_->hdrHeadroomEvFor(metadata);
    float meterFloorEv = floorEv;
    if (metadata.exposureTimeNs > 0 && metadata.sensitivity > 0 && floorEv < kLowLightFloorEv) {
        const double normal = double(metadata.exposureTimeNs) * double(metadata.sensitivity) *
                              std::exp2(-double(appliedEv));
        const float t = std::clamp(float(std::log2(normal / kLowLightBegin)) * 0.5f, 0.0f, 1.0f);
        meterFloorEv = floorEv + (kLowLightFloorEv - floorEv) * t;
    }
    const auto target =
        headroomMeter_.update(stats->weightedBins, stats->totalWeight, std::log2(clipLuma), appliedEv,
                              meterFloorEv, timestampNs);
    if (target) controller->setHdrHeadroomEv(*target);
}

std::optional<rawrcam::pipeline::FrameDiagnosticsPort::SceneKeyFeedback> SessionFrameCallbacks::updateSceneKey(
    std::uint32_t slotIndex, const rawrcam::metadata::FrameMetadataSnapshot& metadata, std::uint64_t timestampNs,
    bool recording) {
    static_assert(image_scopes::kSceneExposureBins == rawrcam::color::kSceneKeyBins);
    static_assert(image_scopes::kSceneExposureMinEv == rawrcam::color::kSceneKeyMinEv);
    static_assert(image_scopes::kSceneExposureMaxEv == rawrcam::color::kSceneKeyMaxEv);
    const auto stats = engine_->monitoringCoordinator_.scopes().consumeSceneExposure(slotIndex);
    updateHdrHeadroom(stats ? &*stats : nullptr, engine_->monitoringCoordinator_.scopes().sceneClipLuma(slotIndex),
                      metadata, timestampNs);
    const auto mode = engine_->autoKeyMode();
    if (mode == rawrcam::color::AutoKeyMode::Off || !sceneKeyMeteringEnabled()) {
        autoKeySmoother_.reset();
        keySmoother_.reset();
        highlightSmoother_.reset();
        std::lock_guard<std::mutex> lock(engine_->autoKeyMutex_);
        engine_->autoKeyCurve_ = {};
        return std::nullopt;
    }
    // One-shot bracket/probe exposures must not move the repeating-scene curve.
    if (!stats || (metadata.optimizedStillRequestId && *metadata.optimizedStillRequestId != 0)) return std::nullopt;
    const auto keyEv = rawrcam::color::sceneKeyEv(stats->weightedBins);
    if (!keyEv) return std::nullopt;
    // Includes the HDR+ headroom lift, like the render gain it predicts.
    const float halGain = postGainFor(metadata);
    const auto highEv = rawrcam::color::scenePercentileEv(stats->weightedBins, 0.95f);
    if (!highEv) return std::nullopt;
    auto curve = rawrcam::color::autoKeyCurve(*keyEv, *highEv, halGain, rawrcam::color::autoKeyPolicy(mode));
    curve.liftEv = autoKeySmoother_.update(curve.liftEv, timestampNs, recording);
    curve.keyEv = keySmoother_.update(curve.keyEv, timestampNs, recording);
    curve.highlightEv = highlightSmoother_.update(curve.highlightEv, timestampNs, recording);
    curve.liftEv = std::min(curve.liftEv, tonemap::adaptiveToneMaxLift(curve));
    {
        std::lock_guard<std::mutex> lock(engine_->autoKeyMutex_);
        engine_->autoKeyCurve_ = curve;
    }
    return SceneKeyFeedback{*keyEv, curve.liftEv};
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
    auto tone = tonemapParams;
    engine_->applyAutoKey(tone, SessionEngine::equivalentIso(metadata, aePostGain, 1u));
    return engine_->capture_.single().deferSubmittedFrame(metadata, colorState, tone, aePostGain, filmEnabled,
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
