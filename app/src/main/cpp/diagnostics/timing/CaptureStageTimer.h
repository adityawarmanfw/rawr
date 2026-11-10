#pragma once
#include <android/log.h>
#include <chrono>
#include <cstdint>

#ifndef NDEBUG
#define CAPTURE_TIMING_LOG(...) __android_log_print(ANDROID_LOG_INFO, "RawrCamTiming", __VA_ARGS__)
#define CAPTURE_DEBUG_LOG(...) LOGI(__VA_ARGS__)
#else
#define CAPTURE_TIMING_LOG(...) ((void)0)
#define CAPTURE_DEBUG_LOG(...) ((void)0)
#endif
namespace rawrcam::diagnostics {
#ifndef NDEBUG
// Wall-clock stages, including blocking IO/GPU waits. Begin/end pairs expose
// hangs too. id is a burst tag during fullres acquisition, requestId afterwards.
// monoMs always uses steady_clock; never subtract it from sensorTimestampNs.
inline double captureMonoMs() noexcept {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
class CaptureStageTimer {
   public:
    CaptureStageTimer(const char* stage, uint64_t id, uint64_t frame = 0) noexcept
        : stage_(stage), id_(id), frame_(frame), started_(captureMonoMs()) {
        __android_log_print(ANDROID_LOG_INFO, "RawrCamTiming",
            "CAPTURE_TIMING stage=%s id=%llu frame=%llu event=begin monoMs=%.3f",
            stage_, static_cast<unsigned long long>(id_), static_cast<unsigned long long>(frame_), started_);
    }
    ~CaptureStageTimer() { finish(); }
    CaptureStageTimer(const CaptureStageTimer&) = delete;
    CaptureStageTimer& operator=(const CaptureStageTimer&) = delete;
    void finish() noexcept {
        if (finished_) return;
        const double now = captureMonoMs();
        __android_log_print(ANDROID_LOG_INFO, "RawrCamTiming",
            "CAPTURE_TIMING stage=%s id=%llu frame=%llu event=end monoMs=%.3f wallMs=%.3f",
            stage_, static_cast<unsigned long long>(id_), static_cast<unsigned long long>(frame_), now, now - started_);
        finished_ = true;
    }
   private:
    const char* stage_;
    uint64_t id_, frame_;
    double started_;
    bool finished_ = false;
};
#else
class CaptureStageTimer {
   public:
    CaptureStageTimer(const char*, uint64_t, uint64_t = 0) noexcept {}
    void finish() noexcept {}
};
#endif
}  // namespace rawrcam::diagnostics
