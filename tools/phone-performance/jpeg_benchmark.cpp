// Host/Android CPU benchmark of production JPEG output, never a camera/Vulkan benchmark.
// See README.md for the baseline build and paired runs.
#include "encoding/jpeg/StripJpegEncoder.h"
#include <turbojpeg.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;
using namespace rawrcam::encoding::jpeg;

static double millis(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}
static bool writeAll(int fd, const unsigned char* data, std::size_t size) {
    while (size) {
        const auto n = ::write(fd, data, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        size -= static_cast<std::size_t>(n);
        data += n;
    }
    return true;
}
static double percentile(std::vector<double> times, double fraction) {
    std::sort(times.begin(), times.end());
    if (fraction == 0.5) return (times[(times.size() - 1) / 2] + times[times.size() / 2]) / 2;
    return times[static_cast<std::size_t>(std::ceil(fraction * times.size())) - 1];
}

int main(int argc, char** argv) {
    try {
        if (argc < 7 || argc > 8) {
            std::fprintf(stderr, "Usage: jpeg_benchmark flat|segments WIDTH HEIGHT QUALITY SUBSAMP ITERATIONS [JPEG]\n");
            return 2;
        }
        const std::string mode = argv[1];
        const int width = std::stoi(argv[2]), height = std::stoi(argv[3]);
        const int quality = std::stoi(argv[4]), subsamp = std::stoi(argv[5]);
        const int count = std::stoi(argv[6]);
        if ((mode != "flat" && mode != "segments") || width < 1 || width > 16384 ||
            height < 1 || height > 16384 || quality < 1 || quality > 100 ||
            subsamp < 0 || subsamp >= TJ_NUMSAMP || count < 1 || count > 1000)
            throw std::runtime_error("invalid benchmark parameters");
#ifndef RAWR_SEGMENTED
        if (mode == "segments") throw std::runtime_error("baseline build has no segmented API");
#endif
        std::vector<unsigned char> image(static_cast<std::size_t>(width) * height * 4);
        // Deterministic integer-only gradients + texture; no camera scene drift.
        std::uint32_t rng = 7;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
                auto* p = image.data() + (static_cast<std::size_t>(y) * width + x) * 4;
                p[0] = static_cast<unsigned char>((x * 127 / width + y * 63 / height + (rng & 31)) & 255);
                p[1] = static_cast<unsigned char>((y * 127 / height + ((x / 31 + y / 47) & 1) * 64 + ((rng >> 8) & 31)) & 255);
                p[2] = static_cast<unsigned char>((x * 191 / width + ((rng >> 16) & 63)) & 255);
                p[3] = 255;
            }
        }
        std::vector<double> encodes, totals;
        std::size_t bytes = 0;
        int stripCount = 0;
        for (int iteration = -3; iteration < count + (argc == 8 ? 1 : 0); ++iteration) {
            const bool save = argc == 8 && iteration == count;
            const int fd = ::open(save ? argv[7] : "/dev/null", O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (fd < 0) throw std::runtime_error("open failed");
            std::string error;
            const auto start = Clock::now();
            auto encodedAt = start;
            bool ok = false;
#ifdef RAWR_SEGMENTED
            if (mode == "segments") {
                StripJpeg encoded;
                if (!encoded.encodeRgba8(image.data(), width, height, quality, subsamp, &error))
                    throw std::runtime_error(error);
                bytes = encoded.byteSize();
                stripCount = encoded.stripsUsed();
                encodedAt = Clock::now();
                ok = encoded.writeTo([&](const unsigned char* data, std::size_t size) { return writeAll(fd, data, size); });
            } else
#endif
            {
                unsigned char* output = nullptr;
                if (!encodeStripsRgba8(image.data(), width, height, quality, subsamp, &output, &bytes, &stripCount, &error))
                    throw std::runtime_error(error);
                encodedAt = Clock::now();
                ok = writeAll(fd, output, bytes);
                tj3Free(output);
            }
            const auto finished = Clock::now();
            const bool closed = ::close(fd) == 0;
            if (!ok || !closed) throw std::runtime_error("write/close failed");
            if (iteration >= 0 && iteration < count) {
                encodes.push_back(millis(start, encodedAt));
                totals.push_back(millis(start, finished));
                std::fprintf(stderr, "{\"sample\":%d,\"encodeMs\":%.3f,\"totalMs\":%.3f}\n",
                             iteration, encodes.back(), totals.back());
            }
        }
        rusage usage{};
        const bool hasRss = ::getrusage(RUSAGE_SELF, &usage) == 0;
#ifdef __APPLE__
        const long long rssBytes = static_cast<long long>(usage.ru_maxrss);
#else
        const long long rssBytes = static_cast<long long>(usage.ru_maxrss) * 1024;
#endif
        std::printf("{\"mode\":\"%s\",\"width\":%d,\"height\":%d,\"quality\":%d,\"subsampling\":%d,"
                    "\"warmups\":3,\"samples\":%d,\"strips\":%d,\"jpegBytes\":%zu,"
                    "\"encodeMedianMs\":%.3f,\"encodeP95Ms\":%.3f,\"totalMedianMs\":%.3f,\"totalP95Ms\":%.3f,"
                    "\"peakRssBytes\":%lld,\"sink\":\"devnull\",\"saveOutsideTiming\":true}\n",
                    mode.c_str(), width, height, quality, subsamp, count, stripCount, bytes,
                    percentile(encodes, 0.5), percentile(encodes, 0.95), percentile(totals, 0.5),
                    percentile(totals, 0.95), hasRss ? rssBytes : -1);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
