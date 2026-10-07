#include "diagnostics/timing/GpuTimingTracker.h"
// Composition root: construction, core lifecycle, metadata ingress, shutdown.
#include <stdexcept>
#include <utility>

#include "camera/NativeCameraController.h"
#include "diagnostics/logging/NativeLog.h"
#include "diagnostics/logging/RuntimeTraceRecorder.h"
#include "metadata/MetadataDiagnostics.h"
#include "session/SessionEngine.h"
#include "video_pipeline/VideoCrop.h"

namespace rawrcam::session {
namespace {
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
        if (window) cameraReaderGeneration_ = generation;
        return window;
    };
    callbacks.destroyRawWindow = [this](uint64_t generation) {
        std::lock_guard<std::mutex> lock(mu_);
        if (generation != cameraReaderGeneration_) return;
        destroyRawInput();
        cameraReaderGeneration_ = 0;
    };
    callbacks.submitMetadata = [this](const rawrcam::metadata::FrameMetadataSnapshot& metadata) {
        return submitMetadata(metadata);
    };
    callbacks.diagnostic = [this](const std::string& line) { recordDiagnosticLine(line); };
    auto controller = std::make_unique<rawrcam::camera::NativeCameraController>(std::move(callbacks));
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
    return ingressQueue_.enqueue(metadata);
}
void SessionEngine::enqueueRawFrame(imaging::AcquiredRawFrame frame) { ingressQueue_.enqueue(std::move(frame)); }
void SessionEngine::stopIngress() { ingressQueue_.stop(); }
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
    if (drops.images) realtime_.timing().recordDropped(drops.images);
    if (drops.images && videoSession_.ready() && realtime_.coordinator().videoSubmitted() > 0)
        videoIngressDrops_.fetch_add(drops.images, std::memory_order_relaxed);
    if (drops.images || drops.metadata)
        LOGI("INGRESS_QUEUE_PRESSURE rawDropped=%llu metadataDropped=%llu",
             static_cast<unsigned long long>(drops.images), static_cast<unsigned long long>(drops.metadata));
    if (raw)
        realtime_.coordinator().onRawFrame(std::get<imaging::RawFrameLease>(event.payload).release());
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
