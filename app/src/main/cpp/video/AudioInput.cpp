#include "video/AudioInput.h"

#include <android/log.h>
#include <time.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace rawrcam::video {
namespace {
constexpr char kTag[] = "RawrAudioInput";
// ~300 ms RMS integration at 1024-frame (21 ms) blocks.
constexpr float kRmsSmoothing = 0.07f;

int64_t bootNs() noexcept {
    timespec ts{};
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}

std::mutex registryMutex;
std::weak_ptr<AudioInput> registry;
}  // namespace

std::shared_ptr<AudioInput> AudioInput::acquire(int channels) {
    std::lock_guard<std::mutex> lock(registryMutex);
    if (auto existing = registry.lock()) return existing;
    std::shared_ptr<AudioInput> created(new AudioInput(channels));
    registry = created;
    return created;
}

AudioInput::AudioInput(int requestedChannels) {
    if (requestedChannels != 1 && requestedChannels != 2) throw std::invalid_argument("microphone channels must be 1 or 2");
    for (auto& value : peak_) value.store(0.0f);
    for (auto& value : rms_) value.store(0.0f);
    // Stereo-first with single mono fallback. Never fails for topology.
    for (int tryChannels : {requestedChannels, 1}) {
        AAudioStreamBuilder* builder = nullptr;
        if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK || !builder)
            throw std::runtime_error("create microphone builder failed");
        AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_INPUT);
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
        AAudioStreamBuilder_setSampleRate(builder, kSampleRate);
        AAudioStreamBuilder_setChannelCount(builder, tryChannels);
        AAudioStreamBuilder_setInputPreset(builder, AAUDIO_INPUT_PRESET_CAMCORDER);
        AAudioStream* stream = nullptr;
        const aaudio_result_t opened = AAudioStreamBuilder_openStream(builder, &stream);
        AAudioStreamBuilder_delete(builder);
        const bool matches = opened == AAUDIO_OK && stream && AAudioStream_getSampleRate(stream) == kSampleRate &&
                             AAudioStream_getChannelCount(stream) == tryChannels &&
                             AAudioStream_getFormat(stream) == AAUDIO_FORMAT_PCM_I16;
        if (matches) {
            stream_ = stream;
            channels_ = tryChannels;
            break;
        }
        if (stream) AAudioStream_close(stream);
        if (tryChannels == 1) break;
    }
    if (!stream_) throw std::runtime_error("microphone did not provide 48 kHz PCM16");
    if (AAudioStream_requestStart(stream_) != AAUDIO_OK) {
        AAudioStream_close(stream_);
        throw std::runtime_error("start microphone failed");
    }
    running_.store(true);
    thread_ = std::thread([this] { run(); });
}

AudioInput::~AudioInput() {
    running_.store(false);
    (void)AAudioStream_requestStop(stream_);
    if (thread_.joinable()) thread_.join();
    (void)AAudioStream_close(stream_);
}

void AudioInput::setSink(Sink sink) {
    std::lock_guard<std::mutex> lock(sinkMutex_);
    sink_ = std::move(sink);
}

AudioInput::Levels AudioInput::takeLevels() noexcept {
    Levels levels;
    levels.channels = channels_;
    for (int c = 0; c < channels_; ++c) {
        levels.peak[c] = peak_[c].exchange(0.0f);
        levels.rms[c] = rms_[c].load();
    }
    levels.clips = clips_.load();
    levels.failed = failed_.load();
    return levels;
}

void AudioInput::run() noexcept {
    std::array<int16_t, kReadFrames * kMaxChannels> pcm{};
    int64_t totalFrames = 0;
    int64_t anchorNs = 0;
    std::string failure;
    while (running_.load()) {
        const aaudio_result_t count = AAudioStream_read(stream_, pcm.data(), kReadFrames, 50'000'000);
        if (count < 0) {
            if (running_.load()) failure = "microphone read failed: " + std::to_string(count);
            break;
        }
        if (count == 0) continue;
        int64_t timestampFrame = 0, timestampNs = 0;
        int64_t measuredFirstNs = bootNs() - static_cast<int64_t>(count) * 1'000'000'000LL / kSampleRate;
        if (AAudioStream_getTimestamp(stream_, CLOCK_BOOTTIME, &timestampFrame, &timestampNs) == AAUDIO_OK)
            measuredFirstNs = timestampNs + (totalFrames - timestampFrame) * 1'000'000'000LL / kSampleRate;
        const int64_t measuredAnchor = measuredFirstNs - totalFrames * 1'000'000'000LL / kSampleRate;
        anchorNs = anchorNs == 0 ? measuredAnchor
                                 : anchorNs + std::clamp<int64_t>(measuredAnchor - anchorNs, -1'000'000, 1'000'000);

        float squares[kMaxChannels] = {};
        int32_t peaks[kMaxChannels] = {};
        bool clipped = false;
        for (int frame = 0; frame < count; ++frame) {
            for (int c = 0; c < channels_; ++c) {
                const int32_t value = pcm[static_cast<size_t>(frame * channels_ + c)];
                const int32_t magnitude = std::abs(value);
                peaks[c] = std::max(peaks[c], magnitude);
                squares[c] += static_cast<float>(value) * static_cast<float>(value);
                clipped |= magnitude >= 32'767;
            }
        }
        for (int c = 0; c < channels_; ++c) {
            const float peak = static_cast<float>(peaks[c]) / 32'768.0f;
            float held = peak_[c].load();
            while (peak > held && !peak_[c].compare_exchange_weak(held, peak)) {
            }
            const float blockRms = std::sqrt(squares[c] / static_cast<float>(count)) / 32'768.0f;
            const float previous = rms_[c].load();
            rms_[c].store(previous + (blockRms - previous) * kRmsSmoothing);
        }
        if (clipped) clips_.fetch_add(1);
        xruns_.store(AAudioStream_getXRunCount(stream_));

        Block block{pcm.data(), static_cast<int>(count), anchorNs + totalFrames * 1'000'000'000LL / kSampleRate};
        totalFrames += count;
        std::lock_guard<std::mutex> lock(sinkMutex_);
        if (sink_.onBlock) sink_.onBlock(block);
    }
    if (failure.empty()) return;
    failed_.store(true);
    __android_log_print(ANDROID_LOG_ERROR, kTag, "%s", failure.c_str());
    std::lock_guard<std::mutex> lock(sinkMutex_);
    if (sink_.onError) sink_.onError(failure);
}

}  // namespace rawrcam::video
