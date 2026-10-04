#include "capture/multiframe/MultiframeCaptureCoordinator.h"

#include <android/log.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>

#include "capture/CaptureRequest.h"
#include "diagnostics/logging/RuntimeTraceRecorder.h"
#include "imaging/FrameLimits.h"
#include "vulkan/VulkanContext.h"
namespace rawrcam::capture::multiframe {
namespace {
constexpr const char* kTag = "RawrCamNative";
constexpr std::size_t kMultiframeCaptureFrames = 30u;
// A copied RAW slot becomes snapshot-ready only when its preview fence retires.
// Keep one slot of headroom per in-flight preview frame so shutter capture can
// obtain 30 ready frames without waiting on or starving the realtime pipeline.
constexpr std::size_t kMultiframeRingFrames = kMultiframeCaptureFrames + rawrcam::imaging::kRealtimeFramesInFlight;
// The ring is uncompressed RAW16 on the GPU. Its byte budget is the footprint
// tuned on a 12.5 MP sensor; larger sensors get fewer frames, never more memory.
constexpr std::uint64_t kMultiframeRingByteBudget =
    static_cast<std::uint64_t>(kMultiframeRingFrames) * 4096u * 3072u * sizeof(uint16_t);
std::size_t multiframeRingFrames(uint32_t width, uint32_t height) {
    const std::uint64_t frameBytes = static_cast<std::uint64_t>(width) * height * sizeof(uint16_t);
    const std::size_t minimum = 2u + rawrcam::imaging::kRealtimeFramesInFlight;
    if (frameBytes == 0) return minimum;
    return std::clamp<std::size_t>(static_cast<std::size_t>(kMultiframeRingByteBudget / frameBytes), minimum,
                                   kMultiframeRingFrames);
}

#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, kTag, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, kTag, __VA_ARGS__)
}  // namespace
MultiframeCaptureCoordinator::MultiframeCaptureCoordinator(vulkan::VulkanContext& context, std::mutex& queueMutex,
                                                           std::string filesDir, PostGain postGain)
    : vulkanContext_(context),
      postGainFor_(std::move(postGain)),
      mfsrService_(std::make_unique<MfsrCaptureService>(context, queueMutex, std::move(filesDir))) {}
MultiframeCaptureCoordinator::~MultiframeCaptureCoordinator() { shutdown(); }

void MultiframeCaptureCoordinator::recordFrame(VkCommandBuffer command, VkImage rawCopy, uint64_t timestampNs,
                                               const metadata::FrameMetadataSnapshot& metadata,
                                               const color::FrameColorTransform& color) {
    if (multiframeFrameRing_) multiframeFrameRing_->observeSubmit(command, rawCopy, timestampNs, metadata, color);
}
void MultiframeCaptureCoordinator::configure(uint32_t width, uint32_t height, uint32_t cfa) {
    config_ = {width, height, cfa};
    configured_ = true;
    resetZsl();
    initializeZslIfNeeded();
    mfsrService_->configure(config_, experimentalMultiframeEnabled_);
}
void MultiframeCaptureCoordinator::reset() noexcept {
    if (mfsrService_) mfsrService_->reset();
    resetZsl();
    configured_ = false;
    config_ = {};
}
void MultiframeCaptureCoordinator::shutdown() noexcept {
    reset();
    mfsrService_.reset();
}
void MultiframeCaptureCoordinator::setExperimentalMultiframeEnabled(bool enabled) noexcept {
    if (experimentalMultiframeEnabled_ == enabled) return;
    experimentalMultiframeEnabled_ = enabled;
    // Live multiframe owns an uncompressed RAW16 GPU ring. RZSL persistence is a
    // separate opt-in compressed path and does not keep the RAW ring alive.
    resetZsl();
    if (experimentalMultiframeEnabled_) initializeZslIfNeeded();
}

void MultiframeCaptureCoordinator::setPersistZslRingEnabled(bool enabled) noexcept {
    if (persistZslRingEnabled_ == enabled) return;
    persistZslRingEnabled_ = enabled;
    rawrcam::diagnostics::RuntimeTraceRecorder::instance().record(rawrcam::diagnostics::RuntimeTraceStage::ZslRingState,
                                                                  0, 0, -1, 0, 0, enabled ? 1u : 0u, 0);
}

void MultiframeCaptureCoordinator::initializeZslIfNeeded() noexcept {
    if (!configured_) return;
    try {
        if (experimentalMultiframeEnabled_ && !multiframeFrameRing_) {
            const std::size_t ringFrames = multiframeRingFrames(config_.rawWidth, config_.rawHeight);
            multiframeFrameRing_ = std::make_unique<rawrcam::capture::multiframe::MultiframeFrameRing>(
                vulkanContext_.physicalDevice(), vulkanContext_.device(), config_.rawWidth, config_.rawHeight,
                ringFrames);
            rawrcam::diagnostics::RuntimeTraceRecorder::instance().record(
                rawrcam::diagnostics::RuntimeTraceStage::ZslRingReady, 0, 0, -1, 0, 0,
                static_cast<uint32_t>(ringFrames), static_cast<int64_t>(multiframeFrameRing_->usedBytes()));
        }
    } catch (...) {
        multiframeFrameRing_.reset();
        rawrcam::diagnostics::RuntimeTraceRecorder::instance().record(
            rawrcam::diagnostics::RuntimeTraceStage::ZslRingRetireFail, 0, 0, -1, 0, 0, 1u, 0);
    }
}

void MultiframeCaptureCoordinator::retireFrame(uint64_t timestampNs) noexcept {
    if (multiframeFrameRing_) {
        multiframeFrameRing_->markReadyForTimestamp(timestampNs);
    }
}

uint32_t MultiframeCaptureCoordinator::prepareExperimentalMultiframeOnShutter(uint32_t maxFrames) noexcept {
    auto& trace = rawrcam::diagnostics::RuntimeTraceRecorder::instance();
    if (!experimentalMultiframeEnabled_ || !multiframeFrameRing_ || pendingMultiframeCapture_) {
        trace.record(rawrcam::diagnostics::RuntimeTraceStage::MultiframeTrigger, 0, 0, -1, 0, 0, 1u, 0);
        return 0u;
    }
    // The merge handles every 2x2 Bayer arrangement (codes 0..3); anything else
    // (e.g. MONO) has no CFA phases to merge, so say why instead of going quiet.
    if (config_.cfa > 3u) {
        trace.record(rawrcam::diagnostics::RuntimeTraceStage::MultiframeFail, 0, 0, -1, 0, 0, 5u, config_.cfa);
        LOGW("MULTIFRAME_UNAVAILABLE reason=cfa code=%u", config_.cfa);
        return 0u;
    }
    try {
        auto frozen = multiframeFrameRing_->freeze(maxFrames);
        trace.record(rawrcam::diagnostics::RuntimeTraceStage::MultiframeSnapshot, 0, 0, -1, 0, 0,
                     static_cast<uint32_t>(frozen ? frozen->refs.size() : 0),
                     static_cast<int64_t>(multiframeFrameRing_->usedBytes()));
        if (!frozen) {
            trace.record(rawrcam::diagnostics::RuntimeTraceStage::MultiframeFail, 0, 0, -1, 0, 0, 4u, 0);
            return 0u;
        }
        auto pending = std::make_unique<rawrcam::capture::multiframe::PendingMultiframeCapture>();
        pending->snapshot = std::move(frozen->snapshot);
        pending->refs = std::move(frozen->refs);
        pending->frames = std::move(frozen->frames);
        pending->metadata = std::move(frozen->metadata);
        pending->colors = std::move(frozen->colors);
        pending->referenceIndex = frozen->referenceIndex;
        pending->referenceMetadata = frozen->referenceMetadata;
        pending->referenceColor = frozen->referenceColor;
        const uint32_t frameCount = static_cast<uint32_t>(pending->frames.size());
        const std::uint64_t referenceId = pending->refs[pending->referenceIndex].frameId;
        pendingMultiframeCapture_ = std::move(pending);
        trace.record(rawrcam::diagnostics::RuntimeTraceStage::MultiframeTrigger, 0, referenceId, -1, 0, 0, 0u,
                     frameCount);
        return frameCount;
    } catch (const std::exception& e) {
        pendingMultiframeCapture_.reset();
        LOGE("MULTIFRAME_GPU_PREPARE_FAIL %s", e.what());
        return 0u;
    } catch (...) {
        pendingMultiframeCapture_.reset();
        LOGE("MULTIFRAME_GPU_PREPARE_FAIL unknown exception");
        return 0u;
    }
}

void MultiframeCaptureCoordinator::cancelPreparedMultiframeCapture() noexcept { pendingMultiframeCapture_.reset(); }

std::string MultiframeCaptureCoordinator::pollMultiframeDngCompletion() { return mfsrService_->pollDngCompletion(); }

std::string MultiframeCaptureCoordinator::pollMultiframeJpegCompletion() { return mfsrService_->pollJpegCompletion(); }

uint64_t MultiframeCaptureCoordinator::startPreparedMultiframeCapture(
    rawrcam::encoding::dng::DngCaptureContext baseDng, rawrcam::encoding::dng::DngCaptureContext mergedDng,
    rawrcam::capture::JpegCaptureRequest mergedJpeg, bool dumpRzslRequested,
    rawrcam::capture::multiframe::MultiframeTuning tuning,
    rawrcam::capture::multiframe::MultiframeBaseFrameMode baseFrameMode, const tonemap::TonemapParams& tone,
    bool filmEnabled, const spektrafilm_native::FilmLook& filmLook) noexcept {
    const bool jpegRequested = mergedJpeg.output.outputFd >= 0;
    const int baseFd = baseDng.outputFd;
    const int mergedFd = mergedDng.outputFd;
    const int jpegFd = mergedJpeg.output.outputFd;
    auto closeOutputs = [&]() noexcept {
        if (baseFd >= 0) close(baseFd);
        if (mergedFd >= 0) close(mergedFd);
        if (jpegFd >= 0) close(jpegFd);
    };
    if (!pendingMultiframeCapture_ || (baseFd < 0 && mergedFd < 0 && jpegFd < 0) ||
        (jpegRequested && (mergedJpeg.output.quality < 95 || mergedJpeg.output.quality > 100))) {
        closeOutputs();
        pendingMultiframeCapture_.reset();
        return 0u;
    }

    auto frozenTonemap = baseDng.captureTone ? *baseDng.captureTone : tone;
    frozenTonemap.aePostGain = postGainFor_(pendingMultiframeCapture_->referenceMetadata);
    const bool captureFilmEnabled = baseDng.captureFilm ? baseDng.captureFilmEnabled : filmEnabled;
    const auto captureFilm = baseDng.captureFilm ? *baseDng.captureFilm : filmLook;
    auto capture = std::move(pendingMultiframeCapture_);
    const uint64_t requestId =
        mfsrService_->start(std::move(capture), std::move(baseDng), std::move(mergedDng), std::move(mergedJpeg),
                            dumpRzslRequested, frozenTonemap, tuning, baseFrameMode, captureFilmEnabled, captureFilm);
    if (requestId == 0u) {
        // Service closed the output FDs on rejection; nothing pending remains.
        return 0u;
    }
    return requestId;
}

void MultiframeCaptureCoordinator::resetZsl() noexcept {
    cancelPreparedMultiframeCapture();
    // Ring destruction waits its GPU copy fences before codec source buffers disappear.
    multiframeFrameRing_.reset();
}

void MultiframeCaptureCoordinator::commitFrame(uint64_t timestampNs) noexcept {
    if (multiframeFrameRing_) multiframeFrameRing_->commitTimestamp(timestampNs);
}
void MultiframeCaptureCoordinator::discardFrame(uint64_t timestampNs) noexcept {
    if (multiframeFrameRing_) multiframeFrameRing_->discardTimestamp(timestampNs);
}
}  // namespace rawrcam::capture::multiframe
