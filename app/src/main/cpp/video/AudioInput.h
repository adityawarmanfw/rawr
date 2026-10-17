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

    struct Config {
        int channels = 2;
        // AAUDIO_INPUT_PRESET_*; CAMCORDER applies the device's AGC/NS, UNPROCESSED none.
        int preset = AAUDIO_INPUT_PRESET_CAMCORDER;
        // AudioDeviceInfo id; 0 lets Android route (USB/wired mics win when attached).
        int deviceId = 0;
    };
    struct Block {
        // Interleaved, channels() wide, after low-cut and gain; within [-1, 1).
        const float* pcm = nullptr;
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
        // Monotonic counts of blocks that clipped; readers latch on change. Input clips
        // are full-scale samples from the mic itself (gain cannot fix them); output
        // clips are samples pushed past full scale by gain.
        uint32_t inputClips = 0;
        uint32_t outputClips = 0;
        bool failed = false;
    };

    // Reuses the open stream when there is one, otherwise opens one with
    // `config` (stereo falls back to mono). Throws if the mic cannot open.
    static std::shared_ptr<AudioInput> acquire(const Config& config);

    ~AudioInput();
    AudioInput(const AudioInput&) = delete;
    AudioInput& operator=(const AudioInput&) = delete;

    [[nodiscard]] int channels() const noexcept { return channels_; }
    [[nodiscard]] const Config& config() const noexcept { return config_; }
    // Digital gain and high-pass corner, applied live to the meter and the recording.
    void setProcessing(float gainDb, int lowCutHz) noexcept;
    [[nodiscard]] int32_t xruns() const noexcept { return xruns_.load(); }
    // Replaces the consumer. Returns only after any in-flight onBlock call finishes.
    void setSink(Sink sink);
    // Consumes the held peaks; intended for the single UI meter reader.
    [[nodiscard]] Levels takeLevels() noexcept;

private:
    explicit AudioInput(const Config& config);
    void run() noexcept;

    Config config_;
    AAudioStream* stream_ = nullptr;
    int channels_ = 0;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::mutex sinkMutex_;
    Sink sink_;
    std::atomic<float> peak_[kMaxChannels];
    std::atomic<float> rms_[kMaxChannels];
    std::atomic<uint32_t> inputClips_{0};
    std::atomic<uint32_t> outputClips_{0};
    std::atomic<float> gainDb_{0.0f};
    std::atomic<int> lowCutHz_{0};
    std::atomic<int32_t> xruns_{0};
    std::atomic<bool> failed_{false};
};

}  // namespace rawrcam::video
