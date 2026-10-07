#include "capture/CaptureRequest.h"
// SessionEngine C-API forwards (SessionEngineApi.h).
// Split from SessionEngine.cpp; pure code motion.
#include "diagnostics/logging/NativeLog.h"
#include "jni/SessionEngineApi.h"
#include "session/SessionEngine.h"

namespace rawrcam::session {
namespace {
SessionEngine* engine(EngineHandle h) noexcept { return reinterpret_cast<SessionEngine*>(h); }
}  // namespace
EngineHandle createEngine(std::string filesDir) noexcept {
    try {
        return new SessionEngine(std::move(filesDir));
    } catch (const std::exception& e) {
        LOGE("create failed: %s", e.what());
        return nullptr;
    }
}
void destroyEngine(EngineHandle h) noexcept { delete engine(h); }
bool setSurface(EngineHandle h, JNIEnv* env, jobject surface, int rotation) noexcept {
    return h && engine(h)->setSurface(env, surface, rotation);
}
bool setVideoSurface(EngineHandle h, JNIEnv* env, jobject surface, uint32_t width, uint32_t height,
                     uint32_t bitDepth) noexcept {
    return h && engine(h)->setVideoSurface(env, surface, width, height, bitDepth);
}
void requestVideoPrewarm(EngineHandle h, uint32_t width, uint32_t height, uint32_t bitDepth) noexcept {
    if (h) engine(h)->requestVideoPrewarm(width, height, bitDepth);
}
void releaseVideoProcessing(EngineHandle h) noexcept {
    if (h) engine(h)->releaseVideoProcessing();
}
void setVideoPreviewCropOut(EngineHandle h, uint32_t width, uint32_t height) noexcept {
    if (h) engine(h)->setVideoPreviewCropOut(width, height);
}
std::string videoStats(EngineHandle h) { return h ? engine(h)->videoStats() : std::string("{}"); }
void setVideoImageSettings(EngineHandle h, const rawrcam::video::VideoProcessingConfig& config) noexcept {
    if (h) engine(h)->setVideoImageSettings(config);
}
void setScopeDeviceRotationDegrees(EngineHandle h, int rotation) noexcept {
    if (h) engine(h)->setScopeDeviceRotationDegrees(rotation);
}
void setCameraActive(EngineHandle h, bool v) noexcept {
    if (h) engine(h)->cameraControls().setCameraActive(v);
}
void setOisEnabled(EngineHandle h, bool v) noexcept {
    if (h) engine(h)->cameraControls().setOisEnabled(v);
}
void setAntibandingMode(EngineHandle h, uint8_t mode) noexcept {
    if (h) engine(h)->cameraControls().setAntibandingMode(mode);
}
void selectLens(EngineHandle h, const std::string& v) noexcept {
    if (h) engine(h)->cameraControls().selectLens(v);
}
bool setCameraProfile(EngineHandle h, const std::string& json) noexcept {
    return h && engine(h)->cameraControls().setCameraProfile(json);
}
void setPreferredCameraId(EngineHandle h, const std::string& v) noexcept {
    if (h) engine(h)->cameraControls().setPreferredCameraId(v);
}
void setPreferredAccessRoute(EngineHandle h, const std::string& v) noexcept {
    if (h) engine(h)->cameraControls().setCameraAccessRoute(v);
}
void setPreferredColorMode(EngineHandle h, const std::string& v) noexcept {
    if (h) engine(h)->setColorMode(v);
}
void setPipelineDiagnostic(EngineHandle h, uint32_t v) noexcept {
    if (h) engine(h)->setPipelineDiagnostic(v);
}
void setPersistentDiagnosticsEnabled(EngineHandle h, bool v) noexcept {
    if (h) engine(h)->setPersistentDiagnosticsEnabled(v);
}
void setPersistZslRingEnabled(EngineHandle h, bool v) noexcept {
    if (h) engine(h)->setPersistZslRingEnabled(v);
}
uint32_t prepareExperimentalMultiframe(EngineHandle h, uint32_t maxFrames) noexcept {
    if (!h) return 0u;
    try {
        return engine(h)->prepareExperimentalMultiframe(maxFrames);
    } catch (...) {
        return 0u;
    }
}
uint64_t startPreparedMultiframeCapture(EngineHandle h, rawrcam::encoding::dng::DngCaptureContext baseDng,
                                        rawrcam::encoding::dng::DngCaptureContext mergedDng,
                                        rawrcam::capture::JpegCaptureRequest mergedJpeg, bool dumpRzslRequested,
                                        rawrcam::capture::multiframe::MultiframeTuning tuning,
                                        rawrcam::capture::multiframe::MultiframeBaseFrameMode baseFrameMode) noexcept {
    if (!h) {
        if (baseDng.outputFd >= 0) close(baseDng.outputFd);
        if (mergedDng.outputFd >= 0) close(mergedDng.outputFd);
        if (mergedJpeg.output.outputFd >= 0) close(mergedJpeg.output.outputFd);
        return 0u;
    }
    try {
        return engine(h)->startPreparedMultiframeCapture(
            std::move(baseDng), std::move(mergedDng), std::move(mergedJpeg), dumpRzslRequested, tuning, baseFrameMode);
    } catch (...) {
        if (baseDng.outputFd >= 0) close(baseDng.outputFd);
        if (mergedDng.outputFd >= 0) close(mergedDng.outputFd);
        if (mergedJpeg.output.outputFd >= 0) close(mergedJpeg.output.outputFd);
        return 0u;
    }
}
void cancelPreparedMultiframeCapture(EngineHandle h) noexcept {
    if (h) engine(h)->cancelPreparedMultiframeCapture();
}
void setInternalTraceCaptureEnabled(EngineHandle h, bool v) noexcept {
    if (h) engine(h)->cameraControls().setInternalTraceCaptureEnabled(v);
}
void setExperimentalZeroCopy(EngineHandle h, bool v) noexcept {
    if (h) engine(h)->setExperimentalZeroCopy(v);
}
void setExperimentalMultiframeEnabled(EngineHandle h, bool v) noexcept {
    if (h) engine(h)->setExperimentalMultiframeEnabled(v);
}
void setHdrPlusBracketEnabled(EngineHandle h, bool v) noexcept {
    if (h) engine(h)->setHdrPlusBracketEnabled(v);
}
void setPersistentEngineEnabled(EngineHandle h, bool v) noexcept {
    if (h) engine(h)->setPersistentEngineEnabled(v);
}
void setLensShadingCorrectionEnabled(EngineHandle h, bool v) noexcept {
    if (h) engine(h)->setLensShadingCorrectionEnabled(v);
}
void setHighlightReconstructionEnabled(EngineHandle h, bool v) noexcept {
    if (h) engine(h)->setHighlightReconstructionEnabled(v);
}
void setMaxAePostGain(EngineHandle h, float gain) noexcept {
    if (h) engine(h)->setMaxAePostGain(gain);
}
void setPostGainKneeWidthEv(EngineHandle h, float widthEv) noexcept {
    if (h) engine(h)->setPostGainKneeWidthEv(widthEv);
}
void setAutoMinFps(EngineHandle h, int fps) noexcept {
    if (h) engine(h)->cameraControls().setAutoMinFps(fps);
}
bool setRecordingFps(EngineHandle h, int fps) noexcept { return h && engine(h)->cameraControls().setRecordingFps(fps); }
void setPreviewForeground(EngineHandle h, bool foreground) noexcept {
    if (h) engine(h)->cameraControls().setForeground(foreground);
}
void setInitialConfigReady(EngineHandle h) noexcept {
    if (h) engine(h)->cameraControls().setInitialConfigReady();
}
void setStillCaptureInFlight(EngineHandle h, bool begin) noexcept {
    if (h) engine(h)->cameraControls().setStillCaptureInFlight(begin);
}
void tickCameraSession(EngineHandle h) noexcept {
    if (h) engine(h)->cameraControls().tickSession();
}
bool canRecoverStills(EngineHandle h) noexcept { return h && engine(h)->cameraControls().canRecoverStills(); }
void setVideoMode(EngineHandle h, bool video, int fps) noexcept {
    if (h) engine(h)->cameraControls().setVideoMode(video, fps);
}
void setShutterAngleDegrees(EngineHandle h, double degrees) noexcept {
    if (h) engine(h)->cameraControls().setShutterAngleDegrees(degrees);
}
void setCpuRawCopyProbeFrames(EngineHandle h, uint32_t v) noexcept {
    if (h) engine(h)->setCpuRawCopyProbeFrames(v);
}
void setMonitoringOverlay(EngineHandle h, uint32_t mode, float sensitivity) noexcept {
    if (h) engine(h)->setMonitoringOverlay(mode, sensitivity);
}
void setScopePresentationState(EngineHandle h, const rawrcam::monitoring::ScopePresentationState& state) noexcept {
    if (h) engine(h)->setScopePresentationState(state);
}
uint64_t requestRawStillCapture(EngineHandle h, rawrcam::encoding::dng::DngCaptureContext dngContext,
                                rawrcam::capture::JpegCaptureRequest jpegContext) noexcept {
    if (!h) {
        if (dngContext.outputFd >= 0) close(dngContext.outputFd);
        if (jpegContext.output.outputFd >= 0) close(jpegContext.output.outputFd);
        return 0;
    }
    try {
        return engine(h)->requestRawStillCapture(std::move(dngContext), std::move(jpegContext));
    } catch (...) {
        if (dngContext.outputFd >= 0) close(dngContext.outputFd);
        if (jpegContext.output.outputFd >= 0) close(jpegContext.output.outputFd);
        return 0;
    }
}
std::string pollDngWriteCompletion(EngineHandle h) { return h ? engine(h)->pollDngWriteCompletion() : std::string{}; }
std::string pollJpegWriteCompletion(EngineHandle h) { return h ? engine(h)->pollJpegWriteCompletion() : std::string{}; }
uint64_t recoverStill(EngineHandle h, const std::string& name, bool multiframe, int dng, int merged, int jpeg) {
    if (h) return engine(h)->recoverStill(name, multiframe, dng, merged, jpeg);
    for (int fd : {dng, merged, jpeg})
        if (fd >= 0) close(fd);
    return 0;
}
std::string runHighlightReplay(EngineHandle h, const std::string& inputPath) {
    return h ? engine(h)->runHighlightReplay(inputPath) : std::string("HIGHLIGHT_REPLAY_FAIL native=null");
}
std::string cameraControlSnapshot(EngineHandle h) {
    if (!h) return "{}";
    auto json = engine(h)->cameraControls().cameraControlSnapshot();
    // Ingress lives in the pipeline, not the camera controller; append it here.
    if (json.size() > 2 && json.back() == '}') {
        json.pop_back();
        json += std::string(",\"rawCpuIngress\":") + (engine(h)->rawCpuIngressActive() ? "true" : "false") + "}";
    }
    return json;
}
int videoRotationDegrees(EngineHandle h, int deviceRotationDegrees) {
    return h ? engine(h)->cameraControls().videoRotationDegrees(deviceRotationDegrees) : -1;
}
std::string faceDetectionsSnapshot(EngineHandle h) { return h ? engine(h)->faceDetectionsDisplaySnapshot() : "[]"; }
void setExposureMode(EngineHandle h, int v) noexcept {
    if (h) engine(h)->setExposureMode(v);
}
void setWhiteBalanceMode(EngineHandle h, int v, int64_t requestId) noexcept {
    if (h) engine(h)->setWhiteBalanceMode(v, requestId);
}
void setWhiteBalanceTempTint(EngineHandle h, int32_t temperatureK, int32_t tint, int32_t editedAxes,
                             int64_t requestId) noexcept {
    if (h) engine(h)->setWhiteBalanceTempTint(temperatureK, tint, editedAxes, requestId);
}
void setWhiteBalanceLocked(EngineHandle h, int32_t temperatureK, int32_t tint, int64_t requestId) noexcept {
    if (h) engine(h)->setWhiteBalanceLocked(temperatureK, tint, requestId);
}
void setManualExposureTimeNs(EngineHandle h, int64_t v) noexcept {
    if (h) engine(h)->setManualExposureTimeNs(v);
}
void setManualSensitivity(EngineHandle h, int32_t v) noexcept {
    if (h) engine(h)->setManualSensitivity(v);
}
void setExposureCompensationSteps(EngineHandle h, int32_t v) noexcept {
    if (h) engine(h)->setExposureCompensationSteps(v);
}
void setSpotAeTarget(EngineHandle h, bool active, float x, float y) noexcept {
    if (h) engine(h)->setSpotAeDisplay(active, x, y);
}
void setFocusMode(EngineHandle h, int v, float x, float y, uint64_t requestId) noexcept {
    if (h) engine(h)->setFocusMode(v, x, y, requestId);
}
void focusAt(EngineHandle h, float x, float y, uint64_t requestId) noexcept {
    if (h) engine(h)->focusAtDisplay(x, y, requestId);
}
void clearTapAf(EngineHandle h, uint64_t requestId) noexcept {
    if (h) engine(h)->clearTapAf(requestId);
}
void setManualFocusNormalized(EngineHandle h, float v, uint64_t requestId) noexcept {
    if (h) engine(h)->setManualFocusNormalized(v, requestId);
}
void setQuickToneNativeValue(EngineHandle h, int target, float v) noexcept {
    if (h) engine(h)->setQuickToneNativeValue(target, v);
}
void setColorRenderProfile(EngineHandle h, uint32_t v, std::string importedProfileId) noexcept {
    if (h) engine(h)->setColorRenderProfile(v, std::move(importedProfileId));
}
void setTonemapParameters(EngineHandle h, float a, float b, float c, float d, float e, float f, float g, float i,
                          float j) noexcept {
    if (h) engine(h)->setTonemapParameters(a, b, c, d, e, f, g, i, j);
}
void setAssetManager(EngineHandle h, AAssetManager* assetManager) noexcept {
    if (h) engine(h)->setAssetManager(assetManager);
}
void setFilmSimEnabled(EngineHandle h, bool v) noexcept {
    if (h) engine(h)->setFilmSimEnabled(v);
}
void setViewfinderDivisor(EngineHandle h, uint32_t divisor) noexcept {
    if (h) engine(h)->setViewfinderDivisor(divisor);
}
void setFilmSimLook(EngineHandle h, spektrafilm_native::FilmLook look) noexcept {
    if (h) engine(h)->setFilmSimLook(look);
}
}  // namespace rawrcam::session
