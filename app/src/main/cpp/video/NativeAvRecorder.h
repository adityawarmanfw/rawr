#pragma once

#include <aaudio/AAudio.h>
#include <android/native_window.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaMuxer.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "diagnostics/timing/RecentPeak.h"

namespace rawrcam::video {

// Owns the microphone, both hardware encoders, their drain threads, and the
// muxer. The UI hands over one MediaStore fd and the resulting encoder Surface.
class NativeAvRecorder final {
public:
    enum class TimestampPolicy : int {
        PreserveRealtime = 0,
        FixedCadenceRepeat = 1,
    };

    [[nodiscard]] static bool supportsTimestampPolicy(TimestampPolicy policy) noexcept;

    struct Settings {
        int width = 1920;
        int height = 1080;
        int fps = 30;
        int bitrate = 12'000'000;
        int intraSeconds = 1;
        int rotationDegrees = 0;
        int bitrateMode = 2;
        int maxBFrames = -1;
        int audioChannels = 2;
        int audioBitrate = 192'000;
        // 8 = HEVC Main, 10 = HEVC Main10.
        int bitDepth = 10;
        // Written into the finished MP4 (moov/udta and mdta keys).
        std::string deviceMake;
        std::string deviceModel;
        std::string software;
        std::string renderProfile;
        std::string gamut;
        std::string transfer;
        bool log = false;
        TimestampPolicy timestampPolicy = TimestampPolicy::PreserveRealtime;
    };

    NativeAvRecorder(int outputFd, Settings settings);
    ~NativeAvRecorder();
    NativeAvRecorder(const NativeAvRecorder&) = delete;
    NativeAvRecorder& operator=(const NativeAvRecorder&) = delete;

    void start();
    bool stop() noexcept;
    [[nodiscard]] ANativeWindow* inputWindow() const noexcept { return inputWindow_; }
    [[nodiscard]] std::string statsJson() const;

private:
    enum class Track { Video, Audio };
    struct PendingSample {
        Track track;
        AMediaCodecBufferInfo info{};
        std::vector<uint8_t> bytes;
    };

    void configureVideo();
    void configureAudio();
    void drainVideo() noexcept;
    void captureAudio() noexcept;
    void drainAudio(bool untilEos);
    void setFormat(Track track, AMediaFormat* format);
    void writeSample(Track track, const uint8_t* data, size_t capacity,
                     const AMediaCodecBufferInfo& info, int64_t presentationTimeUs);
    void writePendingLocked();
    void setFailure(const std::string& message) noexcept;
    void cleanup() noexcept;
    [[nodiscard]] static int64_t bootNs() noexcept;
    [[nodiscard]] static std::string escaped(const std::string& value);

    int outputFd_ = -1;
    Settings settings_{};
    int actualAudioChannels_ = 0;
    int actualAudioBitrate_ = 0;
    bool audioFallback_ = false;
    int64_t originBootUs_ = 0;
    int64_t monotonicToBootUs_ = 0;
    AMediaCodec* videoCodec_ = nullptr;
    std::string videoCodecName_;
    AMediaCodec* audioCodec_ = nullptr;
    ANativeWindow* inputWindow_ = nullptr;
    AAudioStream* audioStream_ = nullptr;
    AMediaMuxer* muxer_ = nullptr;
    std::thread videoThread_;
    std::thread audioThread_;
    std::atomic<bool> audioRunning_{false};
    std::atomic<bool> stopRequested_{false};
    bool started_ = false;
    bool muxerStarted_ = false;
    bool videoFormatReady_ = false;
    bool audioFormatReady_ = false;
    int videoTrack_ = -1;
    int audioTrack_ = -1;
    std::vector<PendingSample> pending_;
    size_t pendingBytes_ = 0;
    mutable std::mutex muxerMutex_;
    mutable std::mutex statsMutex_;
    std::string failure_;
    std::string metadataStatus_ = "pending";
    uint64_t encodedFrames_ = 0;
    uint64_t encodedAudioPackets_ = 0;
    int64_t firstVideoPtsUs_ = -1;
    int64_t lastVideoPtsUs_ = -1;
    int64_t lastSnappedVideoPtsUs_ = -1;
    int64_t firstAudioPtsUs_ = -1;
    int64_t lastAudioPtsUs_ = -1;
    // Sensor capture to encoded output when frames carry sensor timestamps.
    double encoderLatencyMs_ = 0.0;
    rawrcam::diagnostics::RecentPeak encoderLatencyPeak_;
    std::deque<int64_t> recentVideoPtsUs_;
    mutable uint64_t cumulativeTargetDeficit_ = 0;
    uint64_t encodedFrameGaps_ = 0;
    uint64_t audioSamples_ = 0;
    long double audioSumSquares_ = 0;
    int16_t audioPeak_ = 0;
    int32_t audioXruns_ = 0;
};

}  // namespace rawrcam::video
