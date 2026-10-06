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
constexpr int kSampleRate = 48'000;
constexpr int kMaxAudioChannels = 2;
constexpr int kMonoFallbackBitrate = 128'000;
constexpr int kAudioReadFrames = 1024;

void requireMedia(media_status_t status, const char* operation) {
    if (status != AMEDIA_OK) throw std::runtime_error(std::string(operation) + " status=" + std::to_string(status));
}
void requireAudio(aaudio_result_t status, const char* operation) {
    if (status != AAUDIO_OK) throw std::runtime_error(std::string(operation) + " status=" + std::to_string(status));
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
        settings_.bitrate > 400'000'000 || settings_.intraSeconds < 1 || settings_.intraSeconds > 10 ||
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
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, settings_.intraSeconds);
    if (settings_.bitrateMode >= 0) AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_BITRATE_MODE, settings_.bitrateMode);
    // Optional codec key. A codec may reject the request during configure;
    // omit it when left at the automatic default.
    if (settings_.maxBFrames >= 0) AMediaFormat_setInt32(format, "max-bframes", settings_.maxBFrames);
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
    int openedChannels = 0;
    // Stereo-first with single mono fallback. Never fails the recording for topology.
    for (int attempt = 0; attempt < 2; ++attempt) {
        const int tryChannels = (attempt == 0) ? requestedChannels : 1;
        if (attempt == 1 && requestedChannels == 1) break;
        AAudioStreamBuilder* builder = nullptr;
        requireAudio(AAudio_createStreamBuilder(&builder), "create microphone builder");
        if (!builder) throw std::runtime_error("microphone builder is null");
        AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_INPUT);
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
        AAudioStreamBuilder_setSampleRate(builder, kSampleRate);
        AAudioStreamBuilder_setChannelCount(builder, tryChannels);
        AAudioStreamBuilder_setInputPreset(builder, AAUDIO_INPUT_PRESET_CAMCORDER);
        AAudioStream* stream = nullptr;
        const aaudio_result_t opened = AAudioStreamBuilder_openStream(builder, &stream);
        AAudioStreamBuilder_delete(builder);
        if (opened != AAUDIO_OK || !stream) {
            if (stream) AAudioStream_close(stream);
            if (attempt == 0 && requestedChannels != 1) continue;
            requireAudio(opened, "open microphone");
            throw std::runtime_error("microphone open failed");
        }
        const bool matches = AAudioStream_getSampleRate(stream) == kSampleRate &&
                             AAudioStream_getChannelCount(stream) == tryChannels &&
                             AAudioStream_getFormat(stream) == AAUDIO_FORMAT_PCM_I16;
        if (!matches) {
            AAudioStream_close(stream);
            if (attempt == 0 && requestedChannels != 1) continue;
            throw std::runtime_error("microphone did not provide 48 kHz PCM16 at requested channels");
        }
        audioStream_ = stream;
        openedChannels = tryChannels;
        break;
    }
    if (!audioStream_ || openedChannels <= 0) throw std::runtime_error("microphone unavailable at requested channels");
    actualAudioChannels_ = openedChannels;
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
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE, kAudioReadFrames * actualAudioChannels_ * 2);
    const media_status_t configured =
        AMediaCodec_configure(audioCodec_, format, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    AMediaFormat_delete(format);
    requireMedia(configured, "configure AAC");
    requireMedia(AMediaCodec_start(audioCodec_), "start AAC");
    requireAudio(AAudioStream_requestStart(audioStream_), "start microphone");
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
        audioThread_ = std::thread([this] { captureAudio(); });
    } catch (...) {
        audioRunning_.store(false);
        stopRequested_.store(true);
        if (audioStream_) (void)AAudioStream_requestStop(audioStream_);
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
                    std::lock_guard<std::mutex> lock(statsMutex_);
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

void NativeAvRecorder::captureAudio() noexcept {
    try {
        const int channels = actualAudioChannels_ > 0 ? actualAudioChannels_ : settings_.audioChannels;
        std::array<int16_t, kAudioReadFrames * kMaxAudioChannels> pcm{};
        int64_t totalFrames = 0;
        int64_t anchorNs = 0;
        while (audioRunning_.load()) {
            const aaudio_result_t count = AAudioStream_read(audioStream_, pcm.data(), kAudioReadFrames, 50'000'000);
            if (count < 0) throw std::runtime_error("microphone read failed: " + std::to_string(count));
            if (count == 0) {
                drainAudio(false);
                continue;
            }
            int64_t timestampFrame = 0, timestampNs = 0;
            int64_t measuredFirstNs = bootNs() - static_cast<int64_t>(count) * 1'000'000'000LL / kSampleRate;
            if (AAudioStream_getTimestamp(audioStream_, CLOCK_BOOTTIME, &timestampFrame, &timestampNs) == AAUDIO_OK)
                measuredFirstNs = timestampNs + (totalFrames - timestampFrame) * 1'000'000'000LL / kSampleRate;
            const int64_t measuredAnchor = measuredFirstNs - totalFrames * 1'000'000'000LL / kSampleRate;
            anchorNs = anchorNs == 0 ? measuredAnchor
                                     : anchorNs + std::clamp<int64_t>(measuredAnchor - anchorNs, -1'000'000, 1'000'000);
            long double squares = 0;
            int32_t peak = 0;
            const int64_t totalSamples = static_cast<int64_t>(count) * channels;
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
                audioXruns_ = AAudioStream_getXRunCount(audioStream_);
            }
            int consumed = 0;
            while (consumed < count && audioRunning_.load()) {
                const ssize_t index = AMediaCodec_dequeueInputBuffer(audioCodec_, 10'000);
                if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
                    drainAudio(false);
                    continue;
                }
                if (index < 0) throw std::runtime_error("AAC input dequeue failed: " + std::to_string(index));
                size_t capacity = 0;
                uint8_t* input = AMediaCodec_getInputBuffer(audioCodec_, static_cast<size_t>(index), &capacity);
                const int part = std::min<int>(
                    count - consumed, static_cast<int>(capacity / (sizeof(int16_t) * static_cast<size_t>(channels))));
                if (!input || part <= 0) throw std::runtime_error("AAC input buffer unavailable");
                std::memcpy(input, pcm.data() + static_cast<size_t>(consumed) * static_cast<size_t>(channels),
                            static_cast<size_t>(part) * static_cast<size_t>(channels) * sizeof(int16_t));
                const int64_t ptsUs = (anchorNs + (totalFrames + consumed) * 1'000'000'000LL / kSampleRate) / 1'000;
                requireMedia(AMediaCodec_queueInputBuffer(
                                 audioCodec_, static_cast<size_t>(index), 0,
                                 static_cast<size_t>(part) * static_cast<size_t>(channels) * sizeof(int16_t),
                                 static_cast<uint64_t>(std::max<int64_t>(0, ptsUs)), 0),
                             "queue AAC PCM");
                consumed += part;
                drainAudio(false);
            }
            totalFrames += count;
        }
        const int64_t endUs = (anchorNs + totalFrames * 1'000'000'000LL / kSampleRate) / 1'000;
        for (;;) {
            const ssize_t index = AMediaCodec_dequeueInputBuffer(audioCodec_, 10'000);
            if (index >= 0) {
                requireMedia(AMediaCodec_queueInputBuffer(audioCodec_, static_cast<size_t>(index), 0, 0,
                                                          static_cast<uint64_t>(std::max<int64_t>(0, endUs)),
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
    audioRunning_.store(false);
    if (audioStream_) (void)AAudioStream_requestStop(audioStream_);
    if (audioThread_.joinable()) audioThread_.join();
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
    if (audioStream_) (void)AAudioStream_requestStop(audioStream_);
    if (audioThread_.joinable()) audioThread_.join();
    if (videoThread_.joinable()) videoThread_.join();
    if (audioStream_) {
        (void)AAudioStream_close(audioStream_);
        audioStream_ = nullptr;
    }
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
    const double audioRmsDbfs = rms <= 0.0 ? -120.0 : 20.0 * std::log10(rms / 32768.0);
    return std::string("{\"videoCodec\":\"") + escaped(videoCodecName_) +
           "\",\"width\":" + std::to_string(settings_.width) + ",\"height\":" + std::to_string(settings_.height) +
           ",\"requestedFps\":" + std::to_string(settings_.fps) + ",\"bitrate\":" + std::to_string(settings_.bitrate) +
           ",\"intraSeconds\":" + std::to_string(settings_.intraSeconds) +
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
