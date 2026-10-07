#include "capture/CaptureRequest.h"
// Still + multiframe capture routing.
#include "camera/NativeCameraController.h"
#include "diagnostics/logging/NativeLog.h"
#include "session/SessionEngine.h"

namespace rawrcam::session {
uint64_t SessionEngine::recoverStill(const std::string& name, bool multiframe, int dng, int merged, int jpeg) {
    std::lock_guard<std::mutex> lock(mu_);
    try {
        if (vulkanContext_.device() != VK_NULL_HANDLE && (dng >= 0 || merged >= 0 || jpeg >= 0))
            return capture_.single().recover(name, multiframe, dng, merged, jpeg);
    } catch (const std::exception& e) {
        appendDiagnostic("STILL_RECOVERY_FAILED " + std::string(e.what()));
    }
    for (int fd : {dng, merged, jpeg})
        if (fd >= 0) close(fd);
    return 0;
}

uint64_t SessionEngine::requestRawStillCapture(rawrcam::encoding::dng::DngCaptureContext dngContext,
                                               rawrcam::capture::JpegCaptureRequest jpegContext) {
    if (dngContext.outputFd < 0 && jpegContext.output.outputFd < 0) {
        appendDiagnostic("RAW_STILL_CAPTURE_REQUEST_REJECTED reason=invalid_dng_fd");
        if (jpegContext.output.outputFd >= 0) close(jpegContext.output.outputFd);
        return 0;
    }
    const bool jpegRequested = jpegContext.output.outputFd >= 0;
    if (jpegRequested && (jpegContext.output.quality < 95 || jpegContext.output.quality > 100)) {
        appendDiagnostic("RAW_STILL_CAPTURE_REQUEST_REJECTED reason=invalid_jpeg_quality quality=" +
                         std::to_string(jpegContext.output.quality));
        if (dngContext.outputFd >= 0) close(dngContext.outputFd);
        close(jpegContext.output.outputFd);
        return 0;
    }
    std::lock_guard<std::mutex> lock(mu_);
    auto* camera = cameraControls_.controller();
    if (!camera || !camera->stillCaptureMetadataReady()) {
        appendDiagnostic("RAW_STILL_CAPTURE_REQUEST_REJECTED reason=camera_not_streaming");
        if (dngContext.outputFd >= 0) close(dngContext.outputFd);
        if (jpegRequested) close(jpegContext.output.outputFd);
        return 0;
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
void SessionEngine::setHdrPlusBracketEnabled(bool enabled) {
    if (!cameraControls_.controller()) return;
    cameraControls_.controller()->setSensitivityCalibrationWanted(enabled);
    LOGI("HDRPLUS_BRACKET enabled=%s", enabled ? "true" : "false");
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
        requestId = capture_.multiframe().startPreparedMultiframeCapture(
            std::move(baseDng), std::move(mergedDng), std::move(mergedJpeg), dumpRzslRequested, tuning, baseFrameMode,
            look_.toneParams(), look_.requestedFilmEnabled(), look_.filmLook(), &bracket);
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
