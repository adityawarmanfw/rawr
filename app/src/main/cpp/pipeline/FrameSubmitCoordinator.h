#pragma once
#include <android/hardware_buffer.h>
#include <media/NdkImage.h>
#include <spektrafilm/SpektraFilm.h>
#include <tonemap/TonemapEngine.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "color/FrameColorTransform.h"
#include "imaging/FrameLimits.h"
#include "imaging/FramePairer.h"
#include "imaging/RawFrameIngress.h"
#include "metadata/FrameMetadataSnapshot.h"
#include "pipeline/FrameCapturePort.h"
#include "pipeline/FrameDiagnosticsPort.h"
#include "pipeline/FrameLifecyclePort.h"
#include "pipeline/RawCpuUploadPool.h"
#include "pipeline/RealtimeResources.h"

struct AAssetManager;

namespace raw_preview {
class RawPreview;
}
namespace rawrcam::imaging {
class FramePairer;
}
namespace rawrcam::presentation {
class SwapchainRenderer;
}
namespace rawrcam::vulkan {
class RawAhbImporter;
struct ImportedRaw;
class VulkanContext;
}  // namespace rawrcam::vulkan
namespace rawrcam::video {
class VideoSession;
}

namespace rawrcam::pipeline {
struct FrameSlot;
class FrameSlotPool;
}  // namespace rawrcam::pipeline
namespace rawrcam::geometry {
struct NormalizedPoint;
}  // namespace rawrcam::geometry
namespace rawrcam::diagnostics {
class GpuTimingTracker;
}
namespace rawrcam::pipeline {
class RawDevelopRecorder;

struct FrameSubmitConfiguration {
    uint64_t generation = 0;
    uint32_t rawWidth = 0;
    uint32_t rawHeight = 0;
    uint32_t previewWidth = 0;
    uint32_t previewHeight = 0;
    uint32_t cfa = 0;
    int sensorOrientationDegrees = 0;
    uint32_t diagnosticMode = 0;
    bool experimentalZeroCopy = false;
    bool lensShadingCorrectionEnabled = false;
};

// Owns the runtime state machine after Camera2 RAW/metadata enter the app and
// before a completed slot is retired. Resource creation/destruction and Android
// surface/session lifecycle remain SessionEngine responsibilities.
class FrameSubmitCoordinator final {
   public:
    FrameSubmitCoordinator(rawrcam::vulkan::VulkanContext& vulkanContext, pipeline::FrameSlotPool& frameSlots,
                           rawrcam::diagnostics::GpuTimingTracker& performanceTracker, RealtimeResources& resources,
                           rawrcam::presentation::SwapchainRenderer& presentationRenderer,
                           FrameLifecyclePort& lifecyclePort, FrameCapturePort& capturePort,
                           FrameDiagnosticsPort& diagnosticsPort, std::mutex& queueSubmitMutex, std::string filesDir);
    ~FrameSubmitCoordinator();

    FrameSubmitCoordinator(const FrameSubmitCoordinator&) = delete;
    FrameSubmitCoordinator& operator=(const FrameSubmitCoordinator&) = delete;

    void configure(const FrameSubmitConfiguration& config);
    void resetConfiguration() noexcept;
    // Destroy device-owned background still resources before VulkanContext.
    void shutdownDeviceResources() noexcept;
    void clearPairing() noexcept;

    void setDisplayRotationDegrees(int value) noexcept { displayRotationDegrees_ = value; }
    void setPresentationPaused(bool value) noexcept { presentationPaused_ = value; }
    void setVideoOutput(rawrcam::video::VideoSession* value) noexcept { videoOutput_ = value; }
    // Where recording frames are lost. Only counted while the encoder Surface is attached.
    struct VideoDropReasons {
        uint64_t noSlot = 0;       // all GPU frame slots still in flight
        uint64_t encoderBusy = 0;  // encoder swapchain had no free image
        uint64_t presentFail = 0;  // vkQueuePresentKHR to the encoder failed
        uint64_t pairer = 0;       // RAW image or metadata trimmed before pairing
        uint64_t stale = 0;        // frame from an old camera configuration
        uint64_t submitFail = 0;   // exception while recording or submitting
        uint64_t geometry = 0;     // video extent larger than the RAW frame
    };
    void resetVideoCounters() noexcept {
        videoSubmitted_ = 0;
        videoDrops_ = 0;
        previewSkippedDuringVideo_ = 0;
        videoDropReasons_ = {};
        lastVideoSubmitFailure_.clear();
    }
    [[nodiscard]] const VideoDropReasons& videoDropReasons() const noexcept { return videoDropReasons_; }
    // Error text of the submit failure that stopped the last recording, if any.
    [[nodiscard]] const std::string& lastVideoSubmitFailure() const noexcept { return lastVideoSubmitFailure_; }
    [[nodiscard]] uint64_t videoSubmitted() const noexcept { return videoSubmitted_; }
    [[nodiscard]] uint64_t videoDrops() const noexcept { return videoDrops_; }
    [[nodiscard]] uint64_t previewSkippedDuringVideo() const noexcept { return previewSkippedDuringVideo_; }
    void setScopeDeviceRotationDegrees(int value) noexcept { scopeDeviceRotationDegrees_ = value; }
    void setTonemapParams(const tonemap::TonemapParams& value) { tonemapParams_ = value; }
    void setFilmSimEnabled(bool value) noexcept { filmSimEnabled_ = value; }
    void setFilmSimLook(const spektrafilm_native::FilmLook& value) { filmLook_ = value; }
    // Idle video-mode preview crop: recording output size (0 = full frame).
    // Written on the camera worker, read on the submit thread.
    void setVideoPreviewCropOut(uint32_t width, uint32_t height) noexcept {
        videoCropOutWidth_.store(width, std::memory_order_relaxed);
        videoCropOutHeight_.store(height, std::memory_order_relaxed);
    }
    // Preview-only downsample divisor (2..4, default 2). Per-frame, no
    // restart or engine rebuild: the arena stays sized for 2.
    void setViewfinderDivisor(uint32_t value) noexcept { viewfinderDivisor_ = std::clamp(value, 1u, 4u); }
    void setColorMode(std::string value) { colorMode_ = std::move(value); }
    void setDiagnosticMode(uint32_t mode) noexcept {
        diagnosticMode_ = mode;
        config_.diagnosticMode = mode;
    }
    [[nodiscard]] uint32_t diagnosticMode() const noexcept { return diagnosticMode_; }
    void setExperimentalZeroCopy(bool enabled) noexcept {
        experimentalZeroCopy_ = enabled;
        config_.experimentalZeroCopy = enabled;
    }
    [[nodiscard]] bool experimentalZeroCopy() const noexcept { return experimentalZeroCopy_; }
    void setLensShadingCorrectionEnabled(bool enabled) noexcept {
        lensShadingCorrectionEnabled_ = enabled;
        config_.lensShadingCorrectionEnabled = enabled;
    }
    void setHighlightReconstructionEnabled(bool enabled) noexcept { highlightReconstructionEnabled_ = enabled; }
    void setHighlightMethod(uint32_t method, float threshold, float compression) noexcept {
        highlightMethod_ = method;
        highlightThreshold_ = threshold;
        highlightCompression_ = compression;
    }
    [[nodiscard]] bool lensShadingCorrectionEnabled() const noexcept { return lensShadingCorrectionEnabled_; }

    bool submitMetadata(const rawrcam::metadata::FrameMetadataSnapshot& metadata);
    void onRawFrame(rawrcam::imaging::AcquiredRawFrame frame);
    void releaseCompletedSlots(bool force);

    [[nodiscard]] VkImageUsageFlags importedRawUsage() const noexcept;

    // CPU RAW ingress: latched for the process after the first failed GPU
    // import (the driver rejects this device's camera AHBs), or forced for
    // testing. Readers must then be CPU-readable; RAW10 streams always are.
    [[nodiscard]] bool cpuIngressRequired() const noexcept {
        return cpuIngressLatched_.load(std::memory_order_relaxed) || forceCpuIngress_.load(std::memory_order_relaxed);
    }
    // True once this configuration delivered a frame through CPU upload
    // (latched fallback, forced, or a RAW10 stream). Surfaced to the UI.
    [[nodiscard]] bool cpuIngressActive() const noexcept { return cpuIngressActive_.load(std::memory_order_relaxed); }
    void setForceCpuIngress(bool enabled) noexcept { forceCpuIngress_.store(enabled, std::memory_order_relaxed); }
    // Frees CPU-upload resources. Caller must have the GPU idle.
    void releaseCpuUpload() noexcept { cpuUpload_.destroy(); }

   private:
    void tryPair(uint64_t timestampNs);
    bool submitMatchedFrame(rawrcam::imaging::MatchedFrame& matched);
    // Pairer backlog cap + pressure trim. The camera reader owns maxImages=6
    // AImages while slots lease up to 3; capping pairer backlog at 2 keeps
    // total live <= 5 with headroom, so a backlog can never wedge the
    // reader's BufferQueue (self-sustaining freeze, app alive).
    // Three GPU slots + one ingress image + one unmatched image leave one
    // AImageReader lease free (maxImages=6) during ordinary preview operation.
    static constexpr size_t kMaxPairerImages = imaging::kMaxPairerImages;
    static constexpr size_t kMaxPairerMetadata = imaging::kMaxPairerMetadata;
    void trimPairer();

    // submitAhb phases. Acquire may clean-drop (nullopt, no recovery); record
    // and submit throw on failure so the orchestrator runs recovery once.
    struct SubmitParams {
        std::uint64_t timestampNs = 0;
        AHardwareBuffer* ahb = nullptr;
        int acquireFenceFd = -1;
        AImage* imageLease = nullptr;
        std::array<float, 4> black{};
        float white = 0.0f;
        std::array<float, 4> wb{};
        std::array<float, 9> sensorToSrgb{};
        const rawrcam::metadata::FrameMetadataSnapshot* metadata = nullptr;
        const rawrcam::color::FrameColorTransform* colorState = nullptr;
    };
    struct AcquiredSubmit {
        pipeline::FrameSlot* slot = nullptr;
        std::uint32_t slotIndex = 0;
        std::optional<std::uint32_t> swapIndex;
        std::optional<std::uint32_t> videoIndex;
        rawrcam::vulkan::ImportedRaw* raw = nullptr;
    };
    struct RecordedSubmit {
        VkPipelineStageFlags rawWaitStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    };
    std::optional<AcquiredSubmit> acquireSubmitSlot(const SubmitParams& params, pipeline::FrameSlot* slot,
                                                    uint32_t slotIndex);
    RecordedSubmit recordSubmitCommands(const SubmitParams& params, const AcquiredSubmit& acquired);
    // Live post gain; tagged one-shot frames (HDR+ bracket dark frames) are
    // lifted to the last repeating frame's exposure so the viewfinder does
    // not flash dark while they pass through.
    float previewPostGain(const rawrcam::metadata::FrameMetadataSnapshot& metadata);
    void submitSplitVideoAndMonitor(const SubmitParams& params, const AcquiredSubmit& acquired, bool& submitted);
    // Sets submitted=true once the queue submit succeeds; a later present
    // failure still leaves the slot submitted (recovered by fence retire).
    void submitAndPresent(const SubmitParams& params, const AcquiredSubmit& acquired, const RecordedSubmit& recorded,
                          bool& submitted);
    void recoverFailedSubmit(std::uint32_t slotIndex, bool slotSelected, bool queueSubmitted,
                             bool swapchainImageAcquired, const char* error);
    bool submitAhb(uint64_t timestampNs, AHardwareBuffer* ahb, int acquireFenceFd, AImage* imageLease,
                   const std::array<float, 4>& black, float white, const std::array<float, 4>& wb,
                   const std::array<float, 9>& sensorToSrgb, const rawrcam::metadata::FrameMetadataSnapshot& metadata,
                   const rawrcam::color::FrameColorTransform& colorState);

    // Slot retirement phases. pollSlotFence reports readiness; retireSlot
    // reclaims the slot; the report* helpers fan retired-frame feedback out.
    bool pollSlotFence(pipeline::FrameSlot& slot, bool force);
    void retireSlot(std::uint32_t slotIndex, pipeline::FrameSlot& slot);
    void reportRenderedFeedback(std::uint32_t slotIndex, const pipeline::FrameSlot& slot);
    void reportAuditFrame(const pipeline::FrameSlot& slot);

    rawrcam::vulkan::VulkanContext& vulkanContext_;
    pipeline::FrameSlotPool& frameSlots_;
    rawrcam::diagnostics::GpuTimingTracker& performanceTracker_;
    RealtimeResources& resources_;
    rawrcam::presentation::SwapchainRenderer& swapchainRenderer_;
    FrameLifecyclePort& lifecyclePort_;
    FrameCapturePort& capturePort_;
    FrameDiagnosticsPort& diagnosticsPort_;
    std::mutex& queueSubmitMutex_;
    std::string filesDir_;

    std::unique_ptr<rawrcam::imaging::FramePairer> framePairer_;
    FrameSubmitConfiguration config_{};
    bool configured_ = false;
    bool presentationPaused_ = true;
    rawrcam::video::VideoSession* videoOutput_ = nullptr;
    uint64_t videoSubmitted_ = 0;
    uint64_t videoDrops_ = 0;
    // Live RAW content sampling (logcat, at most once per second of sensor time).
    uint64_t lastRawContentStatsNs_ = 0;
    double lastRepeatingExposure_ = 0.0;  // exposure time x sensitivity of the last untagged frame
    std::string lastSubmitFailure_;
    uint64_t repeatedSubmitFailures_ = 0;
    RawCpuUploadPool cpuUpload_;
    std::atomic<bool> cpuIngressLatched_{false};
    std::atomic<bool> forceCpuIngress_{false};
    std::atomic<bool> cpuIngressActive_{false};
    bool cpuIngressLogged_ = false;
    // Set when the RAW10 GPU unpack failed once; RAW10 then stays on the CPU upload.
    std::atomic<bool> raw10GpuUnpackLatched_{false};
    bool raw10GpuUnpackLogged_ = false;
    bool raw10ParityEnabled_ = false;  // debug.rawr.raw10_parity
    uint32_t raw10ParityFrame_ = 0;
    rawrcam::vulkan::ImportedRaw& ingestRaw(const SubmitParams& params, uint32_t slotIndex);
    VideoDropReasons videoDropReasons_{};
    std::string lastVideoSubmitFailure_;
    [[nodiscard]] bool videoActive() const noexcept;
    uint64_t previewSkippedDuringVideo_ = 0;
    int displayRotationDegrees_ = 0;
    int scopeDeviceRotationDegrees_ = 0;
    tonemap::TonemapParams tonemapParams_{};
    // Written on the camera worker, read on the submit thread.
    std::atomic<bool> filmSimEnabled_ = false;
    std::atomic<uint32_t> videoCropOutWidth_{0};
    std::atomic<uint32_t> videoCropOutHeight_{0};
    spektrafilm_native::FilmLook filmLook_{};
    uint32_t viewfinderDivisor_ = 2;
    std::string colorMode_ = "auto";
    uint32_t pipelineAuditMetadataCount_ = 0;
    uint32_t pipelineAuditPairCount_ = 0;
    uint32_t pipelineAuditSubmitCount_ = 0;
    uint32_t metadataDiagnosticCount_ = 0;
    uint32_t pairerPressureLogCount_ = 0;
    uint64_t multiframeStarveLoggedGeneration_ = 0;
    uint32_t diagnosticMode_ = 0;
    bool experimentalZeroCopy_ = false;
    bool lensShadingCorrectionEnabled_ = false;
    // User settings outlive per-camera geometry and resource reconfiguration.
    bool highlightReconstructionEnabled_ = true;
    uint32_t highlightMethod_ = 0;
    float highlightThreshold_ = 1.0f;
    float highlightCompression_ = 163.0f;
};

}  // namespace rawrcam::pipeline
