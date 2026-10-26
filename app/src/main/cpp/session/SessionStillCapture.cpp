#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

#include "capture/CaptureRequest.h"
#include "capture/BurstLimits.h"
#include "capture/multiframe/BracketExposure.h"
#include "capture/persistence/CaptureJob.h"
#include "develop/render/DenoiseProfile.h"
#include "local_tone/ExposureFusion.h"
// Still + multiframe capture routing.
#include "camera/NativeCameraController.h"
#include "diagnostics/logging/NativeLog.h"
#include "session/SessionEngine.h"

namespace rawrcam::session {
uint64_t SessionEngine::recoverStill(const std::string& name, bool multiframe, int dng, int merged, int jpeg) {
    std::lock_guard<std::mutex> lock(mu_);
    try {
        // recover() owns the descriptors from its first statement, including when it throws.
        if (vulkanContext_.device() != VK_NULL_HANDLE && (dng >= 0 || merged >= 0 || jpeg >= 0))
            return capture_.single().recover(name, multiframe, dng, merged, jpeg);
    } catch (const std::exception& e) {
        appendDiagnostic("STILL_RECOVERY_FAILED " + std::string(e.what()));
        return 0;
    }
    for (int fd : {dng, merged, jpeg})
        if (fd >= 0) close(fd);
    return 0;
}

namespace {
constexpr uint64_t kFullSizeUltraPixels = 100'000'000;  // 200 MP stills
}  // namespace

uint64_t SessionEngine::requestRawStillCapture(rawrcam::encoding::dng::DngCaptureContext dngContext,
                                               rawrcam::capture::JpegCaptureRequest jpegContext) {
    // Rejections close the destinations as the contexts go out of scope.
    if (!dngContext.outputFd.valid() && !jpegContext.output.outputFd.valid()) {
        appendDiagnostic("RAW_STILL_CAPTURE_REQUEST_REJECTED reason=invalid_dng_fd");
        return 0;
    }
    const bool jpegRequested = jpegContext.output.outputFd.valid();
    if (jpegRequested && (jpegContext.output.quality < 95 || jpegContext.output.quality > 100)) {
        appendDiagnostic("RAW_STILL_CAPTURE_REQUEST_REJECTED reason=invalid_jpeg_quality quality=" +
                         std::to_string(jpegContext.output.quality));
        return 0;
    }
    auto* camera = cameraControls_.controller();
    if (!camera || !camera->stillCaptureMetadataReady()) {
        appendDiagnostic("RAW_STILL_CAPTURE_REQUEST_REJECTED reason=camera_not_streaming");
        return 0;
    }
    const bool fullSize = camera->hasFullSizeCapture();
    const auto longExposure = fullSize ? std::nullopt : camera->deferredCaptureExposure();
    // A press while earlier stills still hold the capture budget (a 200 MP
    // frame is 382 MiB until saved) waits for it instead of being refused.
    // Presses run on the capture queue; the session lock isn't held.
    uint64_t snapshotBytes = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        snapshotBytes = capture_.single().rawSnapshotBytes();
    }
    if (snapshotBytes && !rawrcam::capture::persistence::canReserve(filesDir_, snapshotBytes, true)) {
        const auto start = std::chrono::steady_clock::now();
        const auto deadline = start + std::chrono::seconds(5);
        bool admitted = false;
        while (!admitted && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            admitted = rawrcam::capture::persistence::canReserve(filesDir_, snapshotBytes, true);
        }
        appendDiagnostic("RAW_STILL_CAPTURE_BUDGET_WAIT admitted=" + std::to_string(admitted ? 1 : 0) + " waitedMs=" +
                         std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now() - start)
                                            .count()));
    }
    std::unique_lock<std::mutex> lock(mu_);
    dngContext.autoKeyEv = appliedAutoKeyEv();
    if (dngContext.captureTone) {
        auto capturedTone = std::make_shared<tonemap::TonemapParams>(*dngContext.captureTone);
        // The frame is picked later; noise-limit by the latest viewfinder frame.
        applyAutoKey(*capturedTone, latestLiftNoise(capturedTone->aePostGain * std::exp2(capturedTone->exposureEV)));
        dngContext.captureTone = std::move(capturedTone);
    }
    // Long exposure: the viewfinder runs a shorter shutter, so the capture is
    // its own one-shot frame; wait for it instead of the next viewfinder frame.
    const uint64_t requestId = capture_.requestSingle(std::move(dngContext), std::move(jpegContext), [this] {
        return ingressQueue_.latestTimestamp(realtime_.generation());
    });
    if (requestId != 0 && fullSize) {
        // Capture-stream lens: the photo is the full-size still this request asks for.
        // The sensor switch takes ~0.4 s at 50 MP; a 200 MP still takes ~1 s
        // on the telephoto and ~4.5 s on the main camera.
        auto wait = std::chrono::milliseconds(1500);
        {
            std::lock_guard<std::mutex> stillLock(fullSize_.mutex);
            fullSize_.requestIds.insert(requestId);
            if (fullSize_.context && uint64_t(fullSize_.context->geometry.rawBufferWidth) *
                                             fullSize_.context->geometry.rawBufferHeight >
                                         kFullSizeUltraPixels)
                wait = std::chrono::milliseconds(8000);
        }
        if (const auto exposure = camera->deferredCaptureExposure())
            wait += std::chrono::milliseconds(exposure->exposureTimeNs / 1'000'000);
        capture_.single().expectOptimizedFrame(requestId, wait);
        // Focus preparation consumes callback results; never wait under mu_.
        lock.unlock();
        const bool sent = camera->captureFullSize(requestId, 1);
        lock.lock();
        if (!sent) {
            capture_.single().failAcquisition(requestId, "full_size_preparation_failed");
            std::lock_guard<std::mutex> stillLock(fullSize_.mutex);
            fullSize_.requestIds.erase(requestId);
            appendDiagnostic("FULL_SIZE_SUBMIT_FAILED requestId=" + std::to_string(requestId));
        }
    }
    if (requestId != 0 && longExposure) {
        const auto waitMs = std::chrono::milliseconds(longExposure->exposureTimeNs / 1'000'000 + 1000);
        capture_.single().expectOptimizedFrame(requestId, waitMs);
        if (!camera->captureLongExposure(requestId, nullptr)) {
            // Nothing submitted: fall back to the viewfinder frame promptly.
            capture_.single().expectOptimizedFrame(requestId);
            appendDiagnostic("RAW_STILL_LONG_EXPOSURE_SUBMIT_FAILED requestId=" + std::to_string(requestId));
        }
    }
    finalizeDeferredSurfaceDetach();
    return requestId;
}

std::string SessionEngine::pollDngWriteCompletion() {
    std::lock_guard<std::mutex> lock(mu_);
    return capture_.pollDng();
}

std::string SessionEngine::pollJpegWriteCompletion() {
    std::lock_guard<std::mutex> lock(mu_);
    const auto result = capture_.pollJpeg();
    finalizeDeferredSurfaceDetach();
    return result;
}

void SessionEngine::setPersistZslRingEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mu_);
    capture_.multiframe().setPersistZslRingEnabled(enabled);
    LOGI("ZSL_RING enabled=%s", enabled ? "true" : "false");
}
void SessionEngine::setExperimentalMultiframeEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mu_);
    capture_.multiframe().setExperimentalMultiframeEnabled(enabled);
    // Fill the bridge the ring copies from, so the toggle needs no camera
    // restart. Turning off keeps it until the next reconfigure drops it.
    if (enabled) realtime_.ensureMultiframeBridge();
    LOGI("MULTIFRAME_EXPERIMENTAL enabled=%s", enabled ? "true" : "false");
}
void SessionEngine::setHdrPlusHeadroomMaxEv(float maxEv) {
    const float floor = std::isfinite(maxEv) ? std::clamp(maxEv, -4.0f, 0.0f) : 0.0f;
    hdrHeadroomMaxEv_.store(floor, std::memory_order_relaxed);
    if (!cameraControls_.controller()) return;
    cameraControls_.controller()->setSensitivityCalibrationWanted(floor < 0.0f);
    // Off: drop the headroom now; on: the meter sets it from the next frames.
    if (!(floor < 0.0f)) cameraControls_.controller()->setHdrHeadroomEv(0.0f);
    LOGI("HDRPLUS_BRACKET headroomMaxEv=%.1f", floor);
}
rawr::local_tone::LiftNoise SessionEngine::liftNoiseFor(const metadata::FrameMetadataSnapshot& m, float renderGain,
                                                        std::uint32_t frames) noexcept {
    rawr::local_tone::LiftNoise n{};
    n.renderGain = renderGain;
    n.frames = std::max<std::uint32_t>(1u, frames);
    float shot = 0.0f, read = 0.0f;
    if (develop::rendered::resolveDenoiseNoise(m, shot, read)) {
        n.shot = shot;
        n.read = read;
    }
    return n;
}
void SessionEngine::setLocalTone(float shadowsEv, float highlightsEv, float sigma, unsigned coarsestMip, bool boostLocalContrast) {
    const float ev = std::isfinite(shadowsEv) && shadowsEv > 0.0f
                         ? std::clamp(shadowsEv, rawr::local_tone::kMinShadowsEv, rawr::local_tone::kMaxShadowsEv)
                         : 0.0f;
    localToneHighlightsEv_.store(std::isfinite(highlightsEv) ? std::clamp(highlightsEv, 0.5f, 2.0f) : 2.0f, std::memory_order_relaxed);
    localToneSigma_.store(std::isfinite(sigma) ? std::clamp(sigma, 3.0f, 5.0f) : 5.0f, std::memory_order_relaxed);
    localToneMip_.store(std::clamp(coarsestMip, 6u, 8u), std::memory_order_relaxed);
    localToneBoost_.store(boostLocalContrast, std::memory_order_relaxed);
    localToneShadowsEv_.store(ev, std::memory_order_relaxed);
    LOGI("LOCAL_TONE shadowsEv=%.2f", ev);
}
float SessionEngine::reportedHeadroomEvFor(const metadata::FrameMetadataSnapshot& metadata) const {
    const auto* controller = cameraControls_.controller();
    return controller ? controller->hdrHeadroomEvFor(metadata.aeMode, metadata.aeCompensationSteps) : 0.0f;
}
float SessionEngine::hdrHeadroomEvFor(const metadata::FrameMetadataSnapshot& metadata) const {
    if (const auto lift = pipelineCallbacks_.normalizedLiftEv(metadata.timestampNs)) return -*lift;
    return reportedHeadroomEvFor(metadata);
}
void SessionEngine::setPersistentEngineEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mu_);
    capture_.multiframe().setPersistentEngineEnabled(enabled);
    LOGI("PERSISTENT_ENGINE enabled=%s", enabled ? "true" : "false");
}
uint32_t SessionEngine::prepareExperimentalMultiframe(uint32_t maxFrames, uint32_t longFrames) {
    // HDR+ Bracketed: when the shutter/gain limits would leave the long
    // frames too dark to help (they would be skipped), freeze their slots as
    // burst frames instead. Checked outside mu_ like other camera calls.
    // Long exposure (shutter past the viewfinder cap): the ZSL ring holds only
    // viewfinder frames, so the merge takes a burst
    // of post-shutter frames at that shutter (full-size stills below on a
    // capture-stream lens).
    if (const auto* controller = cameraControls_.controller()) {
        if (const auto exposure = controller->deferredCaptureExposure()) {
            if (!controller->hasFullSizeCapture())
                return prepareLongExposureBurst(maxFrames + longFrames, exposure->exposureTimeNs);
        }
    }
    // Capture-stream lens: the ZSL ring holds only binned viewfinder frames;
    // the merge takes a burst of full-size stills requested now.
    if (const auto* controller = cameraControls_.controller(); controller && controller->hasFullSizeCapture()) {
        const uint32_t profileLimit = controller->fullSizeBurstLimit();
        const uint32_t cap = capture::fullSizeBurstCount(maxFrames + longFrames, profileLimit);
        LOGI("MULTIFRAME_FULL_SIZE_PLAN requested=%u profileLimit=%u effective=%u sequential=%d",
             maxFrames + longFrames, profileLimit, cap, controller->fullSizeBurstSequential() ? 1 : 0);
        // A lens whose HAL fails full-size bursts takes a single still instead.
        if (cap < 2u) {
            LOGI("MULTIFRAME_FULL_SIZE_SINGLE limit=%u", cap);
            return 0u;
        }
        return prepareFullSizeBurst(cap);
    }
    bool skipLong = false;
    if (longFrames > 0u) {
        const auto* controller = cameraControls_.controller();
        skipLong = controller && !controller->longFramesWorthwhile();
        if (skipLong) LOGI("MULTIFRAME_BRACKET_PRESKIP burst=%u long=%u", maxFrames + longFrames, longFrames);
    }
    std::lock_guard<std::mutex> lock(mu_);
    bracketSkippedForPending_ = skipLong;
    uint32_t burst = std::min<uint32_t>(skipLong ? maxFrames + longFrames : maxFrames, capture::kMaxBurstFrames);
    // The streaming RAW ring enforces its own byte budget; do not impose a separate resolution-based count.
    return capture_.multiframe().prepareExperimentalMultiframeOnShutter(burst);
}
uint32_t SessionEngine::prepareLongExposureBurst(uint32_t maxFrames, int64_t exposureTimeNs) {
    auto* controller = cameraControls_.controller();
    if (!controller) return 0u;
    std::shared_ptr<rawrcam::capture::multiframe::BracketCollector> collector;
    {
        std::lock_guard<std::mutex> lock(mu_);
        collector = capture_.multiframe().armLongExposureBurst(std::min<uint32_t>(maxFrames, capture::kMaxBurstFrames));
    }
    if (!collector) return 0u;
    const auto requestId = collector->requestId();
    const auto abandon = [&] {
        std::lock_guard<std::mutex> lock(mu_);
        capture_.multiframe().abandonBracket(requestId);
    };
    if (!controller->captureLongExposure(requestId, nullptr, collector->expected())) {
        abandon();
        LOGW("MULTIFRAME_LONG_BURST_SUBMIT_FAIL request=%llu", static_cast<unsigned long long>(requestId));
        return 0u;
    }
    // The frames queue behind the preview requests in flight: allow each its
    // exposure plus a second.
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(int64_t(collector->expected()) * (exposureTimeNs / 1'000'000 + 1000));
    auto taken = collector->take(deadline);
    abandon();
    rawrcam::capture::multiframe::FrozenBurst burst;
    for (auto& f : taken) {
        if (f.frames.size() != 1u || f.refs.size() != 1u || f.metadata.size() != 1u || f.colors.size() != 1u ||
            !f.snapshot)
            continue;
        if (!burst.snapshot)
            burst.snapshot = std::move(f.snapshot);
        else
            burst.extraSnapshots.push_back(std::move(f.snapshot));
        burst.refs.push_back(f.refs.front());
        burst.frames.push_back(f.frames.front());
        burst.metadata.push_back(std::move(f.metadata.front()));
        burst.colors.push_back(std::move(f.colors.front()));
    }
    LOGI("MULTIFRAME_LONG_BURST_FRAMES request=%llu expected=%u received=%zu exposureNs=%lld",
         static_cast<unsigned long long>(requestId), collector->expected(), burst.frames.size(),
         static_cast<long long>(exposureTimeNs));
    // Fewer than two frames is no merge: the press falls back to a single still.
    if (burst.frames.size() < 2u) return 0u;
    burst.referenceIndex = static_cast<std::uint32_t>(burst.frames.size() / 2u);
    burst.referenceMetadata = burst.metadata[burst.referenceIndex];
    burst.referenceColor = burst.colors[burst.referenceIndex];
    const auto prepared = static_cast<uint32_t>(burst.frames.size());
    std::lock_guard<std::mutex> lock(mu_);
    bracketSkippedForPending_ = true;  // the long frames are the burst
    return capture_.multiframe().adoptPreparedCapture(
               std::make_unique<rawrcam::capture::multiframe::PendingMultiframeCapture>(std::move(burst)))
               ? prepared
               : 0u;
}
uint64_t SessionEngine::startPreparedMultiframeCapture(
    rawrcam::encoding::dng::DngCaptureContext baseDng, rawrcam::encoding::dng::DngCaptureContext mergedDng,
    rawrcam::capture::JpegCaptureRequest mergedJpeg, bool dumpRzslRequested,
    rawrcam::capture::multiframe::MultiframeTuning tuning,
    rawrcam::capture::multiframe::MultiframeBaseFrameMode baseFrameMode) {
    std::optional<rawrcam::capture::multiframe::BracketPlan> bracket;
    uint64_t requestId = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        // HDR+ Bracketed: record the Camera2 AE exposure the underexposed ZSL
        // burst is lifted back to (unchanged reference exposure in manual).
        if (tuning.mergeAlgorithm == rawrcam::capture::multiframe::kMergeAlgorithmHdrPlusBracketed) {
            tuning.bracketCapture = 1u;
            // Their slots were frozen as burst frames at the shutter (or a
            // full-size burst has no binned long frames to take).
            if (bracketSkippedForPending_) tuning.bracketFrames = 0u;
            if (const auto reference = capture_.multiframe().preparedReferenceMetadata()) {
                const double exposure = rawrcam::capture::multiframe::linearExposure(*reference);
                tuning.aeLinearExposure = float(exposure * std::exp2(-double(hdrHeadroomEvFor(*reference))));
            }
        }
        // Noise limit for the merge: the reference frame's noise model at
        // its render gain over the frames merged (burst plus any
        // post-shutter frames).
        auto tone = look_.toneParams();
        rawr::local_tone::LiftNoise noise{};
        if (const auto reference = capture_.multiframe().preparedReferenceMetadata()) {
            const std::uint32_t frames =
                capture_.multiframe().preparedFrameCount() +
                (tuning.mergeAlgorithm == rawrcam::capture::multiframe::kMergeAlgorithmHdrPlusBracketed
                     ? tuning.bracketFrames
                     : 0u);
            noise = liftNoiseFor(*reference, pipelineCallbacks_.postGainFor(*reference) * std::exp2(tone.exposureEV),
                                 frames);
        }
        applyAutoKey(tone, noise);
        if (baseDng.captureTone) {
            auto capturedTone = std::make_shared<tonemap::TonemapParams>(*baseDng.captureTone);
            applyAutoKey(*capturedTone, noise);
            baseDng.captureTone = std::move(capturedTone);
        }
        if (mergedDng.captureTone) {
            auto capturedTone = std::make_shared<tonemap::TonemapParams>(*mergedDng.captureTone);
            applyAutoKey(*capturedTone, noise);
            mergedDng.captureTone = std::move(capturedTone);
        }
        baseDng.autoKeyEv = tone.adaptiveTone.liftEv;
        mergedDng.autoKeyEv = baseDng.autoKeyEv;
        requestId = capture_.multiframe().startPreparedMultiframeCapture(
            std::move(baseDng), std::move(mergedDng), std::move(mergedJpeg), dumpRzslRequested, tuning, baseFrameMode,
            tone, look_.requestedFilmEnabled(), look_.filmLook(), &bracket);
    }
    // HDR+ bracketed: the dark frames are one-shot camera requests, submitted
    // outside mu_ like every other camera control call.
    if (requestId != 0u && bracket) {
        auto* controller = cameraControls_.controller();
        const bool submitted =
            controller && controller->captureExposureBracket(bracket->requestId, bracket->baseExposureTimeNs,
                                                             bracket->baseSensitivity, bracket->evOffsets);
        if (!submitted) {
            LOGW("MULTIFRAME_BRACKET_SUBMIT_FAIL request=%llu", static_cast<unsigned long long>(bracket->requestId));
            std::lock_guard<std::mutex> lock(mu_);
            capture_.multiframe().abandonBracket(bracket->requestId);
        }
    }
    return requestId;
}
void SessionEngine::cancelPreparedMultiframeCapture() {
    std::lock_guard<std::mutex> lock(mu_);
    bracketSkippedForPending_ = false;
    capture_.multiframe().cancelPreparedMultiframeCapture();
}

}  // namespace rawrcam::session
