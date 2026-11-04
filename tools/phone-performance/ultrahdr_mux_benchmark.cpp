// Isolate container allocation/copy cost; excludes image encoding, fsync and camera.
#include "gainmap/UltrahdrMux.h"
#include <sys/resource.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

static std::vector<uint8_t> jpeg(size_t entropy, uint8_t fill) {
    // Structural fixture: SOI, SOF0, SOS, entropy, EOI. Not a decode benchmark.
    std::vector<uint8_t> out{0xff, 0xd8, 0xff, 0xc0, 0, 11, 8, 0, 16, 0, 16, 1, 1, 0x11, 0,
                             0xff, 0xda, 0, 8, 1, 1, 0, 0, 0x3f, 0};
    out.reserve(out.size() + entropy + 2);
    out.resize(out.size() + entropy, fill);
    out.insert(out.end(), {0xff, 0xd9});
    return out;
}
static bool writeAll(int fd, const uint8_t* p, size_t n) {
    while (n) {
        const auto count = ::write(fd, p, n);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        p += count;
        n -= static_cast<size_t>(count);
    }
    return true;
}
int main(int argc, char** argv) {
    try {
        if (argc < 5 || argc > 6) throw std::runtime_error("MODE BASE_MIB MAP_MIB SAMPLES [OUTPUT]");
        const std::string mode = argv[1];
        const int baseMiB = std::stoi(argv[2]), mapMiB = std::stoi(argv[3]), samples = std::stoi(argv[4]);
        if ((mode != "flat" && mode != "segments") || baseMiB < 1 || baseMiB > 128 ||
            mapMiB < 1 || mapMiB > 128 || samples < 1 || samples > 1000)
            throw std::runtime_error("invalid parameters");
#ifndef RAWR_MUX_SEGMENTS
        if (mode != "flat") throw std::runtime_error("baseline supports flat only");
#endif
        const auto base = jpeg(size_t(baseMiB) << 20, 0x21), map = jpeg(size_t(mapMiB) << 20, 0x42);
        const uint8_t exif[]{'E', 'x', 'i', 'f', 0, 0};
        std::vector<double> times;
        size_t fileBytes = 0;
        for (int i = -3; i < samples + (argc == 6 ? 1 : 0); ++i) {
            const bool save = argc == 6 && i == samples;
            const int fd = ::open(save ? argv[5] : "/dev/null", O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (fd < 0) throw std::runtime_error("open failed");
            const auto begin = std::chrono::steady_clock::now();
            gainmap::UltrahdrMuxResult result;
            bool written = false;
            if (mode == "flat") {
                std::vector<uint8_t> file;
                result = gainmap::UltrahdrMux::assemble(base.data(), base.size(), map.data(), map.size(),
                    exif, sizeof(exif), {}, file);
                if (result.ok) written = writeAll(fd, file.data(), file.size());
            }
#ifdef RAWR_MUX_SEGMENTS
            else {
                gainmap::UltrahdrMuxParts parts;
                result = gainmap::UltrahdrMux::assembleParts(base.data(), base.size(), map.data(), map.size(),
                    exif, sizeof(exif), {}, parts);
                if (result.ok)
                    written = writeAll(fd, parts.primaryHeader.data(), parts.primaryHeader.size()) &&
                              writeAll(fd, parts.primaryScan, parts.primaryScanSize) &&
                              writeAll(fd, parts.secondaryHeader.data(), parts.secondaryHeader.size()) &&
                              writeAll(fd, parts.secondaryBody, parts.secondaryBodySize);
            }
#endif
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
            ::close(fd);
            if (!result.ok || !written) throw std::runtime_error("mux/write failed: " + result.error);
            fileBytes = result.primarySize + result.secondarySize;
            if (i >= 0 && !save) times.push_back(ms);
        }
        std::sort(times.begin(), times.end());
        rusage usage{};
        if (::getrusage(RUSAGE_SELF, &usage) != 0) throw std::runtime_error("getrusage failed");
#ifdef __APPLE__
        const auto rss = static_cast<long long>(usage.ru_maxrss);
#else
        const auto rss = static_cast<long long>(usage.ru_maxrss) * 1024;
#endif
        std::printf("{\"mode\":\"%s\",\"baseMiB\":%d,\"mapMiB\":%d,\"samples\":%d,\"fileBytes\":%zu,"
                    "\"medianMs\":%.4f,\"p95Ms\":%.4f,\"peakRssBytes\":%lld,\"sink\":\"devnull\"}\n",
                    mode.c_str(), baseMiB, mapMiB, samples, fileBytes,
                    times[times.size() / 2], times[(times.size() - 1) * 95 / 100], rss);
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
