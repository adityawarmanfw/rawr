#pragma once
#include <android/asset_manager.h>
#include <jni.h>
#include <spektrafilm/SpektraFilm.h>

#include <cstdint>
#include <string>

#include "capture/CaptureRequest.h"
#include "capture/multiframe/MultiframeBaseFrame.h"
#include "capture/multiframe/MultiframeTuning.h"
#include "encoding/dng/DngCaptureContext.h"
#include "encoding/jpeg/JpegCaptureContext.h"
#include "monitoring/MonitoringState.h"
#include "video_pipeline/VideoProcessingConfig.h"

namespace rawrcam::session {
using EngineHandle = void*;

EngineHandle createEngine(std::string filesDir) noexcept;
void destroyEngine(EngineHandle handle) noexcept;
bool setSurface(EngineHandle handle, JNIEnv* env, jobject surface, int rotation) noexcept;
bool setVideoSurface(EngineHandle handle, JNIEnv* env, jobject surface, uint32_t width, uint32_t height,
                     uint32_t bitDepth) noexcept;
void setVideoPreviewCropOut(EngineHandle handle, uint32_t width, uint32_t height) noexcept;
void requestVideoPrewarm(EngineHandle handle, uint32_t width, uint32_t height, uint32_t bitDepth) noexcept;
void releaseVideoProcessing(EngineHandle handle) noexcept;
std::string videoStats(EngineHandle handle);
void setVideoImageSettings(EngineHandle handle, const rawrcam::video::VideoProcessingConfig& config) noexcept;
void setScopeDeviceRotationDegrees(EngineHandle handle, int rotation) noexcept;
void setCameraActive(EngineHandle handle, bool active) noexcept;
void setOisEnabled(EngineHandle handle, bool enabled) noexcept;
void setAntibandingMode(EngineHandle handle, uint8_t mode) noexcept;
void selectLens(EngineHandle handle, const std::string& lensId) noexcept;
bool setCameraProfile(EngineHandle handle, const std::string& json) noexcept;
void setPreferredCameraId(EngineHandle handle, const std::string& cameraId) noexcept;
void setPreferredAccessRoute(EngineHandle handle, const std::string& route) noexcept;
void setPreferredColorMode(EngineHandle handle, const std::string& mode) noexcept;
void setPipelineDiagnostic(EngineHandle handle, uint32_t mode) noexcept;
void setPersistentDiagnosticsEnabled(EngineHandle handle, bool enabled) noexcept;
void setPersistZslRingEnabled(EngineHandle handle, bool enabled) noexcept;
uint32_t prepareExperimentalMultiframe(EngineHandle handle, uint32_t maxFrames) noexcept;
uint64_t startPreparedMultiframeCapture(EngineHandle handle, rawrcam::encoding::dng::DngCaptureContext baseDng,
                                        rawrcam::encoding::dng::DngCaptureContext mergedDng,
                                        rawrcam::capture::JpegCaptureRequest mergedJpeg, bool dumpRzslRequested,
                                        rawrcam::capture::multiframe::MultiframeTuning tuning,
                                        rawrcam::capture::multiframe::MultiframeBaseFrameMode baseFrameMode) noexcept;
void cancelPreparedMultiframeCapture(EngineHandle handle) noexcept;
void setInternalTraceCaptureEnabled(EngineHandle handle, bool enabled) noexcept;
void setExperimentalZeroCopy(EngineHandle handle, bool enabled) noexcept;
void setExperimentalMultiframeEnabled(EngineHandle handle, bool enabled) noexcept;
void setHdrPlusBracketEnabled(EngineHandle handle, bool enabled) noexcept;
void setPersistentEngineEnabled(EngineHandle handle, bool enabled) noexcept;
void setLensShadingCorrectionEnabled(EngineHandle handle, bool enabled) noexcept;
void setHighlightReconstructionEnabled(EngineHandle handle, bool enabled) noexcept;
void setMaxAePostGain(EngineHandle handle, float gain) noexcept;
void setPostGainKneeWidthEv(EngineHandle handle, float widthEv) noexcept;
void setAutoMinFps(EngineHandle handle, int fps) noexcept;
void setAePriorityDisabled(EngineHandle handle, bool disabled) noexcept;
bool setRecordingFps(EngineHandle handle, int fps) noexcept;
void setPreviewForeground(EngineHandle handle, bool foreground) noexcept;
void setInitialConfigReady(EngineHandle handle) noexcept;
void setStillCaptureInFlight(EngineHandle handle, bool begin) noexcept;
void tickCameraSession(EngineHandle handle) noexcept;
bool canRecoverStills(EngineHandle handle) noexcept;
void setVideoMode(EngineHandle handle, bool video, int fps) noexcept;
void setShutterAngleDegrees(EngineHandle handle, double degrees) noexcept;
void setCpuRawCopyProbeFrames(EngineHandle handle, uint32_t frames) noexcept;
void setMonitoringOverlay(EngineHandle handle, uint32_t mode, float focusSensitivity) noexcept;
void setScopePresentationState(EngineHandle handle, const rawrcam::monitoring::ScopePresentationState& state) noexcept;
uint64_t requestRawStillCapture(EngineHandle handle, rawrcam::encoding::dng::DngCaptureContext dngContext,
                                rawrcam::capture::JpegCaptureRequest jpegContext) noexcept;
std::string pollDngWriteCompletion(EngineHandle handle);
std::string pollJpegWriteCompletion(EngineHandle handle);
uint64_t recoverStill(EngineHandle handle, const std::string& name, bool multiframe, int dng, int merged, int jpeg);
std::string runHighlightReplay(EngineHandle handle, const std::string& inputPath);
std::string cameraControlSnapshot(EngineHandle handle);
int videoRotationDegrees(EngineHandle handle, int deviceRotationDegrees);
std::string faceDetectionsSnapshot(EngineHandle handle);
void setExposureMode(EngineHandle handle, int mode) noexcept;
void setWhiteBalanceMode(EngineHandle handle, int mode, int64_t requestId) noexcept;
void setWhiteBalanceTempTint(EngineHandle handle, int32_t temperatureK, int32_t tint, int32_t editedAxes,
                             int64_t requestId) noexcept;
void setWhiteBalanceLocked(EngineHandle handle, int32_t temperatureK, int32_t tint, int64_t requestId) noexcept;
void setManualExposureTimeNs(EngineHandle handle, int64_t value) noexcept;
void setManualSensitivity(EngineHandle handle, int32_t value) noexcept;
void setExposureCompensationSteps(EngineHandle handle, int32_t value) noexcept;
void setSpotAeTarget(EngineHandle handle, bool active, float x, float y) noexcept;
void setFocusMode(EngineHandle handle, int mode, float x, float y, uint64_t requestId) noexcept;
void focusAt(EngineHandle handle, float x, float y, uint64_t requestId) noexcept;
void clearTapAf(EngineHandle handle, uint64_t requestId) noexcept;
void setManualFocusNormalized(EngineHandle handle, float value, uint64_t requestId) noexcept;
void setQuickToneNativeValue(EngineHandle handle, int target, float value) noexcept;
void setColorRenderProfile(EngineHandle handle, uint32_t profile, std::string importedProfileId) noexcept;
void setTonemapParameters(EngineHandle handle, float exposureEV, float blackPointEV, float shadowLiftEV,
                          float midtoneLiftEV, float contrast, float whitePointEV, float highlightBiasEV,
                          float saturation, float vibrance) noexcept;
void setAssetManager(EngineHandle handle, AAssetManager* assetManager) noexcept;
void setFilmSimEnabled(EngineHandle handle, bool enabled) noexcept;
void setViewfinderDivisor(EngineHandle handle, uint32_t divisor) noexcept;
void setFilmSimLook(EngineHandle handle, spektrafilm_native::FilmLook look) noexcept;
}  // namespace rawrcam::session
