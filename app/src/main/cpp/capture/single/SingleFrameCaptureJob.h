#pragma once
#include "capture/single/SingleFrameCaptureAcquisition.h"
#include "capture/single/SingleFrameCaptureDevelop.h"
#include "capture/single/SingleFrameCaptureOutputs.h"
namespace rawrcam::capture {
class SingleFrameCaptureJob final {
   public:
    using Diagnostic = CaptureDiagnostic;
    SingleFrameCaptureJob(const vulkan::VulkanContext&, std::mutex&, std::string, Diagnostic);
    ~SingleFrameCaptureJob();
    void configureRawSnapshot(uint32_t w, uint32_t h, uint64_t generation) { acquisition_.configure(w, h, generation); }
    void disableRawSnapshot(const std::string& reason) { acquisition_.disable(reason); }
    bool rawSnapshotConfigured() const noexcept { return acquisition_.configured(); }
    bool captureRequested() const noexcept { return acquisition_.captureRequested(); }
    bool deferredSnapshotBusy() const noexcept { return acquisition_.busy(); }
    uint64_t requestRawStillCapture(encoding::dng::DngCaptureContext, JpegCaptureRequest);
    void expectOptimizedFrame(uint64_t id) { acquisition_.expectOptimizedFrame(id); }
    bool deferSubmittedFrame(const metadata::FrameMetadataSnapshot&, const color::FrameColorTransform&,
                             const tonemap::TonemapParams&, float, bool, const spektrafilm_native::FilmLook&);
    bool beginDeferredSnapshot(AImage*, AHardwareBuffer*, uint64_t, int);
    SingleMatchedFrameResult processMatchedFrame(AImage*, AHardwareBuffer*, int, const metadata::FrameMetadataSnapshot&,
                                                 const color::FrameColorTransform&, const tonemap::TonemapParams&,
                                                 float);
    void failLensShadingTimeout(const std::string& reason = "lens_shading_map_timeout");
    void advance();
    void advanceDng();
    void advanceHq();
    std::string pollDngCompletion();
    std::string pollJpegCompletion();
    bool detachedHqWorkActive() const noexcept;
    void cancelPendingDngBeforeIngressDestroy();
    void resetHqProcessing() noexcept;
    void shutdown() noexcept;
    void setResident(bool value) noexcept { journal_.setResident(value); }
    void setAssetManager(AAssetManager* assets) { develop_.setAssetManager(assets); }
    void setAdmission(persistence::CaptureReservation reservation, std::string path, uint64_t id) {
        journal_.setAdmission(std::move(reservation), std::move(path));
        nextRequestId_ = id;
    }
    bool hasDngCompletion() const noexcept { return outputs_.hasDngCompletion(); }
    bool expectsTimestamp(uint64_t timestamp) const { return acquisition_.expectsTimestamp(timestamp); }
    void cancelAcquisition(const std::string& reason);
    void recover(persistence::CaptureJob, const std::string&, uint64_t, encoding::OutputFd dng, encoding::OutputFd jpeg,
                 bool, AAssetManager*);

   private:
    void consumeAcquisition();
    void startCapturedOutputs(SingleFrameSnapshot snapshot);
    void emit(const std::string& line) const {
        if (diagnostic_) diagnostic_(line);
    }
    CaptureDiagnostic diagnostic_;
    // Destruction order: outputs joins encoding before develop releases pixels;
    // acquisition joins before its borrowed journal is destroyed.
    SingleFrameCaptureJournal journal_;
    SingleFrameCaptureAcquisition acquisition_;
    SingleFrameCaptureDevelop develop_;
    SingleFrameCaptureOutputs outputs_;
    uint64_t nextRequestId_ = 1, requestId_ = 0;
};
}  // namespace rawrcam::capture
