#pragma once

#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include "geometry/RawGeometry.h"

namespace rawrcam::imaging {

struct AcquiredRawFrame {
    AImage* image = nullptr;
    AHardwareBuffer* ahb = nullptr;
    int acquireFenceFd = -1;
    uint64_t timestampNs = 0;
    uint64_t generation = 0;
};

class RawFrameIngress final {
   public:
    using FrameCallback = std::function<void(AcquiredRawFrame)>;
    using Diagnostic = std::function<void(const std::string&)>;

    RawFrameIngress(FrameCallback frameCallback, Diagnostic diagnostic, Diagnostic pipelineAudit);
    ~RawFrameIngress();

    RawFrameIngress(const RawFrameIngress&) = delete;
    RawFrameIngress& operator=(const RawFrameIngress&) = delete;

    ANativeWindow* create(uint64_t generation, uint32_t width, uint32_t height, geometry::RawPixelFormat format,
                          uint64_t ahbUsage);
    void destroy();
    // The full-size capture worker calls this after returning an AImage lease.
    // Drain on that worker, serialized with arrival callbacks. A retired generation is a no-op.
    void resumeAfterRelease(uint64_t generation);
    // AHardwareBuffer usage the live reader requested (0 when none).
    [[nodiscard]] uint64_t requestedUsage() const noexcept { return reader_ ? usage_ : 0; }

   private:
    static void imageAvailableThunk(void* context, AImageReader* reader);
    void onImageAvailable(AImageReader* reader);
    void drainLocked(AImageReader* reader);
    std::mutex drainMutex_;
    uint64_t acquiredCount_ = 0, releasedCount_ = 0, slotLimitHits_ = 0;
    void logImageDescription(AImage* image);
    // Logs RAW_READER_STATS every 120 frames: reader starvation and sensor drops.
    void recordReaderStats(uint64_t timestampNs);

    struct ReaderStats {
        std::array<uint64_t, 120> intervalsNs{};
        uint32_t count = 0;
        uint32_t maxImagesHits = 0;
        uint64_t lastTimestampNs = 0;
    };
    ReaderStats readerStats_;
    FrameCallback frameCallback_;
    Diagnostic diagnostic_;
    Diagnostic pipelineAudit_;
    AImageReader* reader_ = nullptr;
    uint64_t generation_ = 0;
    uint64_t usage_ = 0;
    bool imageDescriptionLogged_ = false;
    uint32_t auditImageCount_ = 0;
};

}  // namespace rawrcam::imaging
