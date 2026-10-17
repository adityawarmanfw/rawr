#include "video/MovMuxer.h"

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace rawrcam::video {
namespace {
using Bytes = std::vector<uint8_t>;
constexpr size_t kFlushBytes = 1 << 20;
// About 6 s at 100 Mbps. Past this the producer waits: a stalled card slows
// encoding instead of growing memory without bound.
constexpr size_t kMaxQueuedBytes = 96u << 20;
constexpr uint32_t kMovieTimescale = 1000;

void put8(Bytes& out, uint32_t v) { out.push_back(static_cast<uint8_t>(v)); }
void put16(Bytes& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}
void put32(Bytes& out, uint32_t v) {
    put16(out, v >> 16);
    put16(out, v & 0xffff);
}
void put64(Bytes& out, uint64_t v) {
    put32(out, static_cast<uint32_t>(v >> 32));
    put32(out, static_cast<uint32_t>(v));
}
void putType(Bytes& out, const char* type) { out.insert(out.end(), type, type + 4); }
void append(Bytes& out, const Bytes& more) { out.insert(out.end(), more.begin(), more.end()); }
void zeros(Bytes& out, size_t count) { out.insert(out.end(), count, 0); }

Bytes box(const char* type, const Bytes& payload) {
    Bytes out;
    put32(out, static_cast<uint32_t>(payload.size() + 8));
    putType(out, type);
    append(out, payload);
    return out;
}
Bytes fullBox(const char* type, uint8_t version, uint32_t flags, const Bytes& payload) {
    Bytes body;
    put32(body, (uint32_t(version) << 24) | (flags & 0xffffff));
    append(body, payload);
    return box(type, body);
}
template <typename... Children>
Bytes container(const char* type, const Children&... children) {
    Bytes body;
    (append(body, children), ...);
    return box(type, body);
}

int64_t ticks(int64_t us, uint32_t timescale) {
    const int64_t scaled = us * static_cast<int64_t>(timescale);
    return (scaled >= 0 ? scaled + 500'000 : scaled - 500'000) / 1'000'000;
}
uint64_t rescale(uint64_t value, uint32_t from, uint32_t to) { return (value * to + from / 2) / from; }

void putMatrix(Bytes& out, int rotationDegrees) {
    // a b u / c d v / x y w, 16.16 except u, v, w (2.30). MPEG4Writer's rotation convention.
    uint32_t a = 0x10000, b = 0, c = 0, d = 0x10000;
    switch (((rotationDegrees % 360) + 360) % 360) {
        case 90: a = 0; b = 0x10000; c = 0xFFFF0000; d = 0; break;
        case 180: a = 0xFFFF0000; d = 0xFFFF0000; break;
        case 270: a = 0; b = 0xFFFF0000; c = 0x10000; d = 0; break;
        default: break;
    }
    for (uint32_t v : {a, b, 0u, c, d, 0u, 0u, 0u, 0x40000000u}) put32(out, v);
}

// Splits Annex B into NAL payloads (without start codes or trailing zero bytes).
std::vector<std::pair<const uint8_t*, size_t>> annexBNals(const uint8_t* data, size_t size) {
    std::vector<std::pair<const uint8_t*, size_t>> nals;
    size_t start = SIZE_MAX;
    for (size_t i = 0; i + 2 < size; ++i) {
        if (data[i] != 0 || data[i + 1] != 0 || data[i + 2] != 1) continue;
        if (start != SIZE_MAX) {
            size_t end = i;
            while (end > start && data[end - 1] == 0) --end;
            if (end > start) nals.emplace_back(data + start, end - start);
        }
        start = i + 3;
        i += 2;
    }
    if (start != SIZE_MAX && start < size) nals.emplace_back(data + start, size - start);
    return nals;
}

class Bits {
public:
    explicit Bits(Bytes bytes) : bytes_(std::move(bytes)) {}
    uint32_t u(unsigned count) {
        uint32_t value = 0;
        for (unsigned i = 0; i < count; ++i) {
            if ((position_ >> 3) >= bytes_.size()) throw std::runtime_error("truncated HEVC SPS");
            value = (value << 1) | ((bytes_[position_ >> 3] >> (7 - (position_ & 7))) & 1u);
            ++position_;
        }
        return value;
    }
    uint32_t ue() {
        unsigned zeros = 0;
        while (u(1) == 0) {
            if (++zeros > 31) throw std::runtime_error("malformed HEVC SPS");
        }
        return (1u << zeros) - 1u + u(zeros);
    }
    void skip(unsigned count) {
        for (unsigned i = 0; i < count; ++i) u(1);
    }

private:
    Bytes bytes_;
    size_t position_ = 0;
};

Bytes unescape(const uint8_t* data, size_t size) {
    Bytes out;
    out.reserve(size);
    int zeros = 0;
    for (size_t i = 0; i < size; ++i) {
        if (zeros >= 2 && data[i] == 3) {
            zeros = 0;
            continue;
        }
        zeros = data[i] == 0 ? zeros + 1 : 0;
        out.push_back(data[i]);
    }
    return out;
}

// HEVCDecoderConfigurationRecord (ISO/IEC 14496-15 8.3.3) from Annex B VPS/SPS/PPS.
Bytes hvcc(const Bytes& annexB) {
    std::vector<std::pair<const uint8_t*, size_t>> arrays[3];
    for (const auto& nal : annexBNals(annexB.data(), annexB.size())) {
        const int type = (nal.first[0] >> 1) & 0x3f;
        if (type >= 32 && type <= 34 && nal.second >= 3) arrays[type - 32].push_back(nal);
    }
    if (arrays[0].empty() || arrays[1].empty() || arrays[2].empty())
        throw std::runtime_error("HEVC codec config missing VPS/SPS/PPS");
    const auto& spsNal = arrays[1].front();
    const Bytes sps = unescape(spsNal.first + 2, spsNal.second - 2);
    if (sps.size() < 13) throw std::runtime_error("HEVC SPS too short");
    Bits bits(sps);
    bits.u(4);
    const unsigned subLayers = bits.u(3);
    const unsigned nesting = bits.u(1);
    bits.skip(96);  // general profile_tier_level, copied byte-wise below
    std::vector<std::pair<bool, bool>> present(subLayers);
    for (auto& layer : present) layer = {bits.u(1) != 0, bits.u(1) != 0};
    if (subLayers > 0) bits.skip(2 * (8 - subLayers));
    for (const auto& layer : present) {
        if (layer.first) bits.skip(88);
        if (layer.second) bits.skip(8);
    }
    bits.ue();  // sps_seq_parameter_set_id
    const uint32_t chroma = bits.ue();
    if (chroma == 3) bits.u(1);
    bits.ue();
    bits.ue();
    if (bits.u(1)) {
        for (int i = 0; i < 4; ++i) bits.ue();
    }
    const uint32_t lumaDepth = bits.ue() + 8;
    const uint32_t chromaDepth = bits.ue() + 8;

    Bytes out;
    put8(out, 1);
    out.insert(out.end(), sps.begin() + 1, sps.begin() + 13);  // general PTL: 12 bytes
    put16(out, 0xF000);
    put8(out, 0xFC);
    put8(out, 0xFC | chroma);
    put8(out, 0xF8 | (lumaDepth - 8));
    put8(out, 0xF8 | (chromaDepth - 8));
    put16(out, 0);  // avgFrameRate unspecified
    put8(out, ((subLayers + 1) << 3) | (nesting << 2) | 3);  // 4-byte NAL lengths
    put8(out, 3);
    for (int i = 0; i < 3; ++i) {
        put8(out, 0x80 | (32 + i));  // array_completeness
        put16(out, static_cast<uint32_t>(arrays[i].size()));
        for (const auto& nal : arrays[i]) {
            put16(out, static_cast<uint32_t>(nal.second));
            out.insert(out.end(), nal.first, nal.first + nal.second);
        }
    }
    return out;
}

// MediaFormat color keys to ISO/IEC 23091-2 code points.
Bytes colr(int standard, int transfer, int range) {
    uint16_t primaries = 2, matrix = 2, curve = 2;
    switch (standard) {
        case 1: primaries = 1; matrix = 1; break;  // BT.709
        case 2: primaries = 5; matrix = 5; break;  // BT.601 625
        case 4: primaries = 6; matrix = 6; break;  // BT.601 525
        case 6: primaries = 9; matrix = 9; break;  // BT.2020
        default: break;
    }
    switch (transfer) {
        case 1: curve = 8; break;   // linear
        case 3: curve = 1; break;   // SDR video (BT.709)
        case 6: curve = 16; break;  // ST 2084
        case 7: curve = 18; break;  // HLG
        default: break;
    }
    Bytes body;
    putType(body, "nclx");
    put16(body, primaries);
    put16(body, curve);
    put16(body, matrix);
    put8(body, range == 1 ? 0x80 : 0);
    return box("colr", body);
}

Bytes descriptor(uint8_t tag, const Bytes& payload) {
    Bytes out;
    put8(out, tag);
    const size_t n = payload.size();
    put8(out, 0x80 | ((n >> 21) & 0x7f));
    put8(out, 0x80 | ((n >> 14) & 0x7f));
    put8(out, 0x80 | ((n >> 7) & 0x7f));
    put8(out, n & 0x7f);
    append(out, payload);
    return out;
}

Bytes esds(const Bytes& config, int bitrate) {
    Bytes decoderConfig;
    put8(decoderConfig, 0x40);  // MPEG-4 Audio
    put8(decoderConfig, 0x15);  // AudioStream, upstream 0, reserved 1
    put8(decoderConfig, 0);
    put16(decoderConfig, 0);  // bufferSizeDB
    put32(decoderConfig, static_cast<uint32_t>(bitrate));
    put32(decoderConfig, static_cast<uint32_t>(bitrate));
    append(decoderConfig, descriptor(5, config));
    Bytes es;
    put16(es, 0);  // ES_ID
    put8(es, 0);
    append(es, descriptor(4, decoderConfig));
    append(es, descriptor(6, Bytes{2}));
    return fullBox("esds", 0, 0, descriptor(3, es));
}

// Run-length (count, delta) table.
Bytes runs(const char* type, const std::vector<int64_t>& values, uint8_t version = 0) {
    Bytes body;
    std::vector<std::pair<uint32_t, int64_t>> entries;
    for (int64_t v : values) {
        if (!entries.empty() && entries.back().second == v) ++entries.back().first;
        else entries.emplace_back(1, v);
    }
    put32(body, static_cast<uint32_t>(entries.size()));
    for (const auto& [count, value] : entries) {
        put32(body, count);
        put32(body, static_cast<uint32_t>(value));
    }
    return fullBox(type, version, 0, body);
}

Bytes hdlr(const char* handler, const char* name, bool quickTime) {
    Bytes body;
    if (quickTime) putType(body, "mhlr");
    else put32(body, 0);
    putType(body, handler);
    zeros(body, 12);
    if (quickTime) put8(body, static_cast<uint32_t>(std::strlen(name)));
    body.insert(body.end(), name, name + std::strlen(name));
    if (!quickTime) put8(body, 0);
    return fullBox("hdlr", 0, 0, body);
}

Bytes dinf() {
    Bytes dref;
    put32(dref, 1);
    append(dref, fullBox("url ", 0, 1, {}));
    return container("dinf", fullBox("dref", 0, 0, dref));
}

Bytes mdhd(uint32_t timescale, uint64_t duration) {
    Bytes body;
    put32(body, 0);
    put32(body, 0);
    put32(body, timescale);
    put32(body, static_cast<uint32_t>(duration));
    put16(body, 0x55C4);  // "und"
    put16(body, 0);
    return fullBox("mdhd", 0, 0, body);
}

Bytes tkhd(uint32_t id, uint64_t duration, bool audio, int rotation, int width, int height) {
    Bytes body;
    put32(body, 0);
    put32(body, 0);
    put32(body, id);
    put32(body, 0);
    put32(body, static_cast<uint32_t>(duration));
    zeros(body, 8);
    put16(body, 0);  // layer
    put16(body, audio ? 1 : 0);  // alternate group
    put16(body, audio ? 0x0100 : 0);
    put16(body, 0);
    putMatrix(body, audio ? 0 : rotation);
    put32(body, static_cast<uint32_t>(width) << 16);
    put32(body, static_cast<uint32_t>(height) << 16);
    return fullBox("tkhd", 0, 3, body);
}

// Optional leading empty edit (track starts late), then the media from mediaTime.
Bytes edts(uint64_t emptyMovie, uint64_t durationMovie, int64_t mediaTime) {
    Bytes body;
    put32(body, emptyMovie > 0 ? 2 : 1);
    if (emptyMovie > 0) {
        put32(body, static_cast<uint32_t>(emptyMovie));
        put32(body, 0xFFFFFFFF);
        put32(body, 0x00010000);
    }
    put32(body, static_cast<uint32_t>(durationMovie));
    put32(body, static_cast<uint32_t>(mediaTime));
    put32(body, 0x00010000);
    return container("edts", fullBox("elst", 0, 0, body));
}

Bytes stsc(const std::vector<uint32_t>& samplesPerChunk) {
    Bytes body;
    std::vector<std::pair<uint32_t, uint32_t>> entries;  // first chunk (1-based), samples
    for (size_t i = 0; i < samplesPerChunk.size(); ++i)
        if (entries.empty() || entries.back().second != samplesPerChunk[i])
            entries.emplace_back(static_cast<uint32_t>(i + 1), samplesPerChunk[i]);
    put32(body, static_cast<uint32_t>(entries.size()));
    for (const auto& [first, samples] : entries) {
        put32(body, first);
        put32(body, samples);
        put32(body, 1);
    }
    return fullBox("stsc", 0, 0, body);
}

Bytes co64(const std::vector<uint64_t>& offsets) {
    Bytes body;
    put32(body, static_cast<uint32_t>(offsets.size()));
    for (uint64_t offset : offsets) put64(body, offset);
    return fullBox("co64", 0, 0, body);
}
}  // namespace

MovMuxer::MovMuxer(int fd, Brand brand) : fd_(fd), brand_(brand) {
    if (fd_ < 0) throw std::invalid_argument("MovMuxer needs a writable fd");
    buffer_.reserve(kFlushBytes + (1 << 16));
    writer_ = std::thread([this] { writerLoop(); });
    Bytes ftyp;
    if (brand_ == Brand::QuickTime) {
        putType(ftyp, "qt  ");
        put32(ftyp, 0x200);
        putType(ftyp, "qt  ");
    } else {
        putType(ftyp, "isom");
        put32(ftyp, 0x200);
        for (const char* compatible : {"isom", "iso2", "mp41"}) putType(ftyp, compatible);
    }
    const Bytes header = box("ftyp", ftyp);
    write(header.data(), header.size());
    mdatStart_ = position_;
    Bytes mdat;
    put32(mdat, 1);  // 64-bit size follows; patched in finish()
    putType(mdat, "mdat");
    put64(mdat, 0);
    write(mdat.data(), mdat.size());
}

void MovMuxer::setVideo(const Video& video) {
    if (video_.present) throw std::logic_error("video track already set");
    if (video.width <= 0 || video.height <= 0 || video.fps <= 0) throw std::invalid_argument("invalid video track");
    (void)hvcc(video.annexBConfig);  // Validate now rather than at finish().
    videoConfig_ = video;
    video_.present = true;
    video_.timescale = static_cast<uint32_t>(video.fps) * 1000;
}

void MovMuxer::setAudio(const Audio& audio) {
    if (audio_.present) throw std::logic_error("audio track already set");
    if (audio.sampleRate <= 0 || audio.channels < 1 || audio.channels > 8) throw std::invalid_argument("invalid audio track");
    if (audio.codec == Audio::Codec::Aac && audio.aacConfig.empty()) throw std::invalid_argument("AAC config missing");
    if (audio.codec == Audio::Codec::Pcm &&
        !(audio.bitsPerSample == 16 || audio.bitsPerSample == 24 || audio.bitsPerSample == 32))
        throw std::invalid_argument("unsupported PCM sample size");
    audioConfig_ = audio;
    audio_.present = true;
    audio_.timescale = static_cast<uint32_t>(audio.sampleRate);
}

void MovMuxer::write(const uint8_t* data, size_t size) {
    buffer_.insert(buffer_.end(), data, data + size);
    position_ += size;
    if (buffer_.size() >= kFlushBytes) flush();
}

MovMuxer::~MovMuxer() {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        stopWriter_ = true;
    }
    queueChanged_.notify_all();
    if (writer_.joinable()) writer_.join();
}

void MovMuxer::flush() {
    if (buffer_.empty()) return;
    std::vector<uint8_t> next;
    next.reserve(kFlushBytes + (1 << 16));
    next.swap(buffer_);
    std::unique_lock<std::mutex> lock(queueMutex_);
    queueChanged_.wait(lock, [this] { return queuedBytes_ < kMaxQueuedBytes || !writeError_.empty(); });
    if (!writeError_.empty()) throw std::runtime_error(writeError_);
    queuedBytes_ += next.size();
    queue_.push_back(std::move(next));
    lock.unlock();
    queueChanged_.notify_all();
}

void MovMuxer::writerLoop() noexcept {
    std::unique_lock<std::mutex> lock(queueMutex_);
    for (;;) {
        queueChanged_.wait(lock, [this] { return !queue_.empty() || stopWriter_; });
        if (queue_.empty()) return;
        std::vector<uint8_t> chunk = std::move(queue_.front());
        queue_.pop_front();
        lock.unlock();
        std::string error;
        size_t done = 0;
        while (done < chunk.size()) {
            const ssize_t n = ::write(fd_, chunk.data() + done, chunk.size() - done);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                error = "MOV write failed errno=" + std::to_string(errno);
                break;
            }
            done += static_cast<size_t>(n);
        }
        lock.lock();
        queuedBytes_ -= chunk.size();
        if (!error.empty() && writeError_.empty()) writeError_ = error;
        queueChanged_.notify_all();
    }
}

void MovMuxer::throwIfWriteFailed() {
    std::lock_guard<std::mutex> lock(queueMutex_);
    if (!writeError_.empty()) throw std::runtime_error(writeError_);
}

void MovMuxer::writeVideo(const uint8_t* data, size_t size, int64_t ptsUs, bool sync) {
    if (!video_.present || finished_) throw std::logic_error("video track not writable");
    throwIfWriteFailed();
    const uint64_t offset = position_;
    uint32_t written = 0;
    for (const auto& [nal, length] : annexBNals(data, size)) {
        uint8_t prefix[4] = {static_cast<uint8_t>(length >> 24), static_cast<uint8_t>(length >> 16),
                             static_cast<uint8_t>(length >> 8), static_cast<uint8_t>(length)};
        write(prefix, 4);
        write(nal, length);
        written += static_cast<uint32_t>(4 + length);
    }
    if (written == 0) return;
    video_.chunks.push_back({offset, 1});
    video_.sizes.push_back(written);
    video_.ptsTicks.push_back(ticks(ptsUs, video_.timescale));
    if (sync) video_.syncSamples.push_back(static_cast<uint32_t>(video_.sizes.size()));
}

void MovMuxer::writeAudio(const uint8_t* data, size_t size, int64_t ptsUs) {
    if (!audio_.present || finished_) throw std::logic_error("audio track not writable");
    if (size == 0) return;
    throwIfWriteFailed();
    const uint64_t offset = position_;
    if (audioConfig_.codec == Audio::Codec::Pcm) {
        const size_t frameBytes = static_cast<size_t>(audioConfig_.channels * audioConfig_.bitsPerSample / 8);
        if (size % frameBytes != 0) throw std::invalid_argument("partial PCM frame");
        write(data, size);
        const auto frames = static_cast<uint32_t>(size / frameBytes);
        audio_.chunks.push_back({offset, frames});
        audio_.ptsTicks.push_back(ticks(ptsUs, audio_.timescale));
        audio_.pcmFrames += frames;
        return;
    }
    write(data, size);
    audio_.chunks.push_back({offset, 1});
    audio_.sizes.push_back(static_cast<uint32_t>(size));
    audio_.ptsTicks.push_back(ticks(ptsUs, audio_.timescale));
}

std::vector<uint8_t> MovMuxer::videoTrak(uint32_t movieTimescale) const {
    const auto& pts = video_.ptsTicks;
    const size_t n = pts.size();
    // Decode times: sorted PTS shifted so no sample decodes after it presents (B-frames).
    std::vector<int64_t> sorted(pts);
    std::sort(sorted.begin(), sorted.end());
    int64_t delay = 0;
    for (size_t i = 0; i < n; ++i) delay = std::max(delay, sorted[i] - pts[i]);
    const int64_t frameTicks = 1000;
    std::vector<int64_t> deltas(n), offsets(n);
    bool reordered = false;
    for (size_t i = 0; i < n; ++i) {
        const int64_t dts = sorted[i] - delay;
        deltas[i] = i + 1 < n ? std::max<int64_t>(1, sorted[i + 1] - sorted[i]) : frameTicks;
        offsets[i] = pts[i] - dts;
        reordered |= offsets[i] != 0;
    }
    uint64_t mediaDuration = 0;
    for (int64_t d : deltas) mediaDuration += static_cast<uint64_t>(d);
    // The edit starts at the first presented frame (media time `delay`) and runs the
    // full presentation span: first PTS to last PTS plus one frame, i.e. mediaDuration.
    const uint64_t emptyMovie = rescale(static_cast<uint64_t>(std::max<int64_t>(0, sorted.front())), video_.timescale, movieTimescale);
    const uint64_t durationMovie = rescale(mediaDuration, video_.timescale, movieTimescale);

    Bytes entry;
    zeros(entry, 6);
    put16(entry, 1);
    zeros(entry, 16);
    put16(entry, static_cast<uint32_t>(videoConfig_.width));
    put16(entry, static_cast<uint32_t>(videoConfig_.height));
    put32(entry, 0x00480000);
    put32(entry, 0x00480000);
    put32(entry, 0);
    put16(entry, 1);
    Bytes name(32, 0);
    const char kName[] = "HEVC Coding";
    name[0] = sizeof(kName) - 1;
    std::memcpy(name.data() + 1, kName, sizeof(kName) - 1);
    append(entry, name);
    put16(entry, 0x18);
    put16(entry, 0xFFFF);
    append(entry, box("hvcC", hvcc(videoConfig_.annexBConfig)));
    if (videoConfig_.colorStandard > 0 || videoConfig_.colorTransfer > 0)
        append(entry, colr(videoConfig_.colorStandard, videoConfig_.colorTransfer, videoConfig_.colorRange));
    Bytes stsd;
    put32(stsd, 1);
    append(stsd, box("hvc1", entry));

    Bytes stss;
    put32(stss, static_cast<uint32_t>(video_.syncSamples.size()));
    for (uint32_t index : video_.syncSamples) put32(stss, index);
    Bytes stsz;
    put32(stsz, 0);
    put32(stsz, static_cast<uint32_t>(n));
    for (uint32_t size : video_.sizes) put32(stsz, size);
    std::vector<uint32_t> perChunk;
    std::vector<uint64_t> chunkOffsets;
    for (const auto& chunk : video_.chunks) {
        perChunk.push_back(chunk.samples);
        chunkOffsets.push_back(chunk.offset);
    }
    Bytes stbl = fullBox("stsd", 0, 0, stsd);
    append(stbl, runs("stts", deltas));
    if (reordered) append(stbl, runs("ctts", offsets));
    if (video_.syncSamples.size() < n) append(stbl, fullBox("stss", 0, 0, stss));
    append(stbl, stsc(perChunk));
    append(stbl, fullBox("stsz", 0, 0, stsz));
    append(stbl, co64(chunkOffsets));

    Bytes vmhd;
    zeros(vmhd, 8);
    const bool quickTime = brand_ == Brand::QuickTime;
    return container("trak", tkhd(1, emptyMovie + durationMovie, false, videoConfig_.rotationDegrees, videoConfig_.width,
                                  videoConfig_.height),
                     edts(emptyMovie, durationMovie, delay),
                     container("mdia", mdhd(video_.timescale, mediaDuration), hdlr("vide", "VideoHandle", quickTime),
                               container("minf", fullBox("vmhd", 0, 1, vmhd), dinf(), box("stbl", stbl))));
}

std::vector<uint8_t> MovMuxer::audioTrak(uint32_t movieTimescale) const {
    const bool pcm = audioConfig_.codec == Audio::Codec::Pcm;
    const auto& pts = audio_.ptsTicks;
    std::vector<int64_t> deltas;
    uint64_t mediaDuration = 0;
    if (pcm) {
        mediaDuration = audio_.pcmFrames;
    } else {
        deltas.resize(pts.size());
        for (size_t i = 0; i < pts.size(); ++i)
            deltas[i] = i + 1 < pts.size() ? std::max<int64_t>(1, pts[i + 1] - pts[i]) : 1024;
        for (int64_t d : deltas) mediaDuration += static_cast<uint64_t>(d);
    }
    const uint64_t emptyMovie = rescale(static_cast<uint64_t>(std::max<int64_t>(0, pts.front())), audio_.timescale, movieTimescale);
    const uint64_t durationMovie = rescale(mediaDuration, audio_.timescale, movieTimescale);

    Bytes entry;
    zeros(entry, 6);
    put16(entry, 1);
    const char* format = "mp4a";
    if (!pcm) {
        zeros(entry, 8);
        put16(entry, static_cast<uint32_t>(audioConfig_.channels));
        put16(entry, 16);
        put32(entry, 0);
        put32(entry, static_cast<uint32_t>(audioConfig_.sampleRate) << 16);
        append(entry, esds(audioConfig_.aacConfig, audioConfig_.bitrate));
    } else {
        throw std::logic_error("PCM sample description not implemented");
    }
    Bytes stsd;
    put32(stsd, 1);
    append(stsd, box(format, entry));

    Bytes stsz;
    std::vector<uint32_t> perChunk;
    std::vector<uint64_t> chunkOffsets;
    for (const auto& chunk : audio_.chunks) {
        perChunk.push_back(chunk.samples);
        chunkOffsets.push_back(chunk.offset);
    }
    put32(stsz, 0);
    put32(stsz, static_cast<uint32_t>(audio_.sizes.size()));
    for (uint32_t size : audio_.sizes) put32(stsz, size);
    Bytes stbl = fullBox("stsd", 0, 0, stsd);
    append(stbl, runs("stts", deltas));
    append(stbl, stsc(perChunk));
    append(stbl, fullBox("stsz", 0, 0, stsz));
    append(stbl, co64(chunkOffsets));

    Bytes smhd;
    zeros(smhd, 4);
    const bool quickTime = brand_ == Brand::QuickTime;
    return container("trak", tkhd(2, emptyMovie + durationMovie, true, 0, 0, 0), edts(emptyMovie, durationMovie, 0),
                     container("mdia", mdhd(audio_.timescale, mediaDuration), hdlr("soun", "SoundHandle", quickTime),
                               container("minf", fullBox("smhd", 0, 0, smhd), dinf(), box("stbl", stbl))));
}

std::vector<uint8_t> MovMuxer::moov() const {
    const Bytes video = videoTrak(kMovieTimescale);
    const Bytes audio = audioTrak(kMovieTimescale);
    // tkhd duration sits at a fixed offset: trak(8) + tkhd header(12) + 4 + 4 + 4 + 4.
    const auto trackDuration = [](const Bytes& trak) {
        const uint8_t* p = trak.data() + 8 + 12 + 16;
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    };
    Bytes mvhd;
    put32(mvhd, 0);
    put32(mvhd, 0);
    put32(mvhd, kMovieTimescale);
    put32(mvhd, std::max(trackDuration(video), trackDuration(audio)));
    put32(mvhd, 0x00010000);
    put16(mvhd, 0x0100);
    zeros(mvhd, 10);
    putMatrix(mvhd, 0);
    zeros(mvhd, 24);
    put32(mvhd, 3);
    return container("moov", fullBox("mvhd", 0, 0, mvhd), video, audio);
}

void MovMuxer::finish() {
    if (finished_) return;
    if (!video_.present || video_.sizes.empty()) throw std::runtime_error("MOV has no video samples");
    if (!audio_.present || audio_.chunks.empty()) throw std::runtime_error("MOV has no audio samples");
    finished_ = true;
    const uint64_t mdatEnd = position_;
    const Bytes index = moov();
    write(index.data(), index.size());
    flush();
    {
        std::unique_lock<std::mutex> lock(queueMutex_);
        queueChanged_.wait(lock, [this] { return queue_.empty() && queuedBytes_ == 0; });
    }
    throwIfWriteFailed();
    uint8_t size[8];
    const uint64_t mdatSize = mdatEnd - mdatStart_;
    for (int i = 0; i < 8; ++i) size[i] = static_cast<uint8_t>(mdatSize >> (56 - 8 * i));
    if (::pwrite(fd_, size, sizeof(size), static_cast<off_t>(mdatStart_ + 8)) != static_cast<ssize_t>(sizeof(size)))
        throw std::runtime_error("MOV mdat size write failed errno=" + std::to_string(errno));
}

}  // namespace rawrcam::video
