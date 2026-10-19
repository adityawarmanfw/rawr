// Host check for UltraHdrMapFit.h changes: runs the previous version (OLD_FIT_HEADER, the HEAD copy in another
// namespace) and the current one on a real photo as the SDR base plus a gain map derived from it, and counts the
// map bytes that differ. Usage: uhdr_fit_check photo.jpg. Build: see README.md.
#include OLD_FIT_HEADER
#include "encoding/jpeg/UltraHdrMapFit.h"

#include <turbojpeg.h>

#include <chrono>
#include <cstdio>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) return 2;
    std::fseek(f, 0, SEEK_END);
    std::vector<unsigned char> jpg(std::ftell(f));
    std::fseek(f, 0, SEEK_SET);
    if (std::fread(jpg.data(), 1, jpg.size(), f) != jpg.size()) return 2;
    std::fclose(f);
    tjhandle d = tj3Init(TJINIT_DECOMPRESS);
    tj3DecompressHeader(d, jpg.data(), jpg.size());
    const int w = tj3Get(d, TJPARAM_JPEGWIDTH), h = tj3Get(d, TJPARAM_JPEGHEIGHT);
    std::vector<unsigned char> base(size_t(w) * h * 4);
    if (tj3Decompress8(d, jpg.data(), jpg.size(), base.data(), 0, TJPF_RGBA) != 0) return 2;
    tj3Destroy(d);
    // Wide-range map codes: brighter base -> more headroom, a few hot highlights near the top of the range.
    std::vector<unsigned char> map(base.size());
    for (size_t i = 0; i < size_t(w) * h; ++i) {
        for (int k = 0; k < 3; ++k) {
            int v = base[i * 4 + k];
            int g = v < 180 ? v / 6 : 30 + (v - 180) * 3;
            map[i * 4 + k] = (unsigned char)(g > 255 ? 255 : g);
        }
        map[i * 4 + 3] = 255;
    }
    rawrcam::color::UltraHdrParams uh;
    std::vector<unsigned char> a, b;
    float fa = 0, fb = 0;
    auto t0 = std::chrono::steady_clock::now();
    bool oka = oldfit::fitAndRemapMap(map.data(), w, h, uh, a, fa, base.data(), w, h);
    auto t1 = std::chrono::steady_clock::now();
    bool okb = rawrcam::encoding::jpeg::fitAndRemapMap(map.data(), w, h, uh, b, fb, base.data(), w, h);
    auto t2 = std::chrono::steady_clock::now();
    // Highlight colour restore on top: a capture chromaticity plane (16-bit r, g of r/(r+g+b)) from the base colour,
    // pushed towards saturated so the restore has work to do.
    std::vector<unsigned char> chroma(size_t(w) * h * 4);
    for (size_t i = 0; i < size_t(w) * h; ++i) {
        float r = base[i * 4] + 1.f, g = base[i * 4 + 1] + 1.f, bl = base[i * 4 + 2] + 1.f;
        r = r * r; g = g * g; bl = bl * bl;
        const unsigned cr = unsigned(r / (r + g + bl) * 65535), cg = unsigned(g / (r + g + bl) * 65535);
        chroma[i * 4] = cr & 255; chroma[i * 4 + 1] = cr >> 8; chroma[i * 4 + 2] = cg & 255; chroma[i * 4 + 3] = cg >> 8;
    }
    float mina = 0, maxa = 0, minb = 0, maxb = 0;
    auto t3 = std::chrono::steady_clock::now();
    bool ra = oldfit::restoreHighlightColor(a, chroma.data(), w, h, base.data(), w, h, uh, fa, mina, maxa);
    auto t4 = std::chrono::steady_clock::now();
    bool rb = rawrcam::encoding::jpeg::restoreHighlightColor(b, chroma.data(), w, h, base.data(), w, h, uh, fb, minb, maxb);
    auto t5 = std::chrono::steady_clock::now();
    std::printf("restore ok %d/%d range %.2f..%.2f / %.2f..%.2f  old %.1f ms  new %.1f ms\n", ra, rb, mina, maxa, minb, maxb,
                std::chrono::duration<double, std::milli>(t4 - t3).count(),
                std::chrono::duration<double, std::milli>(t5 - t4).count());
    size_t diff = 0, maxd = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i)
        if (a[i] != b[i]) {
            ++diff;
            size_t dd = a[i] > b[i] ? a[i] - b[i] : b[i] - a[i];
            if (dd > maxd) maxd = dd;
        }
    std::printf("%dx%d fit ok %d/%d max %.4f/%.4f  old %.1f ms  new %.1f ms  differing map bytes %zu of %zu (max |d| %zu)\n",
                w, h, oka, okb, fa, fb, std::chrono::duration<double, std::milli>(t1 - t0).count(),
                std::chrono::duration<double, std::milli>(t2 - t1).count(), diff, a.size(), maxd);
    return (oka && okb && fa == fb && ra == rb && mina == minb && maxa == maxb && diff <= a.size() / 1000000) ? 0 : 1;
}
