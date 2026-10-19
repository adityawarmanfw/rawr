// Host check for encoding/jpeg/StripJpegEncoder: the strip-parallel JPEG must decode to exactly the pixels of a
// one-thread tj3Compress8 with the same settings. Usage: jpeg_strip_check [real.jpg]
// Build: see tools/jpeg_strip_check/README.md.
#include "../../app/src/main/cpp/encoding/jpeg/StripJpegEncoder.h"

#include <turbojpeg.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using rawrcam::encoding::jpeg::encodeStripsRgba8;

static double ms(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

static std::vector<unsigned char> decode(const unsigned char* j, size_t n, int* w, int* h) {
    tjhandle d = tj3Init(TJINIT_DECOMPRESS);
    std::vector<unsigned char> px;
    if (tj3DecompressHeader(d, j, n) == 0) {
        *w = tj3Get(d, TJPARAM_JPEGWIDTH);
        *h = tj3Get(d, TJPARAM_JPEGHEIGHT);
        px.resize(size_t(*w) * *h * 4);
        if (tj3Decompress8(d, j, n, px.data(), 0, TJPF_RGBA) != 0) px.clear();
    }
    tj3Destroy(d);
    return px;
}

static std::vector<unsigned char> synthetic(int w, int h) {
    std::vector<unsigned char> p(size_t(w) * h * 4);
    std::mt19937 rng(7);
    std::normal_distribution<float> noise(0.f, 6.f);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float fx = float(x) / w, fy = float(y) / h;
            float edge = ((x / 97 + y / 61) & 1) ? 40.f : 0.f;
            float r = 128 + 100 * std::sin(fx * 23 + fy * 7) + edge + noise(rng);
            float g = 128 + 90 * std::cos(fy * 19 - fx * 5) + noise(rng);
            float b = 128 + 80 * std::sin((fx + fy) * 31) - edge + noise(rng);
            unsigned char* q = &p[(size_t(y) * w + x) * 4];
            q[0] = (unsigned char)std::fmin(255.f, std::fmax(0.f, r));
            q[1] = (unsigned char)std::fmin(255.f, std::fmax(0.f, g));
            q[2] = (unsigned char)std::fmin(255.f, std::fmax(0.f, b));
            q[3] = 255;
        }
    return p;
}

static int check(const char* name, const unsigned char* rgba, int w, int h, int q, int ss) {
    tjhandle c = tj3Init(TJINIT_COMPRESS);
    tj3Set(c, TJPARAM_QUALITY, q);
    tj3Set(c, TJPARAM_SUBSAMP, ss);
    tj3Set(c, TJPARAM_FASTDCT, 0);
    unsigned char* one = nullptr;
    size_t oneN = 0;
    auto t0 = std::chrono::steady_clock::now();
    if (tj3Compress8(c, rgba, w, 0, h, TJPF_RGBA, &one, &oneN) != 0) {
        std::printf("%s: single encode failed %s\n", name, tj3GetErrorStr(c));
        return 1;
    }
    auto t1 = std::chrono::steady_clock::now();
    unsigned char* par = nullptr;
    size_t parN = 0;
    int strips = 0;
    std::string err;
    bool ok = encodeStripsRgba8(rgba, w, h, q, ss, &par, &parN, &strips, &err);
    auto t2 = std::chrono::steady_clock::now();
    int fails = 0;
    if (!ok) {
        std::printf("%-34s %dx%d q%d ss%d: strip path declined (%s) -> caller falls back\n", name, w, h, q, ss, err.c_str());
    } else {
        int w1, h1, w2, h2;
        auto a = decode(one, oneN, &w1, &h1), b = decode(par, parN, &w2, &h2);
        bool same = !a.empty() && a.size() == b.size() && w1 == w2 && h1 == h2 && std::memcmp(a.data(), b.data(), a.size()) == 0;
        if (!same) ++fails;
        std::printf("%-34s %dx%d q%d ss%d strips=%2d  single %7.1f ms  strips %7.1f ms (x%.1f)  bytes %+lld  pixels %s\n",
                    name, w, h, q, ss, strips, ms(t0, t1), ms(t1, t2), ms(t0, t1) / ms(t1, t2),
                    (long long)parN - (long long)oneN, same ? "IDENTICAL" : "DIFFER");
        tj3Free(par);
    }
    tj3Free(one);
    tj3Destroy(c);
    return fails;
}

int main(int argc, char** argv) {
    int fails = 0;
    const int sizes[][2] = {{4080, 3064}, {4081, 3063}, {8160, 6128}, {1920, 1080}, {640, 480}};
    const int subs[] = {TJSAMP_444, TJSAMP_420, TJSAMP_422, TJSAMP_GRAY};
    for (auto& s : sizes) {
        auto img = synthetic(s[0], s[1]);
        for (int ss : subs)
            for (int q : {95, 100}) fails += check("synthetic", img.data(), s[0], s[1], q, ss);
    }
    if (argc > 1) {
        FILE* f = std::fopen(argv[1], "rb");
        std::vector<unsigned char> buf;
        if (f) {
            std::fseek(f, 0, SEEK_END);
            buf.resize(std::ftell(f));
            std::fseek(f, 0, SEEK_SET);
            if (std::fread(buf.data(), 1, buf.size(), f) != buf.size()) buf.clear();
            std::fclose(f);
        }
        int w = 0, h = 0;
        auto px = decode(buf.data(), buf.size(), &w, &h);
        if (px.empty()) {
            std::printf("could not decode %s\n", argv[1]);
            ++fails;
        } else {
            for (int ss : {TJSAMP_444, TJSAMP_420})
                for (int q : {95, 98}) fails += check("real photo", px.data(), w, h, q, ss);
        }
    }
    std::printf("%s (%d failing)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
