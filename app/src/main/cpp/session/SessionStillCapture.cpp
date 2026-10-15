#include <algorithm>
#include <cmath>

#include "capture/CaptureRequest.h"
#include "capture/multiframe/BracketExposure.h"
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
    std::lock_guard<std::mutex> lock(mu_);
    auto* camera = cameraControls_.controller();
    if (!camera || !camera->stillCaptureMetadataReady()) {
        appendDiagnostic("RAW_STILL_CAPTURE_REQUEST_REJECTED reason=camera_not_streaming");
        return 0;
    }
    dngContext.autoKeyEv = appliedAutoKeyEv();
    if (dngContext.captureTone) {
        auto capturedTone = std::make_shared<tonemap::TonemapParams>(*dngContext.captureTone);
        applyAutoKey(*capturedTone);
        dngContext.captureTone = std::move(capturedTone);
    }
    const uint64_t requestId = capture_.requestSingle(std::move(dngContext), std::move(jpegContext), [this] {
        return ingressQueue_.latestTimestamp(realtime_.generation());
    });
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
float SessionEngine::hdrHeadroomEvFor(const metadata::FrameMetadataSnapshot& metadata) const {
    const auto* controller = cameraControls_.controller();
    return controller ? controller->hdrHeadroomEvFor(metadata.aeMode, metadata.aeCompensationSteps) : 0.0f;
}
void SessionEngine::setPersistentEngineEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mu_);
    capture_.multiframe().setPersistentEngineEnabled(enabled);
    LOGI("PERSISTENT_ENGINE enabled=%s", enabled ? "true" : "false");
}
uint32_t SessionEngine::prepareExperimentalMultiframe(uint32_t maxFrames) {
    std::lock_guard<std::mutex> lock(mu_);
    return capture_.multiframe().prepareExperimentalMultiframeOnShutter(maxFrames);
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
            if (bracketToneActive()) tuning.bracketCurveEv = bracketCurveShareEv_.load(std::memory_order_relaxed);
            if (const auto reference = capture_.multiframe().preparedReferenceMetadata()) {
                const double exposure = rawrcam::capture::multiframe::linearExposure(*reference);
                tuning.aeLinearExposure = float(exposure * std::exp2(-double(hdrHeadroomEvFor(*reference))));
            }
        }
        auto tone = look_.toneParams();
        applyAutoKey(tone);
        if (baseDng.captureTone) {
            auto capturedTone = std::make_shared<tonemap::TonemapParams>(*baseDng.captureTone);
            applyAutoKey(*capturedTone);
            baseDng.captureTone = std::move(capturedTone);
        }
        if (mergedDng.captureTone) {
            auto capturedTone = std::make_shared<tonemap::TonemapParams>(*mergedDng.captureTone);
            applyAutoKey(*capturedTone);
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
    capture_.multiframe().cancelPreparedMultiframeCapture();
}

}  // namespace rawrcam::session
