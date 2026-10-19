#include "diagnostics/timing/GpuTimingTracker.h"
// Composition root: construction, core lifecycle, metadata ingress, shutdown.
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "camera/NativeCameraController.h"
#include "capture/persistence/AcquiredRawJournal.h"
#include "color/ColorCalibration.h"
#include "color/WhiteBalance.h"
#include "diagnostics/logging/NativeLog.h"
#include "diagnostics/logging/RuntimeTraceRecorder.h"
#include "geometry/CfaPattern.h"
#include "imaging/Raw16CpuSnapshot.h"
#include "imaging/RawFrameLease.h"
#include "metadata/MetadataDiagnostics.h"
#include "session/SessionEngine.h"
#include "video_pipeline/VideoCrop.h"
#include "vulkan/VulkanDispatch.h"

namespace rawrcam::session {
namespace {
int64_t steadyMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
std::string jsonEscape(const std::string& value) {
    std::string out;
    for (char ch : value) {
        if (ch == '"' || ch == '\\') out.push_back('\\');
        if (static_cast<unsigned char>(ch) >= 0x20) out.push_back(ch);
    }
    return out;
}
}  // namespace

SessionEngine::SessionEngine(std::string filesDir) : filesDir_(std::move(filesDir)) {
    diagnosticSink_ = std::make_unique<rawrcam::diagnostics::DiagnosticSink>(filesDir_);
    rawrcam::diagnostics::RuntimeTraceRecorder::instance().configureOutputPath(filesDir_ +
                                                                               "/rawrcam_internal_runtime_trace.txt");
    diagnosticSink_->initialize();
    frameAuditWriter_ =
        std::make_unique<rawrcam::diagnostics::FrameAuditWriter>(filesDir_ + "/rawrcam_frame_audit.jsonl");
    realtime_.coordinator().setTonemapParams(look_.toneParams());
    realtime_.coordinator().setColorMode(colorMode_);
    realtime_.coordinator().setVideoOutput(&videoSession_);
    rawrcam::camera::PreviewCallbacks callbacks{};
    callbacks.configurePreview = [this](uint64_t generation, const rawrcam::metadata::CameraContextMetadata& metadata,
                                        const rawrcam::camera::CameraControlCapabilities& capabilities) {
        LOGI("CONFIGURE_PREVIEW_ENTER generation=%llu", static_cast<unsigned long long>(generation));
        std::lock_guard<std::mutex> lock(mu_);
        LOGI("CONFIGURE_PREVIEW_LOCKED generation=%llu", static_cast<unsigned long long>(generation));
        if (generation < cameraSetupGeneration_) return false;
        cameraSetupGeneration_ = generation;
        const bool configured = realtime_.configureCamera(generation, metadata);
        lastCameraContext_ = std::make_shared<const rawrcam::metadata::CameraContextMetadata>(metadata);
        profileReaderUsage_ = metadata.rawReaderUsage;
        // The RAW size may have changed (or become known): rebuild the prewarm.
        if (configured) {
            cameraCapabilities_ = capabilities;
            appendDiagnostic(rawrcam::metadata::describe(metadata));
            if (frameAuditWriter_) frameAuditWriter_->recordCameraContext(metadata);
            markVideoPrewarmDirty();
        }
        return configured;
    };
    callbacks.createRawWindow = [this](uint64_t generation, uint32_t width, uint32_t height,
                                       rawrcam::geometry::RawPixelFormat format) {
        std::lock_guard<std::mutex> lock(mu_);
        if (generation != cameraSetupGeneration_) return static_cast<ANativeWindow*>(nullptr);
        auto* window = createRawInputWindow(generation, width, height, format);
        if (window) {
            cameraReaderGeneration_ = generation;
            constexpr uint64_t kGpuUsage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                                           AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER |
                                           AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT;
            rawReaderUsage_.readerCreated(generation, rawFrameIngress_.requestedUsage() & kGpuUsage,
                                          steadyMillis());
        }
        return window;
    };
    callbacks.destroyRawWindow = [this](uint64_t generation) {
        std::lock_guard<std::mutex> lock(mu_);
        if (generation != cameraReaderGeneration_) return;
        rawReaderUsage_.readerDestroyed(generation);
        destroyRawInput();
        cameraReaderGeneration_ = 0;
    };
    callbacks.createCaptureWindow = [this](uint64_t generation, uint32_t width, uint32_t height,
                                           rawrcam::geometry::RawPixelFormat format) {
        std::lock_guard<std::mutex> lock(mu_);
        if (generation != cameraSetupGeneration_) return static_cast<ANativeWindow*>(nullptr);
        try {
            // vivo full-size outputs configure only with no usage or GPU_DATA_BUFFER.
            auto* window = captureFrameIngress_.create(generation, width, height, format,
                                                       AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER);
            captureReaderGeneration_ = generation;
            fullSizeFrames_.store(0, std::memory_order_relaxed);
            // Full size is 2x (50 MP) or 4x (200 MP) the binned pixel array; the buffer may be wider
            // (8192 vs 8160 on vivo).
            if (lastCameraContext_) {
                auto context = std::make_shared<rawrcam::metadata::CameraContextMetadata>(*lastCameraContext_);
                auto& g = context->geometry;
                const uint32_t scale = g.pixelArrayWidth ? std::max(1u, width / g.pixelArrayWidth) : 1u;
                g.rawBufferWidth = std::min(width, scale * g.pixelArrayWidth);
                g.rawBufferHeight = std::min(height, scale * g.pixelArrayHeight);
                capture_.single().configureRawSnapshot(g.rawBufferWidth, g.rawBufferHeight, generation);
                capture_.multiframe().configureFullSize(g.rawBufferWidth, g.rawBufferHeight, context->rawPreviewCfa);
                appendDiagnostic("FULL_SIZE_STILL_CONFIG raw=" + std::to_string(g.rawBufferWidth) + "x" +
                                 std::to_string(g.rawBufferHeight) + " buffer=" + std::to_string(width) + "x" +
                                 std::to_string(height));
                std::lock_guard<std::mutex> stillLock(fullSize_.mutex);
                fullSize_.generation = generation;
                fullSize_.failed = false;
                fullSize_.failures.clear();
                fullSize_.context = std::move(context);
            }
            return window;
        } catch (const std::exception& e) {
            appendDiagnostic(std::string("FULL_SIZE_READER_FAILED error=") + e.what());
            return static_cast<ANativeWindow*>(nullptr);
        }
    };
    callbacks.destroyCaptureWindow = [this](uint64_t generation) {
        std::lock_guard<std::mutex> lock(mu_);
        if (generation != captureReaderGeneration_) return;
        {
            std::lock_guard<std::mutex> stillLock(fullSize_.mutex);
            fullSize_.frames.clear();
            fullSize_.context.reset();
            fullSize_.requestIds.clear();
            fullSize_.claimedTimestamps.clear();
            fullSize_.burstTags.clear();
            fullSize_.burstResults.clear();
            fullSize_.results.clear();
            fullSize_.generation = 0;
            fullSize_.failed = true;
        }
        fullSize_.arrived.notify_all();
        captureFrameIngress_.destroy();
        captureReaderGeneration_ = 0;
    };
    callbacks.captureFailed = [this](uint64_t generation) {
        std::lock_guard<std::mutex> lock(mu_);
        rawReaderUsage_.captureFailed(generation);
    };
    callbacks.acquisitionFailed = [this](uint64_t generation, uint64_t tag) {
        // Camera callbacks never acquire the session lock or run teardown.
        if (!tag) failedCameraGeneration_.store(generation);
        {
            std::lock_guard<std::mutex> lock(fullSize_.mutex);
            if (generation != fullSize_.generation) return;
            if (tag)
                fullSize_.failures.insert(tag);
            else {
                fullSize_.failed = true;
                failedCameraGeneration_.store(generation);
            }
        }
        fullSize_.arrived.notify_all();
    };
    callbacks.submitMetadata = [this](const rawrcam::metadata::FrameMetadataSnapshot& metadata) {
        return submitMetadata(metadata);
    };
    callbacks.diagnostic = [this](const std::string& line) { recordDiagnosticLine(line); };
    auto controller = std::make_unique<rawrcam::camera::NativeCameraController>(std::move(callbacks));
    controller->setSensitivityCalibrationStore(filesDir_ + "/rawrcam_iso_calibration.txt");
    cameraControls_.attach(std::move(controller));
    look_.start();
    videoSession_.startPrewarm();
    ingressQueue_.start();
}

SessionEngine::~SessionEngine() { shutdown(); }

bool SessionEngine::setSurface(JNIEnv* env, jobject surfaceObj, int displayRotationDegrees) {
    // Retire before detaching presentation, unless a still owns the session.
    // Activation callbacks acquire mu_, so reconcile outside that lock.
    if (!surfaceObj) cameraControls_.setSurfaceReady(false);
    bool ready = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!surfaceObj) {
            videoSession_.stop();
            videoSession_.releaseProcessing();
            swapchainRenderer_.setVideoFrameSources({});
            // Backgrounding may end the process without a device teardown.
            rawrcam::vulkan::dispatch::savePipelineCache(vulkanContext_.device());
        }
        ready = setPresentationSurface(env, surfaceObj, displayRotationDegrees);
    }
    if (surfaceObj) cameraControls_.setSurfaceReady(ready);
    return ready;
}

bool SessionEngine::setVideoSurface(JNIEnv* env, jobject surfaceObj, uint32_t width, uint32_t height,
                                    uint32_t bitDepth) {
    // Reuse a prewarm that is still building instead of building a second copy.
    if (surfaceObj) waitForVideoPrewarmIdle();
    std::lock_guard<std::mutex> lock(mu_);
    const auto restoreFilm = [this] { look_.suppressForVideo(false); };
    try {
        if (!surfaceObj) {
            videoSession_.stop();
            swapchainRenderer_.setVideoFrameSources({});
            restoreFilm();
            // Re-check the cached processing against the current settings.
            markVideoPrewarmDirty();
            return true;
        }
        if (!vulkanContext_.device()) return false;
        look_.suppressForVideo(true);
        realtime_.coordinator().resetVideoCounters();
        videoIngressDrops_.store(0, std::memory_order_relaxed);
        realtime_.timing().resetVideoTiming();
        const auto renderLut = look_.videoRenderLut();
        if (!videoSession_.start(env, surfaceObj, width, height, realtime_.rawWidth(), realtime_.rawHeight(), bitDepth,
                                 renderLut ? &*renderLut : nullptr)) {
            restoreFilm();
            return false;
        }
        std::array<VkImageView, rawrcam::imaging::kRealtimeFramesInFlight> monitorViews{};
        for (uint32_t i = 0; i < monitorViews.size(); ++i) monitorViews[i] = videoSession_.monitorView(i);
        swapchainRenderer_.setVideoFrameSources(monitorViews);
        return true;
    } catch (const std::exception& error) {
        videoSession_.stop();
        swapchainRenderer_.setVideoFrameSources({});
        restoreFilm();
        LOGE("VIDEO_SURFACE_FAIL %s", error.what());
        appendDiagnostic(std::string("VIDEO_SURFACE_FAIL ") + error.what());
        return false;
    }
}

void SessionEngine::setVideoPreviewCropOut(uint32_t width, uint32_t height) {
    std::lock_guard<std::mutex> lock(mu_);
    videoCropOutWidth_ = width;
    videoCropOutHeight_ = height;
    realtime_.coordinator().setVideoPreviewCropOut(width, height);
    swapchainRenderer_.resetLogging();
}

rawrcam::geometry::SourceCropRect SessionEngine::previewVideoCropRectLocked() const {
    rawrcam::geometry::SourceCropRect crop{};
    if (videoCropOutWidth_ == 0 || videoCropOutHeight_ == 0 || !realtime_.configured()) {
        return crop;
    }
    const uint32_t rawW = realtime_.rawWidth();
    const uint32_t rawH = realtime_.rawHeight();
    const uint32_t previewW = realtime_.previewWidth();
    const uint32_t previewH = realtime_.previewHeight();
    if (rawW < 2 || rawH < 2 || previewW == 0 || previewH == 0) return crop;
    const auto full = rawrcam::video::videoSourceRect(rawW, rawH, videoCropOutWidth_, videoCropOutHeight_);
    if (full.width == 0) return crop;
    const auto scaled = rawrcam::video::scaleVideoSourceRect(full, rawW, rawH, previewW, previewH);
    if (scaled.width == 0) return crop;
    crop = {scaled.x, scaled.y, scaled.width, scaled.height, true};
    return crop;
}

std::string SessionEngine::videoStats() {
    std::lock_guard<std::mutex> lock(mu_);
    const auto timing = realtime_.timing().snapshot();
    const auto& processing = videoSession_.activeProcessingConfig();
    const auto& reasons = realtime_.coordinator().videoDropReasons();
    const uint64_t ingressDrops = videoIngressDrops_.load(std::memory_order_relaxed);
    // Every camera frame that never reached the encoder, whatever the stage.
    const uint64_t dropped = ingressDrops + reasons.noSlot + reasons.encoderBusy + reasons.presentFail +
                             reasons.pairer + reasons.stale + reasons.submitFail + reasons.geometry;
    return std::string("{\"submitted\":") + std::to_string(realtime_.coordinator().videoSubmitted()) +
           ",\"dropped\":" + std::to_string(dropped) +
           ",\"ptsSource\":\"" + (videoSession_.stampsPresentTime() ? "sensor" : "queue") + "\"" +
           ",\"previewDropped\":" + std::to_string(timing.dropped) +
           ",\"previewSkippedDuringVideo\":" + std::to_string(realtime_.coordinator().previewSkippedDuringVideo()) +
           ",\"gpuMs\":" + std::to_string(timing.avgTotalGpuMs) +
           ",\"videoProcessMs\":" + std::to_string(timing.avgVideoProcessMs) +
           ",\"videoDemosaicMs\":" + std::to_string(timing.avgVideoDemosaicMs) +
           ",\"videoPostMs\":" + std::to_string(timing.avgVideoPostMs) +
           ",\"videoRenderMs\":" + std::to_string(timing.avgVideoRenderMs) +
           ",\"rawMs\":" + std::to_string(timing.avgRawMs) + ",\"tonemapMs\":" + std::to_string(timing.avgTonemapMs) +
           ",\"highlightMethod\":" + std::to_string(processing.highlightMethod) +
           ",\"fccSteps\":" + std::to_string(processing.fccSteps) +
           ",\"defringeStrength\":" + std::to_string(processing.defringeStrength) +
           ",\"waveletDenoiseStrength\":" + std::to_string(processing.waveletDenoiseStrength) +
           ",\"processingRestartRequired\":" + (videoSession_.processingRestartRequired() ? "true" : "false") +
           ",\"encoderSurfaceFormat\":\"" + videoSession_.outputFormatName() + "\"" +
           ",\"rawStage\":\"" + videoSession_.rawStageMethod() + "\"" + ",\"encoderSurfaceOffered\":\"" +
           videoSession_.offeredFormats() + "\"" +
           ",\"encoderImages\":" + std::to_string(videoSession_.encoderImageCount()) +
           ",\"dropReasons\":{\"ingress\":" + std::to_string(ingressDrops) +
           ",\"noSlot\":" + std::to_string(reasons.noSlot) + ",\"encoderBusy\":" + std::to_string(reasons.encoderBusy) +
           ",\"presentFail\":" + std::to_string(reasons.presentFail) + ",\"pairer\":" + std::to_string(reasons.pairer) +
           ",\"stale\":" + std::to_string(reasons.stale) + ",\"submitFail\":" + std::to_string(reasons.submitFail) +
           ",\"geometry\":" + std::to_string(reasons.geometry) + "}" +
           ",\"stageMs\":" + videoSession_.stageTimingJson() + ",\"startMs\":" + videoSession_.startTimingJson() +
           ",\"prewarm\":\"" + jsonEscape(videoSession_.prewarmStatus()) + "\"" + ",\"submitFailure\":\"" +
           jsonEscape(realtime_.coordinator().lastVideoSubmitFailure()) + "\"" +
           ",\"active\":" + (videoSession_.ready() ? "true" : "false") + "}";
}

void SessionEngine::setVideoImageSettings(const rawrcam::video::VideoProcessingConfig& config) {
    std::lock_guard<std::mutex> lock(mu_);
    videoSession_.setProcessingConfig(config);
    // The same user settings select and tune preview highlight rendering, so
    // the viewfinder shows the method stills and video will use.
    realtime_.coordinator().setHighlightMethod(config.highlightMethod, config.highlightThreshold,
                                               config.highlightCompression);
}

void SessionEngine::setScopeDeviceRotationDegrees(int rotation) {
    std::lock_guard<std::mutex> lock(mu_);
    realtime_.setScopeDeviceRotationDegrees(rotation);
}

bool SessionEngine::submitMetadata(const metadata::FrameMetadataSnapshot& metadata) {
    cameraControls_.noteFrameResult();
    if (metadata.optimizedStillRequestId && metadata.cameraContext) {
        std::lock_guard<std::mutex> lock(fullSize_.mutex);
        const auto tag = *metadata.optimizedStillRequestId;
        if (fullSize_.context && metadata.cameraContext->cameraContextGeneration == fullSize_.generation &&
            (fullSize_.requestIds.count(tag) || fullSize_.burstTags.count(tag))) {
            fullSize_.results.insert_or_assign(metadata.timestampNs, metadata);
            return true;
        }
    }
    return ingressQueue_.enqueue(metadata);
}
void SessionEngine::enqueueRawFrame(imaging::AcquiredRawFrame frame) { ingressQueue_.enqueue(std::move(frame)); }
void SessionEngine::stopIngress() { ingressQueue_.stop(); }
void SessionEngine::tickRawReaderUsage() {
    const auto failed = failedCameraGeneration_.exchange(0);
    if (failed) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (failed != cameraReaderGeneration_) return;
            capture_.single().failAcquisition(0, "camera_device_failed");
        }
        cameraControls_.requestSessionRestart();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!rawReaderUsage_.advance(steadyMillis())) return;
        std::ostringstream out;
        out << "RAW_READER_USAGE_DEMOTED to=cpu_read_only noRawFrameWithinMs="
            << imaging::RawReaderUsageLadder::kFirstFrameDeadlineMs << " requestedUsage=0x" << std::hex
            << rawFrameIngress_.requestedUsage();
        const std::string line = out.str();
        LOGE("%s", line.c_str());
        appendDiagnostic(line);
    }
    // Outside mu_: the restart reconfigures through callbacks that take it.
    cameraControls_.requestSessionRestart();
}
void SessionEngine::consumeIngress(imaging::FrameIngressQueue::Event event, imaging::FrameIngressQueue::Drops drops) {
    const bool raw = std::holds_alternative<imaging::RawFrameLease>(event.payload);
    if (!std::holds_alternative<std::monostate>(event.payload)) {
        const auto delay =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - event.enqueuedAt)
                .count();
        if (delay > 50 && (++ingressSlowReports_ % 120u) == 1u)
            LOGI("INGRESS_QUEUE_SLOW delayMs=%lld kind=%s", static_cast<long long>(delay), raw ? "raw" : "metadata");
    }
    // Outside mu_: the controller takes its own lock and must never nest under it.
    if (raw) cameraControls_.rawFrameArrived(std::get<imaging::RawFrameLease>(event.payload).get().generation);
    std::lock_guard<std::mutex> lock(mu_);
    if (raw) rawReaderUsage_.rawArrived(std::get<imaging::RawFrameLease>(event.payload).get().generation);
    drainFullSizeStills();
    if (drops.images) realtime_.timing().recordDropped(drops.images);
    if (drops.images && videoSession_.ready() && realtime_.coordinator().videoSubmitted() > 0)
        videoIngressDrops_.fetch_add(drops.images, std::memory_order_relaxed);
    if (drops.images || drops.metadata)
        LOGI("INGRESS_QUEUE_PRESSURE rawDropped=%llu metadataDropped=%llu",
             static_cast<unsigned long long>(drops.images), static_cast<unsigned long long>(drops.metadata));
    if (raw)
        realtime_.coordinator().onRawFrame(std::move(std::get<imaging::RawFrameLease>(event.payload)));
    else if (const auto* metadata = std::get_if<metadata::FrameMetadataSnapshot>(&event.payload))
        (void)realtime_.coordinator().submitMetadata(*metadata);
    else {
        realtime_.coordinator().releaseCompletedSlots(false);
        capture_.single().advance();
    }
}

void SessionEngine::shutdown() {
    videoSession_.stopPrewarm();
    look_.stop();
    cameraControls_.shutdown();
    stopIngress();
    std::lock_guard<std::mutex> lock(mu_);
    videoSession_.stop();
    videoSession_.releaseProcessing();
    {
        capture_.single().shutdown();
        if (vulkanContext_.device()) vulkanContext_.waitIdle();
        captureFrameIngress_.destroy();
        captureReaderGeneration_ = 0;
        destroyRawInputInternal();
        realtime_.shutdown();
        swapchainRenderer_.destroySwapchain();
        presentationSurface_.reset();
        vulkanContext_.shutdown();
    }
}

void SessionEngine::requestVideoPrewarm(uint32_t width, uint32_t height, uint32_t bitDepth) {
    videoSession_.requestVideoPrewarm(width, height, bitDepth);
}
void SessionEngine::releaseVideoProcessing() { videoSession_.releaseVideoProcessing(); }
void SessionEngine::markVideoPrewarmDirty() { videoSession_.markVideoPrewarmDirty(); }
void SessionEngine::waitForVideoPrewarmIdle() { videoSession_.waitForVideoPrewarmIdle(); }
}  // namespace rawrcam::session

namespace rawrcam::session {
void SessionEngine::onFullSizeFrame(imaging::AcquiredRawFrame frame) {
    imaging::RawFrameLease lease(frame);
    const auto n = fullSizeFrames_.fetch_add(1, std::memory_order_relaxed) + 1;
    appendDiagnostic("FULL_SIZE_FRAME n=" + std::to_string(n) + " timestampNs=" + std::to_string(frame.timestampNs) +
                     " generation=" + std::to_string(frame.generation));
    {
        std::lock_guard<std::mutex> lock(fullSize_.mutex);
        if (frame.generation != fullSize_.generation || fullSize_.failed) return;
        fullSize_.frames.push_back(std::move(lease));
        // The reader holds few images: never keep more than it can lend.
        while (fullSize_.frames.size() > FullSizeStills::kMaxFrames) fullSize_.frames.pop_front();
    }
    fullSize_.arrived.notify_all();
}
void SessionEngine::drainFullSizeStills() {
    std::lock_guard<std::mutex> lock(fullSize_.mutex);
    if (!fullSize_.context) return;
    if (fullSize_.failed) {
        capture_.single().failAcquisition(0, "camera_device_failed");
        fullSize_.requestIds.clear();
        fullSize_.frames.clear();
        fullSize_.results.clear();
        return;
    }
    for (const auto tag : fullSize_.failures) {
        if (fullSize_.requestIds.erase(tag)) capture_.single().failAcquisition(tag, "camera_capture_failed");
    }
    for (auto it = fullSize_.results.begin(); it != fullSize_.results.end();) {
        auto full = it->second;
        const auto tag = full.optimizedStillRequestId.value_or(0);
        if (fullSize_.failures.count(tag)) {
            it = fullSize_.results.erase(it);
            continue;
        }
        full.cameraContext = fullSize_.context;
        const auto color = color::deriveFrameColorTransform(full, color::parsePreviewColorMode(colorMode_));
        if (fullSize_.burstTags.count(tag)) {
            fullSize_.burstResults.insert_or_assign(it->first, std::make_pair(std::move(full), color));
            it = fullSize_.results.erase(it);
            fullSize_.arrived.notify_all();
            continue;
        }
        if (!fullSize_.requestIds.count(tag)) {
            it = fullSize_.results.erase(it);
            continue;
        }
        const auto frame = std::find_if(fullSize_.frames.begin(), fullSize_.frames.end(),
                                        [&](const auto& f) { return f.get().timestampNs == it->first; });
        if (frame == fullSize_.frames.end()) {
            ++it;
            continue;
        }
        auto tone = look_.toneParams();
        const float gain = pipelineCallbacks_.postGainFor(full);
        applyAutoKey(tone, liftNoiseFor(full, gain * std::exp2(tone.exposureEV), 1u));
        if (!capture_.single().deferSubmittedFrame(full, color, tone, gain, look_.requestedFilmEnabled(),
                                                   look_.filmLook())) {
            ++it;  // asynchronous shot preparation may not have finished yet
            continue;
        }
        if (capture_.single().beginDeferredSnapshot(frame->image(), frame->ahb(), it->first, frame->fence())) {
            (void)frame->takeImage().release();
            appendDiagnostic("FULL_SIZE_DIRECT_SNAPSHOT tag=" + std::to_string(tag) +
                             " timestampNs=" + std::to_string(it->first));
        } else {
            capture_.single().failAcquisition(tag, "full_size_snapshot_failed");
        }
        fullSize_.frames.erase(frame);
        fullSize_.requestIds.erase(tag);
        it = fullSize_.results.erase(it);
    }
}
rawrcam::metadata::CameraContextMetadataPtr SessionEngine::claimFullSizeStill(
    const rawrcam::metadata::FrameMetadataSnapshot& metadata) {
    std::lock_guard<std::mutex> lock(fullSize_.mutex);
    if (!fullSize_.context || !metadata.optimizedStillRequestId ||
        !fullSize_.requestIds.count(*metadata.optimizedStillRequestId))
        return nullptr;
    fullSize_.requestIds.erase(*metadata.optimizedStillRequestId);
    fullSize_.claimedTimestamps.insert(metadata.timestampNs);
    return fullSize_.context;
}
std::pair<bool, bool> SessionEngine::fullSizeCaptureState() {
    std::lock_guard<std::mutex> lock(fullSize_.mutex);
    return {fullSize_.context != nullptr, !fullSize_.requestIds.empty() || !fullSize_.burstTags.empty()};
}
bool SessionEngine::fullSizeStillPending() {
    std::lock_guard<std::mutex> lock(fullSize_.mutex);
    return !fullSize_.requestIds.empty() || !fullSize_.burstTags.empty();
}
bool SessionEngine::fullSizeStillsOnly() {
    std::lock_guard<std::mutex> lock(fullSize_.mutex);
    return fullSize_.context != nullptr;
}
bool SessionEngine::isFullSizeStillCopy(const rawrcam::metadata::FrameMetadataSnapshot& metadata) {
    std::lock_guard<std::mutex> lock(fullSize_.mutex);
    return fullSize_.context && metadata.optimizedStillRequestId &&
           (fullSize_.requestIds.count(*metadata.optimizedStillRequestId) ||
            fullSize_.burstTags.count(*metadata.optimizedStillRequestId));
}
void SessionEngine::observeFullSizeBurstFrame(const rawrcam::metadata::FrameMetadataSnapshot& metadata,
                                              const rawrcam::color::FrameColorTransform& color) {
    {
        std::lock_guard<std::mutex> lock(fullSize_.mutex);
        if (!fullSize_.context || !metadata.optimizedStillRequestId ||
            !fullSize_.burstTags.count(*metadata.optimizedStillRequestId))
            return;
        auto full = metadata;
        full.cameraContext = fullSize_.context;
        fullSize_.burstResults.insert_or_assign(metadata.timestampNs, std::make_pair(std::move(full), color));
    }
    fullSize_.arrived.notify_all();
}
uint32_t SessionEngine::prepareFullSizeBurst(uint32_t maxFrames) {
    auto* controller = cameraControls_.controller();
    if (!controller) return 0u;
    const uint32_t count = std::clamp<uint32_t>(maxFrames, 2u, FullSizeStills::kMaxFrames);
    rawrcam::metadata::CameraContextMetadataPtr context;
    uint64_t tag = 0;
    {
        std::lock_guard<std::mutex> lock(fullSize_.mutex);
        context = fullSize_.context;
        if (!context) return 0u;
        tag = nextFullSizeBurstTag_++;
        fullSize_.burstTags.insert(tag);
    }
    const auto forget = [&] {
        std::lock_guard<std::mutex> lock(fullSize_.mutex);
        fullSize_.burstTags.erase(tag);
        for (auto it = fullSize_.burstResults.begin(); it != fullSize_.burstResults.end();)
            it = it->second.first.optimizedStillRequestId == tag ? fullSize_.burstResults.erase(it) : std::next(it);
    };
    struct Cleanup {
        std::function<void()> release;
        ~Cleanup() { release(); }
    } cleanup{[&] {
        forget();
        controller->releaseFullSizeFocus();
    }};
    const uint32_t width = context->geometry.rawBufferWidth, height = context->geometry.rawBufferHeight;
    const size_t bytes = size_t(width) * height * sizeof(uint16_t);
    auto journal = std::make_shared<capture::persistence::AcquiredRawJournal>(filesDir_, bytes, count);
    std::vector<uint8_t> packed(bytes);
    struct Frame {
        imaging::RawFrameLease lease{imaging::AcquiredRawFrame{}};
        rawrcam::metadata::FrameMetadataSnapshot metadata;
        rawrcam::color::FrameColorTransform color;
    };
    std::vector<Frame> frames;
    // Submit all exposures before copying or writing any frame. The camera can
    // schedule the burst consecutively while we drain its completed buffers.
    if (!controller->captureFullSize(tag, count, true))
        return UINT32_MAX;  // acquisition failed; never submit a replacement capture
    const auto exposure = controller->deferredCaptureExposure();
    const auto budgetMs = count * (8000 + (exposure ? exposure->exposureTimeNs / 1'000'000 : 0));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
    for (uint32_t index = 0; index < count; ++index) {
        std::unique_lock<std::mutex> lock(fullSize_.mutex);
        const auto ready = [&] {
            uint32_t n = 0;
            for (const auto& [ts, result] : fullSize_.burstResults) {
                if (result.first.optimizedStillRequestId != tag) continue;
                if (std::any_of(fullSize_.frames.begin(), fullSize_.frames.end(),
                                [&, ts = ts](const auto& f) { return f.get().timestampNs == ts; }))
                    ++n;
            }
            return n;
        };
        const bool received = fullSize_.arrived.wait_until(lock, deadline, [&] {
            return ready() >= 1 || fullSize_.failed || fullSize_.failures.count(tag) || fullSize_.context != context;
        });
        if (!received || fullSize_.failed || fullSize_.failures.count(tag) || fullSize_.context != context) {
            lock.unlock();
            forget();
            failedCameraGeneration_.store(context->cameraContextGeneration);
            return UINT32_MAX;
        }
        for (auto it = fullSize_.burstResults.begin(); it != fullSize_.burstResults.end();) {
            if (it->second.first.optimizedStillRequestId != tag) {
                ++it;
                continue;
            }
            const auto frame = std::find_if(fullSize_.frames.begin(), fullSize_.frames.end(),
                                            [&](const auto& f) { return f.get().timestampNs == it->first; });
            if (frame != fullSize_.frames.end()) {
                frames.push_back({std::move(*frame), std::move(it->second.first), it->second.second});
                fullSize_.frames.erase(frame);
                fullSize_.burstResults.erase(it);
                break;
            }
            ++it;  // retain metadata whose RAW buffer has not arrived yet
        }
        lock.unlock();
        if (frames.size() != index + 1) return UINT32_MAX;
        auto& lease = frames.back().lease;
        const auto copy =
            imaging::copyCompletedRaw16AhbToPacked(lease.ahb(), width, height, packed, lease.fence(), lease.image());
        lease = imaging::RawFrameLease(imaging::AcquiredRawFrame{});
        if (!copy.success) throw std::runtime_error("full_size_copy_failed:" + copy.error);
        journal->append(packed);
    }
    // Completion belongs to the whole burst, never to each individual frame.
    const auto remainingMs = std::max<int64_t>(
        0, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
    if (!controller->waitFullSizeIdle(remainingMs)) {
        failedCameraGeneration_.store(context->cameraContextGeneration);
        return UINT32_MAX;
    }
    forget();
    appendDiagnostic("FULL_SIZE_BURST_FRAMES tag=" + std::to_string(tag) + " requested=" + std::to_string(count) +
                     " received=" + std::to_string(frames.size()));
    if (frames.size() != count) return UINT32_MAX;
    // Camera buffers are released as they are copied. Reuse one CPU frame buffer
    // and keep the journal in place for the existing strip merge.
    std::vector<uint8_t>().swap(packed);
    auto pending = std::make_unique<rawrcam::capture::multiframe::PendingMultiframeCapture>();
    for (size_t i = 0; i < frames.size(); ++i) {
        auto& f = frames[i];
        rawr::raw_gpu_pipeline::BurstFrame frame{};
        frame.raw.ref = {i + 1, f.metadata.timestampNs, width, height};
        const std::uint32_t cfa = context->rawPreviewCfa;
        frame.parameters.normalization.blackByPhase =
            rawrcam::geometry::reorderRggbByCode(f.metadata.blackLevelPhysicalRggb, cfa);
        frame.parameters.normalization.whiteLevel = f.metadata.effectiveWhiteLevel;
        frame.parameters.whiteBalance = rawrcam::color::collapseRggbToRgb(f.color.baselineWbRggb);
        pending->refs.push_back(frame.raw.ref);
        pending->frames.push_back(frame);
        pending->metadata.push_back(f.metadata);
        pending->colors.push_back(f.color);
    }
    pending->acquiredJournal = std::move(journal);
    pending->referenceIndex = static_cast<std::uint32_t>(pending->frames.size() / 2u);
    pending->referenceMetadata = pending->metadata[pending->referenceIndex];
    pending->referenceColor = pending->colors[pending->referenceIndex];
    const auto prepared = static_cast<uint32_t>(pending->frames.size());
    std::lock_guard<std::mutex> lock(mu_);
    bracketSkippedForPending_ = true;  // no binned long frames for a full-size burst
    return capture_.multiframe().adoptPreparedCapture(std::move(pending)) ? prepared : UINT32_MAX;
}
bool SessionEngine::fullSizeStillClaimed(uint64_t timestampNs) {
    std::lock_guard<std::mutex> lock(fullSize_.mutex);
    return fullSize_.claimedTimestamps.count(timestampNs) != 0;
}
bool SessionEngine::snapshotFullSizeStill(AImage* binned, uint64_t timestampNs) {
    // Both copies come from one capture: the full-size frame is at most a
    // moment behind the binned one.
    constexpr auto kWait = std::chrono::milliseconds(1500);
    imaging::RawFrameLease full{imaging::AcquiredRawFrame{}};
    {
        std::unique_lock<std::mutex> lock(fullSize_.mutex);
        const auto find = [&] {
            return std::find_if(fullSize_.frames.begin(), fullSize_.frames.end(),
                                [&](const auto& f) { return f.get().timestampNs == timestampNs; });
        };
        fullSize_.arrived.wait_for(lock, kWait, [&] { return find() != fullSize_.frames.end(); });
        fullSize_.claimedTimestamps.erase(timestampNs);
        const auto it = find();
        if (it == fullSize_.frames.end()) {
            appendDiagnostic("FULL_SIZE_STILL_MISSING timestampNs=" + std::to_string(timestampNs));
            return false;
        }
        full = std::move(*it);
        fullSize_.frames.erase(it);
    }
    if (!capture_.single().beginDeferredSnapshot(full.image(), full.ahb(), timestampNs, full.fence())) return false;
    (void)full.takeImage().release();  // the capture worker owns the full-size image now
    AImage_delete(binned);             // and the caller hands the binned copy over
    appendDiagnostic("FULL_SIZE_STILL_SNAPSHOT timestampNs=" + std::to_string(timestampNs));
    return true;
}
}  // namespace rawrcam::session
