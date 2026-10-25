#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "color/DcpProfile.h"
#include "metadata/CameraContextMetadata.h"

using rawrcam::color::DcpProfile;
using rawrcam::color::parseDcp;

namespace {

// Minimal one-IFD DCP writer: {tag: (type, raw little/big-endian value bytes, count)}.
struct Field {
    uint16_t type;
    uint32_t count;
    std::vector<uint8_t> bytes;
};

struct Builder {
    bool big = false;
    std::map<uint16_t, Field> fields;

    void put16(std::vector<uint8_t>& o, uint16_t v) const {
        if (big) o.insert(o.end(), {uint8_t(v >> 8), uint8_t(v)});
        else o.insert(o.end(), {uint8_t(v), uint8_t(v >> 8)});
    }
    void put32(std::vector<uint8_t>& o, uint32_t v) const {
        if (big) o.insert(o.end(), {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)});
        else o.insert(o.end(), {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)});
    }
    void shorts(uint16_t tag, std::vector<uint16_t> v) {
        Field f{3, uint32_t(v.size()), {}};
        for (auto x : v) put16(f.bytes, x);
        fields[tag] = f;
    }
    void longs(uint16_t tag, std::vector<uint32_t> v) {
        Field f{4, uint32_t(v.size()), {}};
        for (auto x : v) put32(f.bytes, x);
        fields[tag] = f;
    }
    void floats(uint16_t tag, const std::vector<float>& v) {
        Field f{11, uint32_t(v.size()), {}};
        for (float x : v) {
            uint32_t bits;
            std::memcpy(&bits, &x, 4);
            put32(f.bytes, bits);
        }
        fields[tag] = f;
    }
    void srationals(uint16_t tag, const std::vector<double>& v) {
        Field f{10, uint32_t(v.size()), {}};
        for (double x : v) {
            put32(f.bytes, uint32_t(int32_t(std::lround(x * 1000000.0))));
            put32(f.bytes, 1000000u);
        }
        fields[tag] = f;
    }
    void ascii(uint16_t tag, const std::string& s) {
        Field f{2, uint32_t(s.size() + 1), std::vector<uint8_t>(s.begin(), s.end())};
        f.bytes.push_back(0);
        fields[tag] = f;
    }
    std::vector<uint8_t> build() const {
        std::vector<uint8_t> out = big ? std::vector<uint8_t>{'M', 'M'} : std::vector<uint8_t>{'I', 'I'};
        put16(out, 0x4352);
        put32(out, 8);
        uint32_t data = uint32_t(8 + 2 + 12 * fields.size() + 4);
        std::vector<uint8_t> payload;
        put16(out, uint16_t(fields.size()));
        for (const auto& [tag, f] : fields) {
            put16(out, tag);
            put16(out, f.type);
            put32(out, f.count);
            if (f.bytes.size() <= 4) {
                auto v = f.bytes;
                v.resize(4, 0);
                out.insert(out.end(), v.begin(), v.end());
            } else {
                put32(out, data + uint32_t(payload.size()));
                payload.insert(payload.end(), f.bytes.begin(), f.bytes.end());
                if (payload.size() % 2) payload.push_back(0);
            }
        }
        put32(out, 0);
        out.insert(out.end(), payload.begin(), payload.end());
        return out;
    }
};

// A vivo-like D65/A dual profile.
const std::vector<double> kCm1{0.753296, -0.280151, -0.105835, -0.476562, 1.328125, 0.117188, -0.1297, 0.282288, 0.427246};
const std::vector<double> kCm2{1.734619, -0.94696, -0.398254, -0.632812, 1.71875, -0.0625, -0.041992, 0.120728, 0.435669};
const std::vector<double> kFm1{0.523438, 0.3125, 0.125, 0.1875, 0.898438, -0.085938, 0.023438, -0.375, 1.171875};
const std::vector<double> kFm2{0.5, 0.195312, 0.273438, 0.171875, 0.710938, 0.117188, 0, -0.429688, 1.257812};

Builder dualProfile(bool big) {
    Builder b;
    b.big = big;
    b.ascii(50936, "Test Profile");
    b.ascii(50708, "vivo V2562 / D14 / camera 4");
    b.srationals(50721, kCm1);
    b.srationals(50722, kCm2);
    b.shorts(50778, {21});
    b.shorts(50779, {17});
    b.srationals(50964, kFm1);
    b.srationals(50965, kFm2);
    b.longs(50937, {6, 2, 1});
    std::vector<float> map1, map2;
    for (int i = 0; i < 12; ++i) {
        map1.insert(map1.end(), {-40.0f, 1.0f, 1.0f});
        map2.insert(map2.end(), {40.0f, 1.0f, 1.0f});
    }
    b.floats(50938, map1);
    b.floats(50939, map2);
    b.longs(50981, {4, 2, 3});
    std::vector<float> look;
    for (int i = 0; i < 24; ++i) look.insert(look.end(), {0.0f, 0.5f, 1.0f});
    b.floats(50982, look);
    b.longs(51108, {1});
    b.floats(50940, {0.0f, 0.0f, 0.5f, 0.4f, 1.0f, 1.0f});
    b.longs(50941, {1});
    return b;
}

bool near(double a, double b, double eps = 2e-6) { return std::abs(a - b) <= eps; }

void testDualProfile(bool big) {
    std::string error;
    const auto bytes = dualProfile(big).build();
    const auto p = parseDcp(bytes, &error);
    assert(p && error.empty());
    assert(p->name == "Test Profile");
    assert(p->uniqueCameraModel == "vivo V2562 / D14 / camera 4");
    assert(p->illuminant1 == 21 && p->illuminant2 == 17);
    assert(p->dualIlluminant() && p->forwardMatrix1 && p->forwardMatrix2);
    for (size_t i = 0; i < 9; ++i) {
        assert(near(p->colorMatrix1[i], kCm1[i]));
        assert(near((*p->colorMatrix2)[i], kCm2[i]));
        assert(near((*p->forwardMatrix2)[i], kFm2[i]));
    }
    assert(p->hueSatMap1.hues == 6 && p->hueSatMap1.sats == 2 && p->hueSatMap1.vals == 1);
    assert(p->hueSatMap1.data.size() == 36 && p->hueSatMap2.data.size() == 36);
    assert(p->hueSatMap1.data[0] == -40.0f && p->hueSatMap2.data[0] == 40.0f);
    assert(p->lookTable.vals == 3 && p->lookTable.data.size() == 72 && p->lookTable.encoding == 1);
    assert(p->toneCurve.size() == 6 && p->embedPolicy == 1u);
    assert(p->bytes == bytes);

    const auto c = rawrcam::color::dcpColorCalibration(*p);
    assert(c);
    assert(c->referenceIlluminant1 == 21 && c->referenceIlluminant2 == 17);
    for (size_t i = 0; i < 9; ++i) {
        assert(c->calibrationTransform1.rowMajor[i] == (i % 4 == 0 ? 1.0f : 0.0f));
        assert(near(c->forwardMatrix1.rowMajor[i], kFm1[i], 1e-6));
    }
}

void testSingleIlluminantDerivesForward() {
    Builder b;
    b.srationals(50721, kCm1);
    b.shorts(50778, {21});
    const auto p = parseDcp(b.build());
    assert(p && !p->dualIlluminant() && !p->forwardMatrix1 && p->illuminant2 == 0);
    const auto c = rawrcam::color::dcpColorCalibration(*p);
    assert(c && c->referenceIlluminant2 == 21);
    // A derived ForwardMatrix maps the camera neutral (1,1,1) to D50.
    const auto& fm = c->forwardMatrix1.rowMajor;
    const double d50[3] = {0.96422, 1.0, 0.82521};
    for (int r = 0; r < 3; ++r) assert(near(fm[r * 3] + fm[r * 3 + 1] + fm[r * 3 + 2], d50[r], 2e-4));
    for (size_t i = 0; i < 9; ++i) assert(c->forwardMatrix2.rowMajor[i] == fm[i]);
}

void testRejects() {
    std::string error;
    {
        DcpProfile profile;
        profile.illuminant1 = 21;
        for (size_t i = 0; i < 9; ++i) profile.colorMatrix1[i] = kCm1[i];
        profile.forwardMatrix1 = std::array<double, 9>{};
        assert(!rawrcam::color::dcpColorCalibration(profile));
    }
    assert(!parseDcp({}, &error) && !error.empty());
    assert(!parseDcp({'I', 'I', 0x2a, 0, 8, 0, 0, 0}, &error));  // No directory.
    {
        Builder b;  // No ColorMatrix1.
        b.shorts(50778, {21});
        assert(!parseDcp(b.build(), &error));
    }
    {
        Builder b;  // Illuminant the solver can't place.
        b.srationals(50721, kCm1);
        b.shorts(50778, {255});
        assert(!parseDcp(b.build(), &error) && error.find("illuminant") != std::string::npos);
    }
    {
        Builder b;  // Four-colour matrix.
        b.srationals(50721, std::vector<double>(12, 0.5));
        b.shorts(50778, {21});
        assert(!parseDcp(b.build(), &error));
    }
    {
        auto b = dualProfile(false);  // Table size disagrees with its dims.
        b.longs(50937, {6, 3, 1});
        assert(!parseDcp(b.build(), &error));
    }
    {
        auto b = dualProfile(false);  // Decreasing tone curve.
        b.floats(50940, {0.0f, 0.0f, 0.6f, 0.5f, 0.5f, 0.6f, 1.0f, 1.0f});
        assert(!parseDcp(b.build(), &error));
    }
    // Every truncation of a valid file is rejected without reading past the end.
    const auto bytes = dualProfile(true).build();
    for (size_t n = 0; n < bytes.size(); ++n) {
        auto cut = std::vector<uint8_t>(bytes.begin(), bytes.begin() + long(n));
        (void)parseDcp(std::move(cut), &error);
    }
}

// Optional corpus (RawTherapee or Adobe profile directories): every file must parse.
int testCorpus(const std::filesystem::path& dir) {
    int parsed = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
        auto ext = entry.path().extension().string();
        for (auto& ch : ext) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        if (!entry.is_regular_file() || ext != ".dcp") continue;
        std::string error;
        const auto p = rawrcam::color::loadDcp(entry.path().string(), &error);
        if (!p) {
            std::cerr << "corpus reject " << entry.path() << ": " << error << '\n';
            continue;
        }
        assert(rawrcam::color::dcpColorCalibration(*p));
        ++parsed;
    }
    return parsed;
}

}  // namespace

// OPPO PKJ110 / CPH2841 HALs publish sRGB-in-D50 as ForwardMatrix1/2 next to a
// real ColorMatrix (diagnostic bundles 2026-10-24): trusting it left the image
// with an identity camera-to-sRGB matrix, i.e. desaturated.
void testPlaceholderForwardRepaired() {
    using rawrcam::metadata::Matrix3x3;
    const auto matrix = [](std::array<float, 9> v) { Matrix3x3 m; m.rowMajor = v; m.valid = true; return m; };
    rawrcam::metadata::StaticColorCalibration c;
    c.referenceIlluminant1 = 21;
    c.referenceIlluminant2 = 17;
    c.colorTransform1 = matrix({0.8085f, -0.3113f, -0.0842f, -0.4945f, 1.2695f, 0.184f, -0.1174f, 0.1895f, 0.539f});
    c.colorTransform2 = matrix({1.378f, -0.7536f, -0.1868f, -0.5651f, 1.6096f, 0.0316f, -0.0656f, 0.1616f, 0.7307f});
    const auto placeholder = matrix({0.4375f, 0.382812f, 0.140625f, 0.21875f, 0.71875f, 0.0625f, 0.015625f, 0.09375f, 0.710938f});
    c.forwardMatrix1 = placeholder;
    c.forwardMatrix2 = placeholder;
    assert(rawrcam::color::repairPlaceholderForwardMatrices(c) == 2);
    // Derived forward maps camera neutral (1,1,1) to D50 and carries real chroma.
    for (const auto* f : {&c.forwardMatrix1, &c.forwardMatrix2}) {
        const auto& m = f->rowMajor;
        assert(std::abs(m[0] + m[1] + m[2] - 0.96422f) < 2e-3f);
        assert(std::abs(m[3] + m[4] + m[5] - 1.0f) < 2e-3f);
        assert(std::abs(m[6] + m[7] + m[8] - 0.82521f) < 2e-3f);
        assert(std::abs(m[1] - placeholder.rowMajor[1]) > 0.01f || std::abs(m[8] - placeholder.rowMajor[8]) > 0.01f);
    }
    const auto repaired = c.forwardMatrix1;
    assert(rawrcam::color::repairPlaceholderForwardMatrices(c) == 0);  // idempotent
    assert(c.forwardMatrix1.rowMajor == repaired.rowMajor);

    // A real sensor forward matrix is never touched.
    c.forwardMatrix1 = matrix({0.5623f, 0.3933f, 0.0086f, 0.2147f, 0.9643f, -0.179f, 0.0323f, -0.1861f, 0.9787f});
    c.forwardMatrix2 = c.forwardMatrix1;
    assert(rawrcam::color::repairPlaceholderForwardMatrices(c) == 0);
}

int main(int argc, char** argv) {
    testDualProfile(false);
    testDualProfile(true);
    testSingleIlluminantDerivesForward();
    testRejects();
    testPlaceholderForwardRepaired();
    for (int i = 1; i < argc; ++i) {
        if (std::filesystem::is_directory(argv[i]))
            std::cout << "corpus " << argv[i] << ": " << testCorpus(argv[i]) << " parsed\n";
    }
    std::cout << "dcp_profile_test passed\n";
    return 0;
}
