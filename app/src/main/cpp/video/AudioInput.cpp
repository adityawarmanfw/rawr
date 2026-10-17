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
// Largest 16-bit sample; anything at or past it reads as a clip at every bit depth.
constexpr float kFullScale = 32'767.0f / 32'768.0f;

int64_t bootNs() noexcept {
    timespec ts{};
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}

std::mutex registryMutex;
std::weak_ptr<AudioInput> registry;

// RBJ second-order Butterworth high-pass, run per channel in transposed direct form II.
struct HighPass {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1[AudioInput::kMaxChannels] = {}, z2[AudioInput::kMaxChannels] = {};
    int cornerHz = 0;

    void design(int hz) {
        *this = HighPass{};
        cornerHz = hz;
        if (hz <= 0) return;
        const float w = 2.0f * static_cast<float>(M_PI) * static_cast<float>(hz) / AudioInput::kSampleRate;
        const float alpha = std::sin(w) / (2.0f * static_cast<float>(M_SQRT1_2));
        const float cosw = std::cos(w);
        const float a0 = 1.0f + alpha;
        b0 = (1.0f + cosw) / 2.0f / a0;
        b1 = -(1.0f + cosw) / a0;
        b2 = b0;
        a1 = -2.0f * cosw / a0;
        a2 = (1.0f - alpha) / a0;
    }
    float run(int c, float x) {
        if (cornerHz <= 0) return x;
        const float y = b0 * x + z1[c];
        z1[c] = b1 * x - a1 * y + z2[c];
        z2[c] = b2 * x - a2 * y;
        return y;
    }
};
}  // namespace

std::shared_ptr<AudioInput> AudioInput::acquire(const Config& config) {
    std::lock_guard<std::mutex> lock(registryMutex);
    if (auto existing = registry.lock()) return existing;
    std::shared_ptr<AudioInput> created(new AudioInput(config));
    registry = created;
    return created;
}

AudioInput::AudioInput(const Config& config) : config_(config) {
    const int requestedChannels = config.channels;
    if (requestedChannels != 1 && requestedChannels != 2) throw std::invalid_argument("microphone channels must be 1 or 2");
    for (auto& value : peak_) value.store(0.0f);
    for (auto& value : rms_) value.store(0.0f);
    // Stereo-first with single mono fallback. Never fails for topology.
    for (int tryChannels : {requestedChannels, 1}) {
        AAudioStreamBuilder* builder = nullptr;
        if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK || !builder)
            throw std::runtime_error("create microphone builder failed");
        AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_INPUT);
        // Float keeps the HAL's precision past 16 bits for 24-bit PCM recording.
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
        AAudioStreamBuilder_setSampleRate(builder, kSampleRate);
        AAudioStreamBuilder_setChannelCount(builder, tryChannels);
        AAudioStreamBuilder_setInputPreset(builder, config.preset);
        if (config.deviceId > 0) AAudioStreamBuilder_setDeviceId(builder, config.deviceId);
        AAudioStream* stream = nullptr;
        const aaudio_result_t opened = AAudioStreamBuilder_openStream(builder, &stream);
        AAudioStreamBuilder_delete(builder);
        const bool matches = opened == AAUDIO_OK && stream && AAudioStream_getSampleRate(stream) == kSampleRate &&
                             AAudioStream_getChannelCount(stream) == tryChannels &&
                             AAudioStream_getFormat(stream) == AAUDIO_FORMAT_PCM_FLOAT;
        if (matches) {
            stream_ = stream;
            channels_ = tryChannels;
            break;
        }
        if (stream) AAudioStream_close(stream);
        if (tryChannels == 1) break;
    }
    if (!stream_) throw std::runtime_error("microphone did not provide 48 kHz float PCM");
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

void AudioInput::setProcessing(float gainDb, int lowCutHz) noexcept {
    gainDb_.store(std::clamp(gainDb, -24.0f, 36.0f));
    lowCutHz_.store(std::clamp(lowCutHz, 0, 300));
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
    levels.inputClips = inputClips_.load();
    levels.outputClips = outputClips_.load();
    levels.failed = failed_.load();
    return levels;
}

void AudioInput::run() noexcept {
    std::array<float, kReadFrames * kMaxChannels> pcm{};
    int64_t totalFrames = 0;
    int64_t anchorNs = 0;
    HighPass filter;
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

        if (filter.cornerHz != lowCutHz_.load()) filter.design(lowCutHz_.load());
        const float gain = std::pow(10.0f, gainDb_.load() / 20.0f);
        float squares[kMaxChannels] = {};
        float peaks[kMaxChannels] = {};
        bool inputClipped = false, outputClipped = false;
        for (int frame = 0; frame < count; ++frame) {
            for (int c = 0; c < channels_; ++c) {
                float& sample = pcm[static_cast<size_t>(frame * channels_ + c)];
                inputClipped |= std::abs(sample) >= kFullScale;
                float y = filter.run(c, sample) * gain;
                if (std::abs(y) >= kFullScale) {
                    outputClipped = true;
                    y = std::copysign(kFullScale, y);
                }
                sample = y;
                peaks[c] = std::max(peaks[c], std::abs(y));
                squares[c] += y * y;
            }
        }
        for (int c = 0; c < channels_; ++c) {
            float held = peak_[c].load();
            while (peaks[c] > held && !peak_[c].compare_exchange_weak(held, peaks[c])) {
            }
            const float blockRms = std::sqrt(squares[c] / static_cast<float>(count));
            const float previous = rms_[c].load();
            rms_[c].store(previous + (blockRms - previous) * kRmsSmoothing);
        }
        if (inputClipped) inputClips_.fetch_add(1);
        else if (outputClipped) outputClips_.fetch_add(1);
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
