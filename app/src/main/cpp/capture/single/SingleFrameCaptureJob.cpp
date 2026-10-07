#include "capture/single/SingleFrameCaptureJob.h"

#include "develop/render/RenderResources.h"

#include <android/log.h>
#include <unistd.h>

namespace rawrcam::capture {
SingleFrameCaptureJob::SingleFrameCaptureJob(const vulkan::VulkanContext& vk, std::mutex& mutex, std::string filesDir,
                                             Diagnostic diagnostic)
    : diagnostic_(std::move(diagnostic)),
      journal_(diagnostic_),
      acquisition_(journal_, diagnostic_),
      develop_(vk, mutex, filesDir, diagnostic_),
      outputs_(std::move(filesDir), diagnostic_) {}
SingleFrameCaptureJob::~SingleFrameCaptureJob() { shutdown(); }
uint64_t SingleFrameCaptureJob::requestRawStillCapture(encoding::dng::DngCaptureContext dng, JpegCaptureRequest jpeg) {
    support::UniqueFd dngDestination(dng.outputFd), jpegDestination(jpeg.output.outputFd);
    if (!SingleFrameCaptureOutputs::validate(dng, jpeg) || detachedHqWorkActive()) {
        emit("RAW_STILL_CAPTURE_REQUEST_REJECTED reason=invalid_output_or_busy");
        return 0;
    }
    const uint64_t id = nextRequestId_++;
    if (!acquisition_.request(id)) {
        return 0;
    }
    const bool jpegRequested = jpeg.output.outputFd >= 0;
    journal_.freeze(dng, jpegRequested ? &jpeg : nullptr);
    dng.outputFd = dngDestination.release();
    jpeg.output.outputFd = jpegDestination.release();
    outputs_.accept(std::move(dng), std::move(jpeg), jpegRequested);
    requestId_ = id;
    const auto* acceptedJpeg = outputs_.jpegRequest();
    emit("STILL_CAPTURE_REQUEST_ACCEPTED requestId=" + std::to_string(id) +
         " outputs=" + (jpegRequested ? std::string("dng+jpeg") : std::string("dng")) +
         " demosaic=" + (acceptedJpeg ? singleFrameDemosaicName(acceptedJpeg->develop.demosaicAlgorithm) : "NONE") +
         " pipelineDiagnostics=" +
         (acceptedJpeg && acceptedJpeg->develop.pipelineDiagnosticsEnabled ? "true" : "false") + " colorRender=" +
         (acceptedJpeg ? tonemap_integration::colorRenderProfileName(acceptedJpeg->develop.colorRenderProfile)
                       : "none") +
         " sameRawSnapshot=true");
    return id;
}
bool SingleFrameCaptureJob::deferSubmittedFrame(const metadata::FrameMetadataSnapshot& metadata,
                                                const color::FrameColorTransform& color,
                                                const tonemap::TonemapParams& tone, float gain, bool film,
                                                const spektrafilm_native::FilmLook& look) {
    const bool claimed = acquisition_.deferSubmittedFrame(metadata, color, tone, gain, film, look);
    if (acquisition_.metadataTimedOut()) failLensShadingTimeout();
    return claimed;
}
bool SingleFrameCaptureJob::beginDeferredSnapshot(AImage* image, AHardwareBuffer* ahb, uint64_t timestamp, int fence) {
    return acquisition_.beginDeferredSnapshot(image, ahb, timestamp, fence);
}
SingleMatchedFrameResult SingleFrameCaptureJob::processMatchedFrame(AImage* image, AHardwareBuffer* ahb, int fence,
                                                                    const metadata::FrameMetadataSnapshot& metadata,
                                                                    const color::FrameColorTransform& color,
                                                                    const tonemap::TonemapParams& tone, float gain) {
    auto result = acquisition_.processMatchedFrame(image, ahb, fence, metadata, color, tone, gain);
    if (acquisition_.metadataTimedOut()) failLensShadingTimeout();
    return result;
}
void SingleFrameCaptureJob::consumeAcquisition() {
    if (auto failure = acquisition_.takeFailure()) {
        __android_log_print(ANDROID_LOG_ERROR, "RawrCamNative", "RAW_STILL_SNAPSHOT_FAILED requestId=%llu reason=%s",
                            static_cast<unsigned long long>(requestId_), failure->c_str());
        outputs_.failDng(requestId_, "deferred_raw_snapshot_failed: " + *failure);
        if (outputs_.jpegRequest()) outputs_.failJpeg(requestId_, "deferred_raw_snapshot_failed");
        acquisition_.recycle();
        return;
    }
    if (auto ready = acquisition_.takeReady()) {
        journal_.reloadRaw(*ready);
        emit("RAW_STILL_CAPTURE_SNAPSHOT_READY requestId=" + std::to_string(ready->frame->requestId) +
             " timestampNs=" + std::to_string(ready->frame->timestampNs) +
             " copyMs=" + std::to_string(ready->copyResult.copyMs) + " scheduling=after_preview_fence");
        startCapturedOutputs(std::move(*ready));
    }
}
void SingleFrameCaptureJob::startCapturedOutputs(SingleFrameSnapshot snapshot) {
    auto frame = std::const_pointer_cast<const imaging::RawSnapshot>(snapshot.frame);
    if (!frame) return;
    outputs_.prepare(*frame, snapshot.tonemapParams, snapshot.aePostGain, snapshot.filmEnabled);
    if (const auto* jpeg = outputs_.jpegRequest())
        develop_.start(frame, snapshot.tonemapParams, snapshot.aePostGain, snapshot.filmEnabled, snapshot.filmLook,
                       *jpeg);
    if (!outputs_.startDng(frame, journal_.resolvedRecipe())) acquisition_.releaseFrame();
}
void SingleFrameCaptureJob::advanceDng() {
    consumeAcquisition();
    if (outputs_.advanceDng()) acquisition_.releaseFrame();
}
void SingleFrameCaptureJob::advanceHq() {
    if (outputs_.advanceJpeg()) develop_.releasePixels();
    if (auto result = develop_.pollCompletion()) {
        if (result->completion.filmFallbackMemory && outputs_.jpegRequest()) {
            journal_.markFilmFallback();
            outputs_.failJpeg(result->completion.requestId,
                              "film_memory_saved_as_dng " + develop::rendered::lastFilmGateSummary(), true);
            develop_.releasePixels();
        } else if (!outputs_.startJpeg(*result)) {
            develop_.releasePixels();
        }
    }
}
void SingleFrameCaptureJob::advance() {
    advanceDng();
    advanceHq();
}
std::string SingleFrameCaptureJob::pollDngCompletion() {
    advanceDng();
    return outputs_.pollDngCompletion();
}
std::string SingleFrameCaptureJob::pollJpegCompletion() {
    advanceHq();
    return outputs_.pollJpegCompletion();
}
bool SingleFrameCaptureJob::detachedHqWorkActive() const noexcept {
    return acquisition_.captureRequested() || acquisition_.busy() || outputs_.busy() || develop_.busy();
}
void SingleFrameCaptureJob::failLensShadingTimeout(const std::string& reason) {
    acquisition_.cancel(reason);
    outputs_.failDng(requestId_, reason);
    if (outputs_.jpegRequest()) outputs_.failJpeg(requestId_, reason);
}
void SingleFrameCaptureJob::cancelAcquisition(const std::string& reason) { failLensShadingTimeout(reason); }
void SingleFrameCaptureJob::cancelPendingDngBeforeIngressDestroy() {
    acquisition_.cancel("camera_ingress_destroyed");
    outputs_.cancelDng();
}
void SingleFrameCaptureJob::resetHqProcessing() noexcept {
    outputs_.resetJpeg();
    develop_.reset();
}
void SingleFrameCaptureJob::shutdown() noexcept {
    acquisition_.shutdown();
    cancelPendingDngBeforeIngressDestroy();
    outputs_.resetJpeg();
    develop_.shutdown();
}
void SingleFrameCaptureJob::recover(persistence::CaptureJob saved, const std::string& path, uint64_t id, int dng,
                                    int jpeg, bool jpegRequested, AAssetManager* assets) {
    support::UniqueFd dngDestination(dng), jpegDestination(jpeg);
    journal_.setRecoveryPath(path);
    setAssetManager(assets);
    saved.dng.outputFd = dng;
    saved.jpeg.output.outputFd = jpeg;
    saved.frame.requestId = id;
    requestId_ = id;
    journal_.freeze(saved.dng, jpegRequested ? &saved.jpeg : nullptr);
    saved.dng.outputFd = dngDestination.release();
    saved.jpeg.output.outputFd = jpegDestination.release();
    outputs_.accept(std::move(saved.dng), std::move(saved.jpeg), jpegRequested);
    SingleFrameSnapshot snapshot{std::make_shared<imaging::RawSnapshot>(std::move(saved.frame)),
                                 saved.tone,
                                 saved.gain,
                                 saved.filmEnabled,
                                 saved.film,
                                 {}};
    startCapturedOutputs(std::move(snapshot));
}

}  // namespace rawrcam::capture
