#include "imaging/RawPixelSource.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <vector>

namespace rawrcam::imaging {

size_t minimumRowBytes(RawPixelFormat format, uint32_t width) noexcept {
    if (format == RawPixelFormat::Raw10) return static_cast<size_t>(width) / 4u * 5u;
    return static_cast<size_t>(width) * sizeof(uint16_t);
}

uint16_t readRawPixel(const RawPixelSource& source, uint32_t x, uint32_t y) noexcept {
    const uint8_t* row = source.base + static_cast<size_t>(y) * source.rowStrideBytes;
    if (source.format == RawPixelFormat::Raw10) {
        const uint8_t* group = row + static_cast<size_t>(x / 4u) * 5u;
        const uint32_t lane = x % 4u;
        return static_cast<uint16_t>((static_cast<uint16_t>(group[lane]) << 2) | ((group[4] >> (lane * 2u)) & 0x3u));
    }
    uint16_t value = 0;
    std::memcpy(&value, row + static_cast<size_t>(x) * sizeof(uint16_t), sizeof(value));
    return value;
}

void unpackRaw10Row(const uint8_t* packed, uint32_t width, uint16_t* destination) noexcept {
    for (uint32_t x = 0; x + 3u < width; x += 4u, packed += 5) {
        const uint8_t lsb = packed[4];
        destination[x + 0] = static_cast<uint16_t>((packed[0] << 2) | (lsb & 0x3u));
        destination[x + 1] = static_cast<uint16_t>((packed[1] << 2) | ((lsb >> 2) & 0x3u));
        destination[x + 2] = static_cast<uint16_t>((packed[2] << 2) | ((lsb >> 4) & 0x3u));
        destination[x + 3] = static_cast<uint16_t>((packed[3] << 2) | ((lsb >> 6) & 0x3u));
    }
}

bool copyRawToPackedRaw16(const RawPixelSource& source, uint16_t* destination) noexcept {
    if (!source.base || !destination || source.width == 0 || source.height == 0) return false;
    if (source.rowStrideBytes < minimumRowBytes(source.format, source.width)) return false;
    if (source.format == RawPixelFormat::Raw10 && source.width % 4u != 0) return false;
    for (uint32_t y = 0; y < source.height; ++y) {
        const uint8_t* row = source.base + static_cast<size_t>(y) * source.rowStrideBytes;
        uint16_t* out = destination + static_cast<size_t>(y) * source.width;
        if (source.format == RawPixelFormat::Raw10)
            unpackRaw10Row(row, source.width, out);
        else
            std::memcpy(out, row, static_cast<size_t>(source.width) * sizeof(uint16_t));
    }
    return true;
}

RawBlockUniformity rawBlockUniformity(const RawPixelSource& source, const RawContentStatsInput& input) {
    RawBlockUniformity out;
    if (!source.base || source.rowStrideBytes < minimumRowBytes(source.format, source.width)) return out;
    // A centred window: real sensor noise makes equal blocks rare, while a binned
    // frame upscaled by pixel repetition makes every aligned block equal. Blocks
    // at black or white are flat either way and are skipped.
    const auto uniform = [&](uint32_t block, uint64_t* sampled) {
        const uint32_t span = std::min<uint32_t>(1024u, std::min(source.width, source.height)) / 4u * 4u;
        const uint32_t x0 = (source.width - span) / 2u / 4u * 4u, y0 = (source.height - span) / 2u / 4u * 4u;
        uint64_t count = 0, equal = 0;
        for (uint32_t y = y0; y + block <= y0 + span; y += block) {
            for (uint32_t x = x0; x + block <= x0 + span; x += block) {
                const uint16_t first = readRawPixel(source, x, y);
                if (float(first) <= input.blackLevel || float(first) >= input.whiteLevel) continue;
                bool same = true;
                for (uint32_t dy = 0; dy < block && same; ++dy)
                    for (uint32_t dx = 0; dx < block && same; ++dx) same = readRawPixel(source, x + dx, y + dy) == first;
                ++count;
                if (same) ++equal;
            }
        }
        if (sampled) *sampled = count;
        return count ? double(equal) / double(count) : 0.0;
    };
    out.uniform2 = uniform(2u, &out.sampled);
    out.uniform4 = uniform(4u, nullptr);
    out.verdict = out.sampled < 64u              ? "flat"
                  : out.uniform4 > 0.9           ? "upscaled4x"
                  : out.uniform2 > 0.9           ? "upscaled2x"
                                                 : "real";
    return out;
}

std::string rawContentStatsLine(const RawPixelSource& source, const RawContentStatsInput& input) {
    std::ostringstream out;
    out << "RAW_CONTENT_STATS format=" << rawPixelFormatName(source.format) << " dims=" << source.width << 'x'
        << source.height << " rowStrideBytes=" << source.rowStrideBytes << " cfa=" << input.cfa;
    if (!source.base || source.width < 2 || source.height < 2 ||
        source.rowStrideBytes < minimumRowBytes(source.format, source.width)) {
        out << " error=invalid_source";
        return out.str();
    }

    // Channel slot per position inside the 2x2 quad, in R,Gr,Gb,B order.
    // Camera2 CFA names the top-left, top-right, bottom-left, bottom-right colors.
    static constexpr std::array<std::array<int, 4>, 4> kQuadChannel = {{
        {0, 1, 2, 3},  // RGGB
        {1, 0, 3, 2},  // GRBG
        {2, 3, 0, 1},  // GBRG
        {3, 2, 1, 0},  // BGGR
    }};
    static constexpr const char* kChannelName[4] = {"R", "Gr", "Gb", "B"};
    const auto& quad = kQuadChannel[static_cast<size_t>(std::clamp(input.cfa, 0, 3))];

    const uint32_t step = std::max(1u, input.quadStep);
    std::array<std::vector<uint16_t>, 4> samples;
    uint64_t total = 0;
    uint64_t atOrBelowBlack = 0;
    uint64_t atOrAboveWhite = 0;
    for (uint32_t qy = 0; qy + 1u < source.height; qy += 2u * step) {
        for (uint32_t qx = 0; qx + 1u < source.width; qx += 2u * step) {
            for (uint32_t i = 0; i < 4u; ++i) {
                const uint16_t v = readRawPixel(source, qx + (i & 1u), qy + (i >> 1u));
                samples[static_cast<size_t>(quad[i])].push_back(v);
                ++total;
                if (static_cast<float>(v) <= input.blackLevel) ++atOrBelowBlack;
                if (static_cast<float>(v) >= input.whiteLevel) ++atOrAboveWhite;
            }
        }
    }

    out << std::fixed << std::setprecision(1) << " black=" << input.blackLevel << " white=" << input.whiteLevel
        << " samples=" << total;
    for (size_t c = 0; c < samples.size(); ++c) {
        auto& values = samples[c];
        if (values.empty()) continue;
        uint64_t sum = 0;
        for (uint16_t v : values) sum += v;
        std::sort(values.begin(), values.end());
        const auto at = [&](double q) {
            return values[static_cast<size_t>(q * static_cast<double>(values.size() - 1))];
        };
        out << ' ' << kChannelName[c] << "=[min=" << values.front()
            << " mean=" << static_cast<double>(sum) / static_cast<double>(values.size()) << " p50=" << at(0.5)
            << " p99=" << at(0.99) << " max=" << values.back() << ']';
    }
    const double denom = total ? static_cast<double>(total) : 1.0;
    out << std::setprecision(4) << " fracAtOrBelowBlack=" << static_cast<double>(atOrBelowBlack) / denom
        << " fracAtOrAboveWhite=" << static_cast<double>(atOrAboveWhite) / denom;
    const auto blocks = rawBlockUniformity(source, input);
    out << " block2Uniform=" << blocks.uniform2 << " block4Uniform=" << blocks.uniform4
        << " blocksSampled=" << blocks.sampled << " detail=" << blocks.verdict;
    return out.str();
}

}  // namespace rawrcam::imaging
