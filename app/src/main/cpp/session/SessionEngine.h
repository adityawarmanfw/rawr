#pragma once
#include <android/asset_manager.h>
#include <android/hardware_buffer.h>
#include <android/log.h>
#include <jni.h>
#include <media/NdkImage.h>
#include <spektrafilm/SpektraFilm.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "camera/CameraControlSurface.h"
#include "camera/CameraControlTypes.h"
#include "capture/CaptureCoordinator.h"
#include "capture/CaptureRequest.h"
#include "diagnostics/logging/DiagnosticSink.h"
#include "diagnostics/logging/FrameAuditWriter.h"
#include "diagnostics/probes/PipelineDiagnostics.h"
#include "diagnostics/probes/RawCpuCopyProbe.h"
#include "diagnostics/probes/RawIntegrityProbe.h"
#include "geometry/DisplayToSensorMapper.h"
#include "imaging/FrameIngressQueue.h"
#include "monitoring/MonitoringCoordinator.h"
#include "pipeline/FrameSubmitCoordinator.h"
#include "pipeline/PreviewLookController.h"
#include "pipeline/RealtimePipeline.h"
#include "presentation/PresentationSurface.h"
#include "presentation/SwapchainRenderer.h"
#include "session/SessionFrameCallbacks.h"
#include "video/VideoSession.h"
#include "vulkan/VulkanContext.h"

namespace rawrcam::session {
class SessionEngine {
   public:
    explicit SessionEngine(std::string filesDir);
    ~SessionEngine();
    // Camera-domain surface used by the C-API forwards. All locking and
    // cross-domain sequencing stay inside the called methods.
    [[nodiscard]] rawrcam::camera::CameraControlSurface& cameraControls() noexcept { return cameraControls_; }
    bool setSurface(JNIEnv* env, jobject surfaceObj, int displayRotationDegrees);
    bool setVideoSurface(JNIEnv* env, jobject surfaceObj, uint32_t width, uint32_t height, uint32_t bitDepth);
    // Idle video-mode preview crop: recording output size (0 = full frame).
    // Shows exactly the record window in the viewfinder before recording
    // starts, so framing never jumps. No-op for Open Gate (already full).
    void setVideoPreviewCropOut(uint32_t width, uint32_t height);
    // Keeps the recording processing for this size (0x0 = full RAW, Open
    // Gate) built ahead of recordings. The prewarm thread builds it off the
    // engine lock and rebuilds after camera reconfiguration and recordings.
    void requestVideoPrewarm(uint32_t width, uint32_t height, uint32_t bitDepth);
    // Frees it again when leaving Video mode.
    void releaseVideoProcessing();
    std::string videoStats();
    void setVideoImageSettings(const rawrcam::video::VideoProcessingConfig& config);
    void setScopeDeviceRotationDegrees(int rotation);
    void setExposureMode(int mode);
    void setManualExposureTimeNs(int64_t value);
    void setManualSensitivity(int32_t value);
    void setExposureCompensationSteps(int32_t value);
    void setWhiteBalanceMode(int mode, int64_t requestId);
    void setWhiteBalanceTempTint(int32_t temperatureK, int32_t tint, int32_t editedAxes, int64_t requestId);
    void setWhiteBalanceLocked(int32_t temperatureK, int32_t tint, int64_t requestId);
    void setFocusMode(int mode, float x, float y, uint64_t requestId);
    void setManualFocusNormalized(float value, uint64_t requestId);
    void setQuickToneNativeValue(int target, float value);
    void setTonemapParameters(float exposureEV, float blackPointEV, float shadowLiftEV, float midtoneLiftEV,
                              float contrast, float whitePointEV, float highlightBiasEV, float saturation,
                              float vibrance);
    void setFilmSimEnabled(bool enabled);
    void setViewfinderDivisor(uint32_t divisor);
    void setFilmSimLook(const spektrafilm_native::FilmLook& look);
    void setAssetManager(AAssetManager* assetManager);
    void setColorRenderProfile(uint32_t profileId, std::string importedProfileId);
    void setMonitoringOverlay(uint32_t mode, float focusSensitivity);
    void setScopePresentationState(const rawrcam::monitoring::ScopePresentationState& state);
    void focusAtDisplay(float x, float y, uint64_t requestId);
    void clearTapAf(uint64_t requestId);
    // Face detections in display-normalized coordinates for the viewfinder
    // overlay, as a JSON array [[x,y,w,h,score],...]. Empty array when none.
    std::string faceDetectionsDisplaySnapshot();
    [[nodiscard]] bool rawCpuIngressActive() const noexcept { return realtime_.coordinator().cpuIngressActive(); }
    void setSpotAeDisplay(bool active, float x, float y);
    void setColorMode(const std::string& mode);
    void setCpuRawCopyProbeFrames(uint32_t frames);
    uint64_t requestRawStillCapture(rawrcam::encoding::dng::DngCaptureContext dngContext,
                                    rawrcam::capture::JpegCaptureRequest jpegContext);
    uint64_t recoverStill(const std::string& name, bool multiframe, int dng, int merged, int jpeg);
    std::string runHighlightReplay(const std::string& inputPath);
    std::string pollDngWriteCompletion();
    std::string pollJpegWriteCompletion();
    void setPersistZslRingEnabled(bool enabled);
    void setExperimentalMultiframeEnabled(bool enabled);
    // HDR+ Bracketed selected (with multiframe on): allows the DCG ISO calibration frame.
    void setHdrPlusBracketEnabled(bool enabled);
    void setPersistentEngineEnabled(bool enabled);
    uint32_t prepareExperimentalMultiframe(uint32_t maxFrames);
    uint64_t startPreparedMultiframeCapture(rawrcam::encoding::dng::DngCaptureContext baseDng,
                                            rawrcam::encoding::dng::DngCaptureContext mergedDng,
                                            rawrcam::capture::JpegCaptureRequest mergedJpeg, bool dumpRzslRequested,
                                            rawrcam::capture::multiframe::MultiframeTuning tuning,
                                            rawrcam::capture::multiframe::MultiframeBaseFrameMode baseFrameMode =
                                                rawrcam::capture::multiframe::MultiframeBaseFrameMode::Sharpest);
    void cancelPreparedMultiframeCapture();
    void setPersistentDiagnosticsEnabled(bool enabled);
    void setLensShadingCorrectionEnabled(bool enabled);
    void setHighlightReconstructionEnabled(bool enabled);
    void setMaxAePostGain(float gain);
    [[nodiscard]] float maxAePostGain() const noexcept { return maxAePostGain_.load(std::memory_order_relaxed); }
    void setPostGainKneeWidthEv(float widthEv);
    [[nodiscard]] float postGainKneeWidthEv() const noexcept {
        return postGainKneeWidthEv_.load(std::memory_order_relaxed);
    }
    void setExperimentalZeroCopy(bool enabled);
    void setPipelineDiagnostic(uint32_t mode);
    bool submitMetadata(const rawrcam::metadata::FrameMetadataSnapshot& metadata);
    void recordPipelineAuditLine(const std::string& line);
    void recordDiagnosticLine(const std::string& line);

   private:
    friend class SessionFrameCallbacks;
    void appendDiagnostic(const std::string& line);
    // Current idle-preview crop window in preview-source pixels for focus,
    // spot-AE, and face-box mapping. Call with mu_ held. Identity (disabled)
    // unless video mode armed with a valid in-sensor output size.
    rawrcam::geometry::SourceCropRect previewVideoCropRectLocked() const;
    void shutdown();
    bool setPresentationSurface(JNIEnv* env, jobject surfaceObj, int displayRotationDegrees);
    uint64_t cameraSetupGeneration_ = 0;
    uint64_t cameraReaderGeneration_ = 0;
    ANativeWindow* createRawInputWindow(uint64_t generation, uint32_t width, uint32_t height,
                                        rawrcam::geometry::RawPixelFormat format);
    void destroyRawInput();
    void destroyRawInputInternal() noexcept;
    bool hqStillDetachedWorkActive() const noexcept;
    void detachPresentationSurface() noexcept;
    void finalizeDeferredSurfaceDetach();
    void recreateSwapchainAfterOutOfDate();
    void enqueueRawFrame(rawrcam::imaging::AcquiredRawFrame frame);
    void consumeIngress(imaging::FrameIngressQueue::Event event, imaging::FrameIngressQueue::Drops drops);
    void stopIngress();
    // Schedules the requested prewarm again (camera reconfigured, recording ended).
    void markVideoPrewarmDirty();
    void waitForVideoPrewarmIdle();

    rawrcam::camera::CameraControlSurface cameraControls_;

    std::mutex mu_;
    imaging::FrameIngressQueue ingressQueue_{
        [this](imaging::FrameIngressQueue::Event event, imaging::FrameIngressQueue::Drops drops) {
            consumeIngress(std::move(event), drops);
        }};
    std::atomic<uint64_t> videoIngressDrops_{0};
    uint64_t ingressSlowReports_ = 0;
    std::string filesDir_;

    std::unique_ptr<rawrcam::diagnostics::DiagnosticSink> diagnosticSink_;
    std::unique_ptr<rawrcam::diagnostics::FrameAuditWriter> frameAuditWriter_;

    rawrcam::vulkan::VulkanContext vulkanContext_;
    std::mutex& queueSubmitMutex_ = vulkanContext_.primaryQueue().mutex();
    pipeline::PreviewLookController look_{
        vulkanContext_,
        mu_,
        queueSubmitMutex_,
        filesDir_,
        [this](const std::string& line) { appendDiagnostic(line); },
        [this](bool enabled, const spektrafilm_native::FilmLook& look) {
            realtime_.coordinator().setFilmSimLook(look);
            realtime_.coordinator().setFilmSimEnabled(enabled);
        },
        [this](const tonemap::TonemapParams& tone) { realtime_.coordinator().setTonemapParams(tone); },
        [this](uint32_t divisor) { realtime_.coordinator().setViewfinderDivisor(divisor); }};
    AAssetManager* replayAssetManager_ = nullptr;  // Borrowed APK assets for frozen replay.
    rawrcam::camera::CameraControlCapabilities cameraCapabilities_{};
    rawrcam::diagnostics::RawCpuCopyProbe rawCpuCopyProbe_{[this](const std::string& line) { appendDiagnostic(line); }};
    rawrcam::monitoring::MonitoringCoordinator monitoringCoordinator_{
        [this](const std::string& line) { appendDiagnostic(line); }};
    rawrcam::diagnostics::RawIntegrityProbe rawIntegrityProbe_{
        [this](const std::string& line) { appendDiagnostic(line); }};
    rawrcam::presentation::SwapchainRenderer swapchainRenderer_{
        [this](const std::string& line) { appendDiagnostic(line); }};
    rawrcam::video::VideoSession videoSession_{
        vulkanContext_, mu_, [this] { return VkExtent2D{realtime_.rawWidth(), realtime_.rawHeight()}; },
        [this] { return look_.videoRenderLut(); }, [this](const std::string& line) { appendDiagnostic(line); }};
    // Armed idle-preview crop output size (mu_). Zero disables.
    uint32_t videoCropOutWidth_ = 0;
    uint32_t videoCropOutHeight_ = 0;
    rawrcam::diagnostics::PipelineDiagnostics pipelineDiagnostics_{
        [this](const std::string& line) { appendDiagnostic(line); }};
    capture::CaptureCoordinator capture_{
        vulkanContext_, queueSubmitMutex_, filesDir_, [this](const std::string& line) { appendDiagnostic(line); },
        [this](const metadata::FrameMetadataSnapshot& metadata) { return pipelineCallbacks_.postGainFor(metadata); }};

    SessionFrameCallbacks pipelineCallbacks_{this};
    pipeline::RealtimePipeline realtime_{filesDir_ + "/raw_preview.comp.spv",
                                         filesDir_ + "/raw_preview_cfa_state.comp.spv",
                                         filesDir_,
                                         vulkanContext_,
                                         look_,
                                         monitoringCoordinator_,
                                         rawIntegrityProbe_,
                                         pipelineDiagnostics_,
                                         swapchainRenderer_,
                                         rawCpuCopyProbe_,
                                         pipelineCallbacks_,
                                         pipelineCallbacks_,
                                         pipelineCallbacks_,
                                         queueSubmitMutex_,
                                         [this](const std::string& line) { appendDiagnostic(line); },
                                         [this](const std::string& line) { recordPipelineAuditLine(line); }};

    imaging::RawFrameIngress rawFrameIngress_{
        [this](imaging::AcquiredRawFrame frame) { enqueueRawFrame(std::move(frame)); },
        [this](const std::string& line) { appendDiagnostic(line); },
        [this](const std::string& line) { recordPipelineAuditLine(line); }};
    presentation::PresentationSurface presentationSurface_;
    bool deferredSurfaceDetach_ = false;

    std::string colorMode_ = "auto";
    uint32_t diagnosticMode_ = 0;
    // User-configured AE post-gain linear cap (1, 2, or 4; default 4).
    // Read by PipelineCallbacks::postGainFor via AePostGainCurve soft knee.
    // Atomic: written under mu_ from Kotlin sync, read lock-free per frame.
    std::atomic<float> maxAePostGain_{4.0f};
    // HighlightProtection-driven knee width in EV (Low=2, Normal=1, High=0.5).
    std::atomic<float> postGainKneeWidthEv_{1.0f};
};
}  // namespace rawrcam::session
