#pragma once

#include <aaudio/AAudio.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace rawrcam::video {

// The one microphone stream for video mode. The level meter and the recorder
// share it, so recording starts on an already-running stream with no reopen
// gap. The stream closes when the last holder releases it.
class AudioInput final {
public:
    static constexpr int kSampleRate = 48'000;
    static constexpr int kMaxChannels = 2;
    static constexpr int kReadFrames = 1024;

    struct Block {
        const int16_t* pcm = nullptr;  // Interleaved, channels() wide.
        int frames = 0;
        // CLOCK_BOOTTIME of the first frame, smoothed against AAudio timestamps.
        int64_t firstFrameBootNs = 0;
    };
    struct Sink {
        std::function<void(const Block&)> onBlock;
        std::function<void(const std::string&)> onError;
    };
    // Linear 0..1 full scale. Peaks hold the maximum since the previous read.
    struct Levels {
        int channels = 0;
        float peak[kMaxChannels] = {};
        float rms[kMaxChannels] = {};
        // Monotonic counts of blocks with a full-scale sample; readers latch on change.
        uint32_t clips = 0;
        bool failed = false;
    };

    // Reuses the open stream when there is one, otherwise opens one with
    // `channels` (stereo falls back to mono). Throws if the mic cannot open.
    static std::shared_ptr<AudioInput> acquire(int channels);

    ~AudioInput();
    AudioInput(const AudioInput&) = delete;
    AudioInput& operator=(const AudioInput&) = delete;

    [[nodiscard]] int channels() const noexcept { return channels_; }
    [[nodiscard]] int32_t xruns() const noexcept { return xruns_.load(); }
    // Replaces the consumer. Returns only after any in-flight onBlock call finishes.
    void setSink(Sink sink);
    // Consumes the held peaks; intended for the single UI meter reader.
    [[nodiscard]] Levels takeLevels() noexcept;

private:
    explicit AudioInput(int channels);
    void run() noexcept;

    AAudioStream* stream_ = nullptr;
    int channels_ = 0;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::mutex sinkMutex_;
    Sink sink_;
    std::atomic<float> peak_[kMaxChannels];
    std::atomic<float> rms_[kMaxChannels];
    std::atomic<uint32_t> clips_{0};
    std::atomic<int32_t> xruns_{0};
    std::atomic<bool> failed_{false};
};

}  // namespace rawrcam::video
