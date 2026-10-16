#pragma once
#include <chrono>
#include <mutex>
#include <thread>

#include "capture/single/SingleCaptureResult.h"
#include "capture/single/SingleFrameCaptureJournal.h"
#include "capture/single/SingleShotSlot.h"
namespace rawrcam::capture {
class SingleFrameCaptureAcquisition final {
   public:
    SingleFrameCaptureAcquisition(SingleFrameCaptureJournal& journal, CaptureDiagnostic diagnostic)
        : journal_(journal), diagnostic_(std::move(diagnostic)), slot_(diagnostic_) {}
    ~SingleFrameCaptureAcquisition() { shutdown(); }
    void configure(uint32_t w, uint32_t h, uint64_t generation) { slot_.configure(w, h, generation); }
    void disable(const std::string& reason) { slot_.disable(reason); }
    bool configured() const noexcept { return slot_.configured(); }
    bool captureRequested() const noexcept { return slot_.captureRequested(); }
    bool request(uint64_t id) {
        waitFrames_ = 0;
        return slot_.requestCapture(id);
    }
    void expectOptimizedFrame(uint64_t id, std::chrono::milliseconds extraWait) {
        slot_.expectOptimizedFrame(id, extraWait);
    }
    bool busy() const noexcept;
    bool metadataTimedOut() const noexcept { return waitFrames_ >= 8; }
    bool expectsTimestamp(uint64_t timestamp) const;
    bool deferSubmittedFrame(const metadata::FrameMetadataSnapshot&, const color::FrameColorTransform&,
                             const tonemap::TonemapParams&, float, bool, const spektrafilm_native::FilmLook&);
    bool beginDeferredSnapshot(AImage*, AHardwareBuffer*, uint64_t, int);
    SingleMatchedFrameResult processMatchedFrame(AImage*, AHardwareBuffer*, int, const metadata::FrameMetadataSnapshot&,
                                                 const color::FrameColorTransform&, const tonemap::TonemapParams&,
                                                 float);
    std::optional<SingleFrameSnapshot> takeReady();
    std::optional<std::string> takeFailure();
    void cancel(const std::string& reason);
    void recycle() { slot_.recycleDeferredSlot(); }
    void releaseFrame() { slot_.reset(); }
    void shutdown() noexcept;

   private:
    void failLensShadingTimeout(const std::string& reason = "lens_shading_map_timeout");
    void emit(const std::string& line) const {
        if (diagnostic_) diagnostic_(line);
    }
    SingleFrameCaptureJournal& journal_;
    CaptureDiagnostic diagnostic_;
    SingleShotSlot slot_;
    mutable std::mutex mutex_;
    std::thread worker_;
    std::optional<SingleFrameSnapshot> pending_, ready_;
    std::optional<std::string> failure_;
    bool workerRunning_ = false;
    uint32_t waitFrames_ = 0;
};
}  // namespace rawrcam::capture
