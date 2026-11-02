#include "StripJpegEncoder.h"

#include <sys/resource.h>
#include <turbojpeg.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

namespace rawrcam::encoding::jpeg {
namespace {

// Below this the strips' thread start-up costs more than it saves.
constexpr std::int64_t kMinPixels = 1'000'000;
constexpr unsigned kMaxThreads = 8;

struct Strip {
    int y = 0;
    int rows = 0;
    unsigned char* jpeg = nullptr;
    std::size_t size = 0;
    std::string error;
};

// Offsets inside one strip's JPEG: [0, sosStart) headers before SOS, [sosStart, scanStart) the SOS segment,
// [scanStart, size - 2) entropy-coded data, then EOI.
struct Layout {
    std::size_t sofHeight = 0;  // offset of SOF's 16-bit image height
    std::size_t sosStart = 0;
    std::size_t scanStart = 0;
};

std::uint16_t be16(const unsigned char* p) { return static_cast<std::uint16_t>((p[0] << 8) | p[1]); }

bool parse(const unsigned char* d, std::size_t n, Layout* out, std::string* error) {
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8 || d[n - 2] != 0xFF || d[n - 1] != 0xD9) {
        *error = "strip is not SOI..EOI";
        return false;
    }
    std::size_t p = 2;
    bool sof = false;
    while (p + 4 <= n) {
        if (d[p] != 0xFF) {
            *error = "strip header out of sync";
            return false;
        }
        const unsigned char marker = d[p + 1];
        const std::size_t len = be16(d + p + 2);
        if (len < 2 || p + 2 + len > n) {
            *error = "strip segment truncated";
            return false;
        }
        if (marker == 0xC0) {  // SOF0, baseline: FF C0 Lh Ll P Yh Yl ...
            if (len < 7) {
                *error = "strip SOF too short";
                return false;
            }
            out->sofHeight = p + 5;
            sof = true;
        } else if (marker == 0xDA) {
            out->sosStart = p;
            out->scanStart = p + 2 + len;
            if (!sof) {
                *error = "strip is not baseline (no SOF0 before SOS)";
                return false;
            }
            return true;
        } else if ((marker >= 0xC1 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) ||
                   marker == 0xDD) {
            *error = "strip uses an unexpected marker";  // progressive / arithmetic / a restart interval
            return false;
        }
        p += 2 + len;
    }
    *error = "strip has no SOS";
    return false;
}

// Headers of two strips equal except SOF's image height: same tables, same component layout.
bool sameHeaders(const unsigned char* a, const Layout& la, const unsigned char* b, const Layout& lb) {
    if (la.scanStart != lb.scanStart || la.sofHeight != lb.sofHeight) return false;
    return std::memcmp(a, b, la.sofHeight) == 0 &&
           std::memcmp(a + la.sofHeight + 2, b + lb.sofHeight + 2, la.scanStart - la.sofHeight - 2) == 0;
}

void encodeStrip(const unsigned char* rgba, int width, int quality, int subsamp, Strip* s) {
    tjhandle h = tj3Init(TJINIT_COMPRESS);
    if (!h) {
        s->error = "tj3Init failed";
        return;
    }
    if (tj3Set(h, TJPARAM_QUALITY, quality) != 0 || tj3Set(h, TJPARAM_SUBSAMP, subsamp) != 0 ||
        tj3Set(h, TJPARAM_FASTDCT, 0) != 0 || tj3Set(h, TJPARAM_OPTIMIZE, 0) != 0 ||
        tj3Set(h, TJPARAM_PROGRESSIVE, 0) != 0 || tj3Set(h, TJPARAM_ARITHMETIC, 0) != 0 ||
        tj3Set(h, TJPARAM_RESTARTBLOCKS, 0) != 0 || tj3Set(h, TJPARAM_RESTARTROWS, 0) != 0) {
        s->error = tj3GetErrorStr(h);
    } else if (tj3Compress8(h, rgba + static_cast<std::size_t>(s->y) * static_cast<std::size_t>(width) * 4u, width, 0,
                            s->rows, TJPF_RGBA, &s->jpeg, &s->size) != 0) {
        s->error = tj3GetErrorStr(h);
    }
    tj3Destroy(h);
}

}  // namespace

struct StripJpeg::Impl {
    std::vector<Strip> strips;
    std::vector<Layout> layouts;
    std::vector<unsigned char> header;
    std::size_t bytes = 0;
    ~Impl() {
        for (auto& strip : strips)
            if (strip.jpeg) tj3Free(strip.jpeg);
    }
};

StripJpeg::StripJpeg() = default;
StripJpeg::~StripJpeg() = default;
void StripJpeg::reset() noexcept { impl_.reset(); }
std::size_t StripJpeg::byteSize() const noexcept { return impl_ ? impl_->bytes : 0; }
int StripJpeg::stripsUsed() const noexcept { return impl_ ? static_cast<int>(impl_->strips.size()) : 0; }

bool StripJpeg::encodeRgba8(const unsigned char* rgba, int width, int height, int quality, int tjSubsamp,
                           std::string* error) {
    reset();
    if (!rgba || width <= 0 || height <= 0 || width > 65535 || height > 65535 ||
        quality < 1 || quality > 100 || tjSubsamp < 0 || tjSubsamp >= TJ_NUMSAMP) {
        *error = "invalid arguments";
        return false;
    }
    const unsigned threads = std::min(kMaxThreads, std::max(1u, std::thread::hardware_concurrency()));
    if (threads < 2 || static_cast<std::int64_t>(width) * height < kMinPixels) {
        *error = "image too small or one core";
        return false;
    }
    const int mcuW = tjMCUWidth[tjSubsamp], mcuH = tjMCUHeight[tjSubsamp];
    const std::int64_t mcusPerRow = (width + mcuW - 1) / mcuW;
    const int mcuRows = (height + mcuH - 1) / mcuH;
    // The restart interval (one strip's MCUs) is a 16-bit field.
    const int maxStripMcuRows = static_cast<int>(std::min<std::int64_t>(65535 / mcusPerRow, mcuRows));
    if (maxStripMcuRows < 1) {
        *error = "image too wide for a restart interval";
        return false;
    }
    const int stripMcuRows = std::min(maxStripMcuRows, (mcuRows + static_cast<int>(threads) - 1) / static_cast<int>(threads));
    const int stripCount = (mcuRows + stripMcuRows - 1) / stripMcuRows;
    if (stripCount < 2) {
        *error = "one strip";
        return false;
    }

    auto encoded = std::make_unique<Impl>();
    auto& strips = encoded->strips;
    strips.resize(static_cast<std::size_t>(stripCount));
    for (int i = 0; i < stripCount; ++i) {
        strips[i].y = i * stripMcuRows * mcuH;
        strips[i].rows = std::min(stripMcuRows * mcuH, height - strips[i].y);
    }
    std::atomic<int> next{0};
    auto work = [&]() {
        // Background priority, as the caller's encode thread: never preempt camera or preview work.
        ::setpriority(PRIO_PROCESS, 0, 10);
        for (int i = next.fetch_add(1); i < stripCount; i = next.fetch_add(1))
            encodeStrip(rgba, width, quality, tjSubsamp, &strips[i]);
    };
    {
        std::vector<std::thread> pool;
        const unsigned extra = std::min<unsigned>(threads, static_cast<unsigned>(stripCount)) - 1u;
        pool.reserve(extra);
        try {
            for (unsigned t = 0; t < extra; ++t) pool.emplace_back(work);
        } catch (...) {
            // Fewer threads: the strips still all get encoded below.
        }
        work();
        for (auto& t : pool) t.join();
    }

    bool ok = true;
    auto& layouts = encoded->layouts;
    layouts.resize(strips.size());
    for (std::size_t i = 0; ok && i < strips.size(); ++i) {
        if (!strips[i].error.empty() || !strips[i].jpeg) {
            *error = "strip " + std::to_string(i) + ": " + (strips[i].error.empty() ? "no output" : strips[i].error);
            ok = false;
        } else if (!parse(strips[i].jpeg, strips[i].size, &layouts[i], error)) {
            ok = false;
        } else if (i > 0 && !sameHeaders(strips[0].jpeg, layouts[0], strips[i].jpeg, layouts[i])) {
            *error = "strip " + std::to_string(i) + " headers differ from strip 0";
            ok = false;
        }
    }

    if (ok) {
        const Layout& l0 = layouts[0];
        std::size_t total = l0.scanStart + 6u + 2u;  // headers + DRI + EOI
        for (std::size_t i = 0; i < strips.size(); ++i) {
            const auto bytes = (strips[i].size - 2u - layouts[i].scanStart) + (i > 0 ? 2u : 0u);
            if (bytes > std::numeric_limits<std::size_t>::max() - total) {
                *error = "JPEG size overflow";
                return false;
            }
            total += bytes;
        }
        encoded->bytes = total;
        encoded->header.resize(l0.scanStart + 6u);
        {
            unsigned char* w = encoded->header.data();
            const unsigned char* s0 = strips[0].jpeg;
            std::memcpy(w, s0, l0.sosStart);
            w[l0.sofHeight] = static_cast<unsigned char>((height >> 8) & 0xFF);
            w[l0.sofHeight + 1] = static_cast<unsigned char>(height & 0xFF);
            w += l0.sosStart;
            const auto interval = static_cast<std::uint32_t>(mcusPerRow * stripMcuRows);
            const unsigned char dri[6] = {0xFF, 0xDD, 0x00, 0x04, static_cast<unsigned char>(interval >> 8),
                                          static_cast<unsigned char>(interval & 0xFF)};
            std::memcpy(w, dri, sizeof(dri));
            w += sizeof(dri);
            std::memcpy(w, s0 + l0.sosStart, l0.scanStart - l0.sosStart);
        }
        // Headers have been checked and strip 0's copied. Reuse two dead
        // header bytes before each later scan for its restart marker, so a
        // file sink receives one large chunk per strip rather than tiny writes.
        for (std::size_t i = 1; i < strips.size(); ++i) {
            auto* restart = strips[i].jpeg + layouts[i].scanStart - 2u;
            restart[0] = 0xFF;
            restart[1] = static_cast<unsigned char>(0xD0 + ((i - 1) & 7u));
        }
    }
    if (ok) impl_ = std::move(encoded);
    return ok;
}

bool StripJpeg::writeTo(const Sink& sink) const {
    if (!impl_ || !sink || !sink(impl_->header.data(), impl_->header.size())) return false;
    for (std::size_t i = 0; i < impl_->strips.size(); ++i) {
        const auto& strip = impl_->strips[i];
        const auto begin = impl_->layouts[i].scanStart - (i > 0 ? 2u : 0u);
        // Keep the final strip's original EOI; earlier ones are replaced by RST.
        const auto end = strip.size - (i + 1u < impl_->strips.size() ? 2u : 0u);
        if (!sink(strip.jpeg + begin, end - begin)) return false;
    }
    return true;
}

bool encodeStripsRgba8(const unsigned char* rgba, int width, int height, int quality, int tjSubsamp,
                       unsigned char** jpeg, std::size_t* jpegSize, int* stripsUsed, std::string* error) {
    *jpeg = nullptr;
    *jpegSize = 0;
    if (stripsUsed) *stripsUsed = 1;
    StripJpeg encoded;
    if (!encoded.encodeRgba8(rgba, width, height, quality, tjSubsamp, error)) return false;
    auto* output = static_cast<unsigned char*>(tj3Alloc(encoded.byteSize()));
    if (!output) {
        *error = "tj3Alloc failed";
        return false;
    }
    auto* cursor = output;
    encoded.writeTo([&](const unsigned char* data, std::size_t size) {
        std::memcpy(cursor, data, size);
        cursor += size;
        return true;
    });
    *jpeg = output;
    *jpegSize = encoded.byteSize();
    if (stripsUsed) *stripsUsed = encoded.stripsUsed();
    return true;
}

}  // namespace rawrcam::encoding::jpeg
