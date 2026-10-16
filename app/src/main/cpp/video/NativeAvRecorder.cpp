#include "video/HevcLogMetadata.h"
#include "video/NativeAvRecorder.h"

#include <android/log.h>
#include <media/NdkMediaFormat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include "video/Mp4MetadataPatcher.h"

namespace rawrcam::video {
namespace {
constexpr char kTag[] = "RawrNativeVideo";
constexpr int kSurfaceColorFormat = 2130708361;  // MediaCodecInfo.COLOR_FormatSurface.
constexpr int kMainProfile = 1;                  // HEVCProfileMain.
constexpr int kMain10Profile = 2;                // HEVCProfileMain10.
constexpr int kAacLcProfile = 2;
constexpr int kSampleRate = AudioInput::kSampleRate;
constexpr int kMaxAudioChannels = 2;
constexpr int kMonoFallbackBitrate = 128'000;

void requireMedia(media_status_t status, const char* operation) {
    if (status != AMEDIA_OK) throw std::runtime_error(std::string(operation) + " status=" + std::to_string(status));
}
int64_t clockNs(clockid_t which) noexcept {
    timespec ts{};
    clock_gettime(which, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}
}  // namespace

bool NativeAvRecorder::supportsTimestampPolicy(TimestampPolicy policy) noexcept {
    // The Vulkan encoder Surface generates PTS at presentation. Repeating a
    // frame needs a separate GPU present for each missing slot; rewriting
    // muxer PTS alone would desynchronize video and audio.
    return policy == TimestampPolicy::PreserveRealtime;
}

NativeAvRecorder::NativeAvRecorder(int outputFd, Settings settings) : outputFd_(outputFd), settings_(settings) {
    if (outputFd_ < 0 || settings_.width <= 0 || settings_.height <= 0 || (settings_.width & 1) != 0 ||
        (settings_.height & 1) != 0 || (settings_.fps != 24 && settings_.fps != 30) || settings_.bitrate < 1'000'000 ||
        settings_.bitrate > 400'000'000 || settings_.intraFrames < 1 || settings_.intraFrames > 60 ||
        settings_.rotationDegrees % 90 != 0 || settings_.bitrateMode < -1 || settings_.bitrateMode > 2 ||
        settings_.maxBFrames < -1 || settings_.maxBFrames > 4 ||
        (settings_.audioChannels != 1 && settings_.audioChannels != 2) || settings_.audioBitrate < 64'000 ||
        settings_.audioBitrate > 512'000 || (settings_.bitDepth != 8 && settings_.bitDepth != 10) ||
        (settings_.log && settings_.bitDepth != 10) || !supportsTimestampPolicy(settings_.timestampPolicy))
        throw std::invalid_argument("invalid native recording settings");
}

NativeAvRecorder::~NativeAvRecorder() { (void)stop(); }

int64_t NativeAvRecorder::bootNs() noexcept { return clockNs(CLOCK_BOOTTIME); }

void NativeAvRecorder::configureVideo() {
    videoCodec_ = AMediaCodec_createEncoderByType("video/hevc");
    if (!videoCodec_) throw std::runtime_error("HEVC encoder unavailable");
    char* codecName = nullptr;
    requireMedia(AMediaCodec_getName(videoCodec_, &codecName), "query HEVC encoder");
    videoCodecName_ = codecName ? codecName : "";
    if (codecName) AMediaCodec_releaseName(videoCodec_, codecName);
    if (videoCodecName_.rfind("c2.android.", 0) == 0 || videoCodecName_.rfind("OMX.google.", 0) == 0)
        throw std::runtime_error("software HEVC encoder selected: " + videoCodecName_);
    AMediaFormat* format = AMediaFormat_new();
    if (!format) throw std::runtime_error("video format allocation failed");
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "video/hevc");
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, settings_.width);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, settings_.height);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, kSurfaceColorFormat);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_PROFILE, settings_.bitDepth == 8 ? kMainProfile : kMain10Profile);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_BIT_RATE, settings_.bitrate);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_FRAME_RATE, settings_.fps);
    // i-frame-interval is in seconds and accepts a float; 0 asks for every frame to be a sync frame.
    AMediaFormat_setFloat(format, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL,
                          settings_.intraFrames <= 1 ? 0.0f
                                                     : static_cast<float>(settings_.intraFrames) / settings_.fps);
    if (settings_.bitrateMode >= 0) AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_BITRATE_MODE, settings_.bitrateMode);
    // Optional codec key. A codec may reject the request during configure;
    // omit it when left at the automatic default.
    const int maxBFrames = settings_.intraFrames <= 1 ? 0 : settings_.maxBFrames;
    if (maxBFrames >= 0) AMediaFormat_setInt32(format, "max-bframes", maxBFrames);
    // Preserve the encoder's BT.709 RGB-to-YCbCr conversion. LOG signalling
    // is rewritten after encoding, without changing the encoded samples.
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_STANDARD, 1);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_TRANSFER, 3);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_RANGE, 2);
    const media_status_t configured =
        AMediaCodec_configure(videoCodec_, format, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    AMediaFormat_delete(format);
    if (configured != AMEDIA_OK)
        throw std::runtime_error(std::string("configure HEVC ") + (settings_.bitDepth == 8 ? "Main " : "Main10 ") +
                                 std::to_string(settings_.width) + "x" + std::to_string(settings_.height) + " at " +
                                 std::to_string(settings_.fps) + " fps on " + videoCodecName_ +
                                 " status=" + std::to_string(configured));
    requireMedia(AMediaCodec_createInputSurface(videoCodec_, &inputWindow_), "create HEVC input surface");
    if (!inputWindow_) throw std::runtime_error("HEVC input surface is null");
    requireMedia(AMediaCodec_start(videoCodec_), "start HEVC");
}

void NativeAvRecorder::configureAudio() {
    const int requestedChannels = settings_.audioChannels;
    // Reuses the meter's running stream; a stereo stream is downmixed for a mono request.
    audioInput_ = AudioInput::acquire(requestedChannels);
    actualAudioChannels_ = std::min(requestedChannels, audioInput_->channels());
    audioFallback_ = (actualAudioChannels_ != requestedChannels);
    // Agreed policy: mono fallback steps down to 128k even if stereo asked for 192k.
    actualAudioBitrate_ =
        audioFallback_ ? std::min(settings_.audioBitrate, kMonoFallbackBitrate) : settings_.audioBitrate;

    audioCodec_ = AMediaCodec_createEncoderByType("audio/mp4a-latm");
    if (!audioCodec_) throw std::runtime_error("AAC encoder unavailable");
    AMediaFormat* format = AMediaFormat_new();
    if (!format) throw std::runtime_error("audio format allocation failed");
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "audio/mp4a-latm");
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_SAMPLE_RATE, kSampleRate);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, actualAudioChannels_);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_AAC_PROFILE, kAacLcProfile);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_BIT_RATE, actualAudioBitrate_);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE, AudioInput::kReadFrames * actualAudioChannels_ * 2);
    const media_status_t configured =
        AMediaCodec_configure(audioCodec_, format, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    AMediaFormat_delete(format);
    requireMedia(configured, "configure AAC");
    requireMedia(AMediaCodec_start(audioCodec_), "start AAC");
}

void NativeAvRecorder::start() {
    if (started_) throw std::runtime_error("recording already started");
    originBootUs_ = bootNs() / 1'000;
    monotonicToBootUs_ = (bootNs() - clockNs(CLOCK_MONOTONIC)) / 1'000;
    try {
        muxer_ = AMediaMuxer_new(outputFd_, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
        if (!muxer_) throw std::runtime_error("native MP4 muxer unavailable");
        requireMedia(AMediaMuxer_setOrientationHint(muxer_, (settings_.rotationDegrees + 360) % 360),
                     "set MP4 orientation");
        configureVideo();
        configureAudio();
        started_ = true;
        audioRunning_.store(true);
        videoThread_ = std::thread([this] { drainVideo(); });
        audioInput_->setSink({[this](const AudioInput::Block& block) { onAudioBlock(block); },
                              [this](const std::string& error) { setFailure(error); }});
    } catch (...) {
        audioRunning_.store(false);
        stopRequested_.store(true);
        if (audioInput_) audioInput_->setSink({});
        if (videoThread_.joinable() && videoCodec_) (void)AMediaCodec_signalEndOfInputStream(videoCodec_);
        cleanup();
        throw;
    }
}

void NativeAvRecorder::setFailure(const std::string& message) noexcept {
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        if (failure_.empty()) failure_ = message;
    }
    __android_log_print(ANDROID_LOG_ERROR, kTag, "%s", message.c_str());
}

void NativeAvRecorder::setFormat(Track track, AMediaFormat* format) {
    if (!format) throw std::runtime_error("encoder output format missing");
    if (track == Track::Video && settings_.log) {
        int32_t profile = 0;
        if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_PROFILE, &profile) && profile != kMain10Profile)
            throw std::runtime_error("LOG encoder did not produce HEVC Main10");
        void* csd = nullptr;
        size_t size = 0;
        if (!AMediaFormat_getBuffer(format, "csd-0", &csd, &size))
            throw std::runtime_error("LOG encoder output missing HEVC codec configuration");
        const auto patched = logHevcAccessUnit(static_cast<const uint8_t*>(csd), size, true);
        AMediaFormat_setBuffer(format, "csd-0", patched.data(), patched.size());
    }
    if (track == Track::Video) {
        void* csd = nullptr;
        size_t size = 0;
        if (AMediaFormat_getBuffer(format, "csd-0", &csd, &size))
            frameTypes_.setCodecConfig(static_cast<const uint8_t*>(csd), size);
    }
    std::lock_guard<std::mutex> lock(muxerMutex_);
    bool& ready = track == Track::Video ? videoFormatReady_ : audioFormatReady_;
    int& index = track == Track::Video ? videoTrack_ : audioTrack_;
    if (ready) throw std::runtime_error("duplicate encoder output format");
    index = static_cast<int>(AMediaMuxer_addTrack(muxer_, format));
    if (index < 0) throw std::runtime_error("add MP4 track failed: " + std::to_string(index));
    ready = true;
    if (videoFormatReady_ && audioFormatReady_) {
        requireMedia(AMediaMuxer_start(muxer_), "start MP4 muxer");
        muxerStarted_ = true;
        writePendingLocked();
    }
}

void NativeAvRecorder::writePendingLocked() {
    std::stable_sort(pending_.begin(), pending_.end(), [](const PendingSample& a, const PendingSample& b) {
        return a.info.presentationTimeUs < b.info.presentationTimeUs;
    });
    for (auto& sample : pending_) {
        const int index = sample.track == Track::Video ? videoTrack_ : audioTrack_;
        requireMedia(AMediaMuxer_writeSampleData(muxer_, static_cast<size_t>(index), sample.bytes.data(), &sample.info),
                     "write pending MP4 sample");
    }
    pending_.clear();
    pendingBytes_ = 0;
}

void NativeAvRecorder::writeSample(Track track, const uint8_t* data, size_t capacity, const AMediaCodecBufferInfo& info,
                                   int64_t presentationTimeUs) {
    if (info.size <= 0 || (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) != 0) return;
    if (!data || info.offset < 0 || static_cast<size_t>(info.offset) + static_cast<size_t>(info.size) > capacity)
        throw std::runtime_error("encoded buffer bounds invalid");
    AMediaCodecBufferInfo sample = info;
    sample.presentationTimeUs = std::max<int64_t>(0, presentationTimeUs - originBootUs_);
    std::vector<uint8_t> patched;
    if (track == Track::Video && settings_.log) {
        patched = logHevcAccessUnit(data + info.offset, static_cast<size_t>(info.size));
        if (patched.size() > INT32_MAX) throw std::runtime_error("LOG sample too large");
        data = patched.data();
        sample.offset = 0;
        sample.size = static_cast<int32_t>(patched.size());
    }
    std::lock_guard<std::mutex> lock(muxerMutex_);
    if (!muxerStarted_) {
        if (pending_.size() >= 256 || pendingBytes_ + static_cast<size_t>(sample.size) > 8'000'000)
            throw std::runtime_error("MP4 startup queue exceeded bound");
        PendingSample saved{};
        saved.track = track;
        saved.info = sample;
        saved.info.offset = 0;
        saved.bytes.assign(data + sample.offset, data + sample.offset + sample.size);
        pendingBytes_ += saved.bytes.size();
        pending_.push_back(std::move(saved));
        return;
    }
    const int index = track == Track::Video ? videoTrack_ : audioTrack_;
    requireMedia(AMediaMuxer_writeSampleData(muxer_, static_cast<size_t>(index), data, &sample), "write MP4 sample");
}

void NativeAvRecorder::drainVideo() noexcept {
    try {
        AMediaCodecBufferInfo info{};
        bool eos = false;
        int idleAfterStop = 0;
        while (!eos) {
            const ssize_t index = AMediaCodec_dequeueOutputBuffer(videoCodec_, &info, 20'000);
            if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                AMediaFormat* format = AMediaCodec_getOutputFormat(videoCodec_);
                try {
                    setFormat(Track::Video, format);
                } catch (...) {
                    if (format) AMediaFormat_delete(format);
                    throw;
                }
                AMediaFormat_delete(format);
                continue;
            }
            if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER || index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
                if (stopRequested_.load() && ++idleAfterStop > 500) throw std::runtime_error("HEVC EOS timeout");
                continue;
            }
            if (index < 0) throw std::runtime_error("HEVC dequeue failed: " + std::to_string(index));
            idleAfterStop = 0;
            eos = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
            try {
                if (info.size > 0 && (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) == 0) {
                    size_t capacity = 0;
                    const uint8_t* data =
                        AMediaCodec_getOutputBuffer(videoCodec_, static_cast<size_t>(index), &capacity);
                    const int64_t bootPtsUs = info.presentationTimeUs + monotonicToBootUs_;
                    // Quantize to the selected fps grid so one short delta (startup
                    // burst, catch-up, jitter, B-frame reorder) cannot produce a
                    // bogus MediaInfo max (e.g. 200fps on a 24fps recording).
                    // Gaps survive as multiples of the frame duration.
                    int64_t snappedPtsUs = bootPtsUs;
                    if (firstVideoPtsUs_ >= 0 && settings_.fps > 0) {
                        const int64_t frameDurUs = 1'000'000LL / settings_.fps;
                        const int64_t slots = static_cast<int64_t>(std::llround(
                            static_cast<double>(bootPtsUs - firstVideoPtsUs_) / static_cast<double>(frameDurUs)));
                        snappedPtsUs = firstVideoPtsUs_ + std::max<int64_t>(0, slots) * frameDurUs;
                        if (lastSnappedVideoPtsUs_ >= 0 && snappedPtsUs <= lastSnappedVideoPtsUs_)
                            snappedPtsUs = lastSnappedVideoPtsUs_ + frameDurUs;
                    }
                    writeSample(Track::Video, data, capacity, info, snappedPtsUs);
                    const auto frameType = frameTypes_.classify(data + info.offset, static_cast<size_t>(info.size));
                    std::lock_guard<std::mutex> lock(statsMutex_);
                    if (frameType != HevcFrameTypeParser::Type::Unknown) {
                        const int slot = frameType == HevcFrameTypeParser::Type::I   ? 0
                                         : frameType == HevcFrameTypeParser::Type::P ? 1
                                                                                     : 2;
                        ++frameTypeCount_[slot];
                        frameTypeBytes_[slot] += static_cast<uint64_t>(info.size);
                        if (slot == 0) {
                            if (frameTypeCount_[0] > 1) lastGopFrames_ = framesSinceI_;
                            framesSinceI_ = 0;
                        }
                    }
                    ++framesSinceI_;
                    if (lastVideoPtsUs_ >= 0 && bootPtsUs > lastVideoPtsUs_) {
                        const int64_t deltaUs = bootPtsUs - lastVideoPtsUs_;
                        const int64_t missed = static_cast<int64_t>(std::llround(static_cast<double>(deltaUs) *
                                                                                 settings_.fps / 1'000'000.0)) -
                                               1;
                        if (missed > 0) encodedFrameGaps_ += static_cast<uint64_t>(missed);
                    }
                    if (firstVideoPtsUs_ < 0) {
                        firstVideoPtsUs_ = bootPtsUs;
                        snappedPtsUs = bootPtsUs;
                    }
                    lastVideoPtsUs_ = bootPtsUs;
                    lastSnappedVideoPtsUs_ = snappedPtsUs;
                    recentVideoPtsUs_.push_back(snappedPtsUs);
                    while (recentVideoPtsUs_.size() > 2 && recentVideoPtsUs_.front() < snappedPtsUs - 5'000'000)
                        recentVideoPtsUs_.pop_front();
                    const double latency = std::max(0.0, (bootNs() / 1'000.0 - bootPtsUs) / 1'000.0);
                    encoderLatencyMs_ = encodedFrames_ == 0 ? latency : encoderLatencyMs_ * 0.9 + latency * 0.1;
                    encoderLatencyPeak_.add(bootNs(), latency);
                    ++encodedFrames_;
                }
            } catch (...) {
                AMediaCodec_releaseOutputBuffer(videoCodec_, static_cast<size_t>(index), false);
                throw;
            }
            requireMedia(AMediaCodec_releaseOutputBuffer(videoCodec_, static_cast<size_t>(index), false),
                         "release HEVC output");
        }
    } catch (const std::exception& error) {
        setFailure(error.what());
    }
}

void NativeAvRecorder::drainAudio(bool untilEos) {
    AMediaCodecBufferInfo info{};
    const int64_t deadline = bootNs() + 3'000'000'000LL;
    do {
        const ssize_t index = AMediaCodec_dequeueOutputBuffer(audioCodec_, &info, untilEos ? 10'000 : 0);
        if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat* format = AMediaCodec_getOutputFormat(audioCodec_);
            try {
                setFormat(Track::Audio, format);
            } catch (...) {
                if (format) AMediaFormat_delete(format);
                throw;
            }
            AMediaFormat_delete(format);
            continue;
        }
        if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER || index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            if (!untilEos) return;
            continue;
        }
        if (index < 0) throw std::runtime_error("AAC dequeue failed: " + std::to_string(index));
        const bool eos = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
        try {
            if (info.size > 0 && (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) == 0) {
                size_t capacity = 0;
                const uint8_t* data = AMediaCodec_getOutputBuffer(audioCodec_, static_cast<size_t>(index), &capacity);
                writeSample(Track::Audio, data, capacity, info, info.presentationTimeUs);
                std::lock_guard<std::mutex> lock(statsMutex_);
                if (firstAudioPtsUs_ < 0) firstAudioPtsUs_ = info.presentationTimeUs;
                lastAudioPtsUs_ = info.presentationTimeUs;
                ++encodedAudioPackets_;
            }
        } catch (...) {
            AMediaCodec_releaseOutputBuffer(audioCodec_, static_cast<size_t>(index), false);
            throw;
        }
        requireMedia(AMediaCodec_releaseOutputBuffer(audioCodec_, static_cast<size_t>(index), false),
                     "release AAC output");
        if (eos) return;
    } while (!untilEos || bootNs() < deadline);
    throw std::runtime_error("AAC EOS timeout");
}

void NativeAvRecorder::onAudioBlock(const AudioInput::Block& block) {
    if (!audioRunning_.load()) return;
    try {
        const int inputChannels = audioInput_->channels();
        const int channels = actualAudioChannels_;
        std::array<int16_t, AudioInput::kReadFrames * kMaxAudioChannels> mixed{};
        const int16_t* pcm = block.pcm;
        if (inputChannels != channels) {
            for (int frame = 0; frame < block.frames; ++frame)
                mixed[static_cast<size_t>(frame)] = static_cast<int16_t>(
                    (static_cast<int32_t>(block.pcm[frame * 2]) + block.pcm[frame * 2 + 1]) / 2);
            pcm = mixed.data();
        }
        long double squares = 0;
        int32_t peak = 0;
        const int64_t totalSamples = static_cast<int64_t>(block.frames) * channels;
        for (int64_t i = 0; i < totalSamples; ++i) {
            const int32_t value = pcm[static_cast<size_t>(i)];
            squares += static_cast<long double>(value) * value;
            peak = std::max(peak, std::abs(value));
        }
        {
            std::lock_guard<std::mutex> lock(statsMutex_);
            audioSamples_ += static_cast<uint64_t>(totalSamples);
            audioSumSquares_ += squares;
            audioPeak_ = static_cast<int16_t>(std::min<int32_t>(32'767, std::max<int32_t>(audioPeak_, peak)));
            audioXruns_ = audioInput_->xruns();
        }
        int consumed = 0;
        while (consumed < block.frames && audioRunning_.load()) {
            const ssize_t index = AMediaCodec_dequeueInputBuffer(audioCodec_, 10'000);
            if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
                drainAudio(false);
                continue;
            }
            if (index < 0) throw std::runtime_error("AAC input dequeue failed: " + std::to_string(index));
            size_t capacity = 0;
            uint8_t* input = AMediaCodec_getInputBuffer(audioCodec_, static_cast<size_t>(index), &capacity);
            const int part = std::min<int>(
                block.frames - consumed, static_cast<int>(capacity / (sizeof(int16_t) * static_cast<size_t>(channels))));
            if (!input || part <= 0) throw std::runtime_error("AAC input buffer unavailable");
            std::memcpy(input, pcm + static_cast<size_t>(consumed) * static_cast<size_t>(channels),
                        static_cast<size_t>(part) * static_cast<size_t>(channels) * sizeof(int16_t));
            const int64_t ptsUs = (block.firstFrameBootNs + consumed * 1'000'000'000LL / kSampleRate) / 1'000;
            requireMedia(AMediaCodec_queueInputBuffer(
                             audioCodec_, static_cast<size_t>(index), 0,
                             static_cast<size_t>(part) * static_cast<size_t>(channels) * sizeof(int16_t),
                             static_cast<uint64_t>(std::max<int64_t>(0, ptsUs)), 0),
                         "queue AAC PCM");
            consumed += part;
            audioEndUs_ = (block.firstFrameBootNs + consumed * 1'000'000'000LL / kSampleRate) / 1'000;
            drainAudio(false);
        }
    } catch (const std::exception& error) {
        audioRunning_.store(false);
        setFailure(error.what());
    }
}

// Runs on the stopping thread after the sink is detached, so no block is in flight.
void NativeAvRecorder::finishAudio() noexcept {
    if (!audioCodec_) return;
    try {
        for (;;) {
            const ssize_t index = AMediaCodec_dequeueInputBuffer(audioCodec_, 10'000);
            if (index >= 0) {
                requireMedia(AMediaCodec_queueInputBuffer(audioCodec_, static_cast<size_t>(index), 0, 0,
                                                          static_cast<uint64_t>(std::max<int64_t>(0, audioEndUs_)),
                                                          AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM),
                             "queue AAC EOS");
                break;
            }
            if (index != AMEDIACODEC_INFO_TRY_AGAIN_LATER)
                throw std::runtime_error("AAC EOS input dequeue failed: " + std::to_string(index));
            drainAudio(false);
        }
        drainAudio(true);
    } catch (const std::exception& error) {
        setFailure(error.what());
    }
}

bool NativeAvRecorder::stop() noexcept {
    if (!started_) {
        cleanup();
        return false;
    }
    started_ = false;
    if (audioInput_) audioInput_->setSink({});
    audioRunning_.store(false);
    finishAudio();
    stopRequested_.store(true);
    if (videoCodec_) {
        const media_status_t status = AMediaCodec_signalEndOfInputStream(videoCodec_);
        if (status != AMEDIA_OK) setFailure("signal HEVC EOS failed: " + std::to_string(status));
    }
    if (videoThread_.joinable()) videoThread_.join();
    bool finalized = false;
    {
        std::lock_guard<std::mutex> lock(muxerMutex_);
        if (muxerStarted_) {
            const media_status_t status = AMediaMuxer_stop(muxer_);
            if (status != AMEDIA_OK) setFailure("finalize MP4 failed: " + std::to_string(status));
            finalized = status == AMEDIA_OK;
            muxerStarted_ = false;
        } else
            setFailure("MP4 tracks were not initialized");
    }
    // Device tags are best effort. LOG colour signalling must succeed before publication.
    if (finalized && outputFd_ >= 0 && (settings_.log || !settings_.deviceMake.empty() || !settings_.deviceModel.empty())) {
        std::string error;
        const bool patched = patchMp4DeviceMetadata(
            outputFd_, {settings_.deviceMake, settings_.deviceModel, settings_.software, settings_.renderProfile,
                        settings_.gamut, settings_.transfer, std::to_string(settings_.bitDepth), settings_.log}, &error);
        if (!patched && settings_.log) setFailure("LOG metadata finalization failed: " + error);
        std::lock_guard<std::mutex> lock(statsMutex_);
        metadataStatus_ = patched ? "written" : "failed: " + error;
        if (!patched) __android_log_print(ANDROID_LOG_WARN, kTag, "MP4_METADATA_SKIPPED %s", error.c_str());
    }
    if (outputFd_ >= 0) (void)fsync(outputFd_);
    bool valid = false;
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        valid = failure_.empty() && encodedFrames_ > 0 && encodedAudioPackets_ > 0;
    }
    cleanup();
    return valid;
}

void NativeAvRecorder::cleanup() noexcept {
    audioRunning_.store(false);
    if (audioInput_) {
        audioInput_->setSink({});
        audioInput_.reset();
    }
    if (videoThread_.joinable()) videoThread_.join();
    if (audioCodec_) {
        (void)AMediaCodec_stop(audioCodec_);
        (void)AMediaCodec_delete(audioCodec_);
        audioCodec_ = nullptr;
    }
    if (videoCodec_) {
        (void)AMediaCodec_stop(videoCodec_);
        (void)AMediaCodec_delete(videoCodec_);
        videoCodec_ = nullptr;
    }
    if (inputWindow_) {
        ANativeWindow_release(inputWindow_);
        inputWindow_ = nullptr;
    }
    if (muxer_) {
        (void)AMediaMuxer_delete(muxer_);
        muxer_ = nullptr;
    }
    if (outputFd_ >= 0) {
        close(outputFd_);
        outputFd_ = -1;
    }
}

std::string NativeAvRecorder::escaped(const std::string& value) {
    std::string out;
    for (char ch : value) {
        if (ch == '"' || ch == '\\') out.push_back('\\');
        if (static_cast<unsigned char>(ch) >= 0x20) out.push_back(ch);
    }
    return out;
}

std::string NativeAvRecorder::statsJson() const {
    std::lock_guard<std::mutex> lock(statsMutex_);
    const char* policyName =
        settings_.timestampPolicy == TimestampPolicy::FixedCadenceRepeat ? "FIXED_CADENCE_REPEAT" : "REALTIME";
    const double currentFps = recentVideoPtsUs_.size() > 1 && recentVideoPtsUs_.back() > recentVideoPtsUs_.front()
                                  ? (recentVideoPtsUs_.size() - 1) * 1'000'000.0 /
                                        static_cast<double>(recentVideoPtsUs_.back() - recentVideoPtsUs_.front())
                                  : 0.0;
    const int64_t slots = firstVideoPtsUs_ >= 0 && lastVideoPtsUs_ >= firstVideoPtsUs_
                              ? 1 + (lastVideoPtsUs_ - firstVideoPtsUs_) * settings_.fps / 1'000'000
                              : 0;
    const uint64_t targetDeficit = slots > static_cast<int64_t>(encodedFrames_)
                                       ? static_cast<uint64_t>(slots - static_cast<int64_t>(encodedFrames_))
                                       : 0;
    cumulativeTargetDeficit_ = std::max(cumulativeTargetDeficit_, targetDeficit);
    const double rms = audioSamples_ == 0 ? 0.0 : std::sqrt(static_cast<double>(audioSumSquares_ / audioSamples_));
    const auto avgKb = [this](int slot) {
        return frameTypeCount_[slot] == 0 ? 0.0 : frameTypeBytes_[slot] / 1024.0 / frameTypeCount_[slot];
    };
    const double audioRmsDbfs = rms <= 0.0 ? -120.0 : 20.0 * std::log10(rms / 32768.0);
    return std::string("{\"videoCodec\":\"") + escaped(videoCodecName_) +
           "\",\"width\":" + std::to_string(settings_.width) + ",\"height\":" + std::to_string(settings_.height) +
           ",\"requestedFps\":" + std::to_string(settings_.fps) + ",\"bitrate\":" + std::to_string(settings_.bitrate) +
           ",\"intraFrames\":" + std::to_string(settings_.intraFrames) +
           ",\"bitrateMode\":" + std::to_string(settings_.bitrateMode) +
           ",\"maxBFrames\":" + std::to_string(settings_.maxBFrames) +
           ",\"bitDepth\":" + std::to_string(settings_.bitDepth) +
           ",\"audioChannelsRequested\":" + std::to_string(settings_.audioChannels) + ",\"audioChannelsActual\":" +
           std::to_string(actualAudioChannels_ > 0 ? actualAudioChannels_ : settings_.audioChannels) +
           ",\"audioBitrateRequested\":" + std::to_string(settings_.audioBitrate) + ",\"audioBitrateActual\":" +
           std::to_string(actualAudioBitrate_ > 0 ? actualAudioBitrate_ : settings_.audioBitrate) +
           ",\"audioFallback\":" + (audioFallback_ ? "true" : "false") + ",\"timestampPolicy\":\"" + policyName +
           "\",\"videoTimestampSource\":\"encoder_surface_grid_snapped\"" +
           ",\"encodedFrames\":" + std::to_string(encodedFrames_) +
           ",\"encodedAudioPackets\":" + std::to_string(encodedAudioPackets_) +
           ",\"currentFps\":" + std::to_string(currentFps) +
           ",\"targetFrameShortfall\":" + std::to_string(cumulativeTargetDeficit_) +
           ",\"encodedFrameGaps\":" + std::to_string(encodedFrameGaps_) +
           ",\"iFrames\":" + std::to_string(frameTypeCount_[0]) + ",\"pFrames\":" + std::to_string(frameTypeCount_[1]) +
           ",\"bFrames\":" + std::to_string(frameTypeCount_[2]) + ",\"avgIKb\":" + std::to_string(avgKb(0)) +
           ",\"avgPKb\":" + std::to_string(avgKb(1)) + ",\"avgBKb\":" + std::to_string(avgKb(2)) +
           ",\"actualGop\":" + std::to_string(lastGopFrames_) +
           ",\"encoderLatencyMs\":" + std::to_string(encoderLatencyMs_) +
           ",\"encoderLatencyPeakMs\":" + std::to_string(encoderLatencyPeak_.peak(bootNs())) +
           ",\"encoderLatencyMaxMs\":" + std::to_string(encoderLatencyPeak_.max()) +
           ",\"firstVideoPtsUs\":" + std::to_string(firstVideoPtsUs_) +
           ",\"lastVideoPtsUs\":" + std::to_string(lastVideoPtsUs_) +
           ",\"firstAudioPtsUs\":" + std::to_string(firstAudioPtsUs_) +
           ",\"lastAudioPtsUs\":" + std::to_string(lastAudioPtsUs_) +
           ",\"audioRmsDbfs\":" + std::to_string(audioRmsDbfs) + ",\"audioPeak\":" + std::to_string(audioPeak_) +
           ",\"audioXruns\":" + std::to_string(audioXruns_) + ",\"metadata\":\"" + escaped(metadataStatus_) +
           "\",\"failure\":\"" + escaped(failure_) + "\"}";
}

}  // namespace rawrcam::video
