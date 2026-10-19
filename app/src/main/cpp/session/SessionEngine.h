#pragma once
#include <android/asset_manager.h>
#include <android/hardware_buffer.h>
#include <android/log.h>
#include <jni.h>
#include <media/NdkImage.h>
#include <spektrafilm/SpektraFilm.h>
#include <unistd.h>

#include <atomic>
#include <map>
#include <set>
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
#include "color/AutoKeyExposure.h"
#include "capture/CaptureCoordinator.h"
#include "capture/CaptureRequest.h"
#include "diagnostics/logging/DiagnosticSink.h"
#include "diagnostics/logging/FrameAuditWriter.h"
#include "diagnostics/probes/PipelineDiagnostics.h"
#include "diagnostics/probes/RawCpuCopyProbe.h"
#include "diagnostics/probes/RawIntegrityProbe.h"
#include "geometry/DisplayToSensorMapper.h"
#include "imaging/FrameIngressQueue.h"
#include "imaging/RawReaderUsageLadder.h"
#include "monitoring/MonitoringCoordinator.h"
#include "pipeline/FrameSubmitCoordinator.h"
#include "pipeline/PreviewLookController.h"
#include "pipeline/RealtimePipeline.h"
#include "presentation/PresentationSurface.h"
#include "presentation/SwapchainRenderer.h"
#include "session/SessionFrameCallbacks.h"
#include "local_tone/ExposureFusion.h"
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
    // Demotes a RAW reader that never delivered a frame to CPU-only usage and
    // restarts the session. Call on the serial lane before tickSession().
    void tickRawReaderUsage();
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
    void setAgxParameters(const float* values);
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
    [[nodiscard]] capture::multiframe::MultiframeCaptureCoordinator::BracketHold bracketHold() const noexcept {
        return capture_.multiframe().bracketHold();
    }
    // Full-size route availability and outstanding RAW acquisition, for the hold notice.
    std::pair<bool, bool> fullSizeCaptureState();
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
    // HDR+ Bracketed selected (with multiframe on): maxEv < 0 is the most
    // highlight headroom the meter may take (ZSL underexposure); 0 = off.
    // Also allows the ISO calibration probe frame for the long frames.
    void setHdrPlusHeadroomMaxEv(float maxEv);
    [[nodiscard]] float hdrHeadroomMaxEv() const noexcept { return hdrHeadroomMaxEv_.load(std::memory_order_relaxed); }
    // Headroom EV (<= 0) a delivered frame was exposed with, from its actual
    // exposure against the normalizer's reference (ExposureNormalizer); its
    // reported compensation when the frame was not normalized; 0 when none.
    [[nodiscard]] float hdrHeadroomEvFor(const metadata::FrameMetadataSnapshot& metadata) const;
    // The compensation part only, as the frame's result reports it.
    [[nodiscard]] float reportedHeadroomEvFor(const metadata::FrameMetadataSnapshot& metadata) const;
    void setPersistentEngineEnabled(bool enabled);
    // longFrames: HDR+ Bracketed post-shutter frames planned after these
    // maxFrames burst frames; frozen as burst frames when they would be skipped.
    uint32_t prepareExperimentalMultiframe(uint32_t maxFrames, uint32_t longFrames = 0u);
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
    void setAutoKeyMode(int mode);
    [[nodiscard]] rawrcam::color::AutoKeyMode autoKeyMode() const noexcept {
        return rawrcam::color::autoKeyModeFromId(autoKeyMode_.load(std::memory_order_relaxed));
    }
    // Snapshot the adaptive curve independently of HAL gain; LOG always bypasses it.
    [[nodiscard]] float appliedAutoKeyEv() const noexcept {
        if (autoKeyMode() == rawrcam::color::AutoKeyMode::Off || sceneReferredRender_.load(std::memory_order_relaxed))
            return 0.0f;
        std::lock_guard<std::mutex> lock(autoKeyMutex_);
        return autoKeyCurve_.liftEv;
    }
    // Photo tone mapping: local (exposure fusion, shadows EV > 0) replaces the
    // adaptive shadow-lift curve; 0 keeps the curve. The app sends 0 in video.
    // LOG stays scene-referred.
    void setLocalTone(float shadowsEv, float highlightsEv, float sigma, unsigned coarsestMip, bool boostLocalContrast);
    // Frames the next photo merges (1 = single frame): the viewfinder's
    // noise limit then matches the still's.
    void setExpectedMergeFrames(std::uint32_t frames) noexcept {
        expectedMergeFrames_.store(std::max<std::uint32_t>(1u, frames), std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint32_t expectedMergeFrames() const noexcept {
        return expectedMergeFrames_.load(std::memory_order_relaxed);
    }
    // Noise of a rendered frame for the local tone lift limit: the frame's
    // sensor noise model, its render gain and the frames merged into it.
    [[nodiscard]] static rawr::local_tone::LiftNoise liftNoiseFor(const metadata::FrameMetadataSnapshot& m,
                                                                  float renderGain, std::uint32_t frames) noexcept;
    // The latest viewfinder frame's noise model (for a still whose frame is
    // picked later).
    [[nodiscard]] rawr::local_tone::LiftNoise latestLiftNoise(float renderGain) const noexcept {
        return {latestNoiseShot_.load(std::memory_order_relaxed), latestNoiseRead_.load(std::memory_order_relaxed),
                renderGain, 1u};
    }
    void noteLiftNoise(const rawr::local_tone::LiftNoise& n) noexcept {
        latestNoiseShot_.store(n.shot, std::memory_order_relaxed);
        latestNoiseRead_.store(n.read, std::memory_order_relaxed);
    }
    [[nodiscard]] tonemap::LocalToneParams localToneFor(const tonemap::TonemapParams& tone,
                                                        const rawr::local_tone::LiftNoise& noise) const noexcept {
        const float shadows = localToneShadowsEv_.load(std::memory_order_relaxed);
        if (!(shadows > 0.0f) || sceneReferredRender_.load(std::memory_order_relaxed) ||
            tone.renderTransform == tonemap::RenderTransform::Log)
            return {};
        return {rawr::local_tone::noiseLimitedShadowsEv(shadows, noise),
                localToneHighlightsEv_.load(std::memory_order_relaxed), localToneSigma_.load(std::memory_order_relaxed),
                localToneMip_.load(std::memory_order_relaxed), unsigned(localToneBoost_.load(std::memory_order_relaxed))};
    }
    // A default noise (unknown model) applies no limit.
    void applyAutoKey(tonemap::TonemapParams& tone, const rawr::local_tone::LiftNoise& noise = {}) const {
        tone.localTone = localToneFor(tone, noise);
        const bool local = tone.localTone.shadowsEv > 0.0f;
        std::lock_guard<std::mutex> lock(autoKeyMutex_);
        tone.adaptiveTone = local || autoKeyMode() == rawrcam::color::AutoKeyMode::Off ||
                                    sceneReferredRender_.load(std::memory_order_relaxed) ||
                                    tone.renderTransform == tonemap::RenderTransform::Log
                                ? tonemap::AdaptiveToneParams{}
                                : autoKeyCurve_;
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
    imaging::RawReaderUsageLadder rawReaderUsage_;  // guarded by mu_
    // LensRoute::readerUsage of the configured camera; overrides the ladder. Guarded by mu_.
    std::optional<uint64_t> profileReaderUsage_;
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
    // Full-size stills of a capture-stream lens (vivo seamless route); its
    // frames bypass the viewfinder.
    imaging::RawFrameIngress captureFrameIngress_{
        [this](imaging::AcquiredRawFrame frame) { onFullSizeFrame(std::move(frame)); },
        [this](const std::string& line) { appendDiagnostic(line); },
        [this](const std::string& line) { recordPipelineAuditLine(line); }};
    uint64_t captureReaderGeneration_ = 0;
    std::atomic<uint32_t> fullSizeFrames_{0};
    void onFullSizeFrame(imaging::AcquiredRawFrame frame);
    // Full-size stills of a capture-stream lens: each still also lands binned
    // (same timestamp) in the viewfinder stream, where the photo claims it;
    // the photo then snapshots the full-size frame instead.
    struct FullSizeStills {
        std::mutex mutex;
        std::condition_variable arrived;
        uint64_t generation = 0;
        bool failed = false;
        std::set<uint64_t> failures;
        std::map<uint64_t, metadata::FrameMetadataSnapshot> results;
        std::deque<imaging::RawFrameLease> frames;  // newest last, at most kMaxFrames
        metadata::CameraContextMetadataPtr context;  // the session's context at full size
        std::set<uint64_t> requestIds;               // photos waiting for a full-size still
        std::set<uint64_t> claimedTimestamps;        // claimed binned copies awaiting their snapshot
        // Merged bursts: their tags, and each frame's result (full-size context) by timestamp.
        std::set<uint64_t> burstTags;
        std::map<uint64_t, std::pair<metadata::FrameMetadataSnapshot, color::FrameColorTransform>> burstResults;
        static constexpr size_t kMaxFrames = 6;
    };
    FullSizeStills fullSize_;
    void drainFullSizeStills();  // session worker, under mu_; independent of preview pairing
    std::atomic<uint64_t> failedCameraGeneration_{0};
    metadata::CameraContextMetadataPtr lastCameraContext_;
    // The full-size context when `metadata` is a full-size still's binned copy (and marks it claimed).
    metadata::CameraContextMetadataPtr claimFullSizeStill(const metadata::FrameMetadataSnapshot& metadata);
    bool fullSizeStillClaimed(uint64_t timestampNs);
    bool isFullSizeStillCopy(const metadata::FrameMetadataSnapshot& metadata);
    void observeFullSizeBurstFrame(const metadata::FrameMetadataSnapshot& metadata,
                                   const color::FrameColorTransform& color);
    // Merged mode on a capture-stream lens: requests a burst of full-size
    // stills, waits for them, uploads them and prepares the merge. Frames, or 0.
    uint32_t prepareFullSizeBurst(uint32_t maxFrames);
    uint64_t nextFullSizeBurstTag_ = 1ull << 62;  // apart from photo request ids
    // A capture-stream lens: photos come only from its full-size stills, never a binned viewfinder frame.
    bool fullSizeStillsOnly();
    bool fullSizeStillPending();
    // Snapshots the full-size frame of a claimed still; on success also deletes `binned`, which the caller hands over.
    bool snapshotFullSizeStill(AImage* binned, uint64_t timestampNs);
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
    // Auto scene key exposure (AutoKeyExposure.h). The mode is written from
    // Kotlin sync; the smoothed lift by SessionFrameCallbacks at slot retire;
    // curve snapshots are protected separately from the session mutex.
    std::atomic<int> autoKeyMode_{0};
    mutable std::mutex autoKeyMutex_;
    tonemap::AdaptiveToneParams autoKeyCurve_{};
    // HDR+ Bracketed headroom floor (EV < 0) or 0 when the meter is off.
    std::atomic<float> hdrHeadroomMaxEv_{0.0f};
    // The prepared HDR+ Bracketed capture froze its long-frame slots as burst
    // frames (long frames would be skipped). Guarded by mu_.
    bool bracketSkippedForPending_ = false;
    // Image > Shadow Lift: photo local tone mapping shadows EV (0 = adaptive curve).
    std::atomic<float> localToneShadowsEv_{0.0f};
    std::atomic<float> localToneHighlightsEv_{2.0f}, localToneSigma_{5.0f};
    std::atomic<unsigned> localToneMip_{6};
    std::atomic<bool> localToneBoost_{false};
    std::atomic<std::uint32_t> expectedMergeFrames_{1u};
    std::atomic<float> latestNoiseShot_{0.0f}, latestNoiseRead_{0.0f};
    // LOG profiles stay scene-referred: no auto key lift.
    std::atomic<bool> sceneReferredRender_{false};
};
}  // namespace rawrcam::session
