#include "color/DcpProfile.h"

#include <cmath>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>

#include "color/ColorMath.h"
#include "metadata/CameraContextMetadata.h"

namespace rawrcam::color {
namespace {

constexpr uint16_t kUniqueCameraModel = 50708;
constexpr uint16_t kColorMatrix1 = 50721;
constexpr uint16_t kColorMatrix2 = 50722;
constexpr uint16_t kCalibrationIlluminant1 = 50778;
constexpr uint16_t kCalibrationIlluminant2 = 50779;
constexpr uint16_t kProfileCalibrationSignature = 50932;
constexpr uint16_t kProfileName = 50936;
constexpr uint16_t kProfileHueSatMapDims = 50937;
constexpr uint16_t kProfileHueSatMapData1 = 50938;
constexpr uint16_t kProfileHueSatMapData2 = 50939;
constexpr uint16_t kProfileToneCurve = 50940;
constexpr uint16_t kProfileEmbedPolicy = 50941;
constexpr uint16_t kProfileCopyright = 50942;
constexpr uint16_t kForwardMatrix1 = 50964;
constexpr uint16_t kForwardMatrix2 = 50965;
constexpr uint16_t kProfileLookTableDims = 50981;
constexpr uint16_t kProfileLookTableData = 50982;
constexpr uint16_t kProfileHueSatMapEncoding = 51107;
constexpr uint16_t kProfileLookTableEncoding = 51108;
constexpr uint16_t kBaselineExposureOffset = 51109;
constexpr uint16_t kDefaultBlackRender = 51110;

// Far above any real profile (Adobe: 90x30x1 maps, 36x8x16 looks).
constexpr uint64_t kMaxTableEntries = 1u << 20;

struct Entry {
    uint16_t tag = 0;
    uint16_t type = 0;
    uint32_t count = 0;
    size_t offset = 0;  // Absolute offset of the value bytes.
};

class Reader {
public:
    Reader(const std::vector<uint8_t>& bytes, bool bigEndian) : bytes_(bytes), big_(bigEndian) {}

    bool has(size_t offset, size_t n) const { return offset <= bytes_.size() && n <= bytes_.size() - offset; }
    uint16_t u16(size_t o) const {
        const uint8_t* p = bytes_.data() + o;
        return big_ ? uint16_t(p[0] << 8 | p[1]) : uint16_t(p[1] << 8 | p[0]);
    }
    uint32_t u32(size_t o) const {
        const uint8_t* p = bytes_.data() + o;
        return big_ ? uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]
                    : uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
    }
    uint64_t u64(size_t o) const {
        const uint64_t a = u32(o), b = u32(o + 4);
        return big_ ? a << 32 | b : b << 32 | a;
    }
    const uint8_t* data(size_t o) const { return bytes_.data() + o; }

private:
    const std::vector<uint8_t>& bytes_;
    bool big_;
};

size_t typeSize(uint16_t type) {
    switch (type) {
        case 1: case 2: case 6: case 7: return 1;  // BYTE ASCII SBYTE UNDEFINED
        case 3: case 8: return 2;                  // SHORT SSHORT
        case 4: case 9: case 11: return 4;         // LONG SLONG FLOAT
        case 5: case 10: case 12: return 8;        // RATIONAL SRATIONAL DOUBLE
        default: return 0;
    }
}

bool fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}

std::nullopt_t reject(std::string* error, const std::string& message) {
    fail(error, message);
    return std::nullopt;
}

// Numeric values of any integer, rational or float entry.
bool numbers(const Reader& r, const Entry& e, std::vector<double>& out) {
    out.clear();
    const size_t size = typeSize(e.type);
    if (!size || e.type == 2 || e.type == 7) return false;
    out.reserve(e.count);
    for (uint32_t i = 0; i < e.count; ++i) {
        const size_t o = e.offset + size_t(i) * size;
        double v = 0.0;
        switch (e.type) {
            case 1: v = *r.data(o); break;
            case 6: v = int8_t(*r.data(o)); break;
            case 3: v = r.u16(o); break;
            case 8: v = int16_t(r.u16(o)); break;
            case 4: v = r.u32(o); break;
            case 9: v = int32_t(r.u32(o)); break;
            case 5: {
                const uint32_t d = r.u32(o + 4);
                if (!d) return false;
                v = double(r.u32(o)) / d;
                break;
            }
            case 10: {
                const int32_t d = int32_t(r.u32(o + 4));
                if (!d) return false;
                v = double(int32_t(r.u32(o))) / d;
                break;
            }
            case 11: {
                const uint32_t bits = r.u32(o);
                float f;
                std::memcpy(&f, &bits, 4);
                v = f;
                break;
            }
            case 12: {
                const uint64_t bits = r.u64(o);
                std::memcpy(&v, &bits, 8);
                break;
            }
            default: return false;
        }
        if (!std::isfinite(v)) return false;
        out.push_back(v);
    }
    return true;
}

std::string ascii(const Reader& r, const Entry& e) {
    if (e.type != 2 && e.type != 1 && e.type != 7) return {};
    std::string s(reinterpret_cast<const char*>(r.data(e.offset)), e.count);
    if (const auto nul = s.find('\0'); nul != std::string::npos) s.resize(nul);
    return s;
}

const Entry* find(const std::vector<Entry>& entries, uint16_t tag) {
    for (const auto& e : entries)
        if (e.tag == tag) return &e;
    return nullptr;
}

bool matrix(const Reader& r, const Entry* e, std::array<double, 9>& out, const char* label, std::string* error) {
    std::vector<double> v;
    if (!numbers(r, *e, v)) return fail(error, std::string("unreadable ") + label);
    if (v.size() != 9) return fail(error, std::string(label) + " is not 3x3 (only three-colour profiles are supported)");
    std::copy(v.begin(), v.end(), out.begin());
    return true;
}

bool dims(const Reader& r, const Entry* e, DcpHueSatTable& t, const char* label, std::string* error) {
    std::vector<double> v;
    if (!numbers(r, *e, v) || v.size() != 3) return fail(error, std::string("unreadable ") + label);
    t.hues = uint32_t(v[0]);
    t.sats = uint32_t(v[1]);
    t.vals = uint32_t(v[2]);
    // One hue division is legal (no hue variation); saturation needs two.
    if (t.hues < 1 || t.sats < 2 || t.vals < 1 || uint64_t(t.hues) * t.sats * t.vals > kMaxTableEntries)
        return fail(error, std::string(label) + " has invalid dimensions");
    return true;
}

bool table(const Reader& r, const Entry* e, DcpHueSatTable& t, const char* label, std::string* error) {
    std::vector<double> v;
    if (!numbers(r, *e, v)) return fail(error, std::string("unreadable ") + label);
    if (v.size() != size_t(t.hues) * t.sats * t.vals * 3) return fail(error, std::string(label) + " size mismatch");
    t.data.assign(v.begin(), v.end());
    return true;
}

bool knownIlluminant(uint16_t id) { return math::referenceIlluminantCctKelvin(id).has_value(); }

using Mat3d = std::array<double, 9>;
using Vec3d = std::array<double, 3>;

Mat3d mul(const Mat3d& a, const Mat3d& b) {
    Mat3d out{};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            for (int k = 0; k < 3; ++k) out[r * 3 + c] += a[r * 3 + k] * b[k * 3 + c];
    return out;
}
Vec3d mul(const Mat3d& a, const Vec3d& v) {
    return {a[0] * v[0] + a[1] * v[1] + a[2] * v[2], a[3] * v[0] + a[4] * v[1] + a[5] * v[2],
            a[6] * v[0] + a[7] * v[1] + a[8] * v[2]};
}
std::optional<Mat3d> inv(const Mat3d& m) {
    const double det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
                       m[2] * (m[3] * m[7] - m[4] * m[6]);
    if (!(std::abs(det) > 1e-12)) return std::nullopt;
    return Mat3d{(m[4] * m[8] - m[5] * m[7]) / det, (m[2] * m[7] - m[1] * m[8]) / det,
                 (m[1] * m[5] - m[2] * m[4]) / det, (m[5] * m[6] - m[3] * m[8]) / det,
                 (m[0] * m[8] - m[2] * m[6]) / det, (m[2] * m[3] - m[0] * m[5]) / det,
                 (m[3] * m[7] - m[4] * m[6]) / det, (m[1] * m[6] - m[0] * m[7]) / det,
                 (m[0] * m[4] - m[1] * m[3]) / det};
}

// XYZ white (Y=1) of an illuminant the app solver knows: tabulated whites for
// the CIE standards, the Planckian locus for studio tungsten.
Vec3d illuminantWhite(uint16_t id) {
    switch (id) {
        case 17: return {1.09850, 1, 0.35585};  // Standard light A
        case 18: return {0.99072, 1, 0.85223};  // Standard light B
        case 19: return {0.98074, 1, 1.18232};  // Standard light C
        case 20: return {0.95682, 1, 0.92149};  // D55
        case 21: return {0.95047, 1, 1.08883};  // D65
        case 22: return {0.94972, 1, 1.22638};  // D75
        case 23: return {0.96422, 1, 0.82521};  // D50
        default: break;
    }
    // ISO studio tungsten, 3200 K (Kim et al. Planckian locus).
    const double t = math::referenceIlluminantCctKelvin(id).value_or(3200.0f);
    const double x = -0.2661239e9 / (t * t * t) - 0.2343589e6 / (t * t) + 0.8776956e3 / t + 0.179910;
    const double y = -0.9549476 * x * x * x - 1.37418593 * x * x + 2.09137015 * x - 0.16748867;
    return {x / y, 1, (1 - x - y) / y};
}

metadata::Matrix3x3 toMatrix(const Mat3d& m) {
    metadata::Matrix3x3 out;
    for (size_t i = 0; i < 9; ++i) out.rowMajor[i] = float(m[i]);
    out.valid = true;
    return out;
}

}  // namespace

std::optional<DcpProfile> parseDcp(std::vector<uint8_t> bytes, std::string* error) {
    if (bytes.size() < 8) return reject(error, "file too short");
    const bool big = bytes[0] == 'M' && bytes[1] == 'M';
    if (!big && !(bytes[0] == 'I' && bytes[1] == 'I')) return reject(error, "not a DCP file");
    const Reader r(bytes, big);
    // DCP magic is "RC" (0x4352); plain TIFF (42) is accepted for hand-made files.
    const uint16_t magic = r.u16(2);
    if (magic != 0x4352 && magic != 42) return reject(error, "not a DCP file");
    const size_t ifd = r.u32(4);
    if (!r.has(ifd, 2)) return reject(error, "truncated header");
    const uint16_t count = r.u16(ifd);
    if (!r.has(ifd + 2, size_t(count) * 12)) return reject(error, "truncated tag directory");

    std::vector<Entry> entries;
    entries.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        const size_t o = ifd + 2 + size_t(i) * 12;
        Entry e;
        e.tag = r.u16(o);
        e.type = r.u16(o + 2);
        e.count = r.u32(o + 4);
        const size_t size = typeSize(e.type);
        if (!size) continue;  // Unknown types are skipped, as TIFF readers do.
        const uint64_t total = uint64_t(size) * e.count;
        e.offset = total <= 4 ? o + 8 : r.u32(o + 8);
        if (total > bytes.size() || !r.has(e.offset, size_t(total)))
            return reject(error, "tag " + std::to_string(e.tag) + " points outside the file");
        entries.push_back(e);
    }

    DcpProfile p;
    std::vector<double> v;
    const auto* cm1 = find(entries, kColorMatrix1);
    const auto* il1 = find(entries, kCalibrationIlluminant1);
    if (!cm1 || !il1) return reject(error, "no ColorMatrix1 / CalibrationIlluminant1");
    if (!matrix(r, cm1, p.colorMatrix1, "ColorMatrix1", error)) return std::nullopt;
    if (!numbers(r, *il1, v) || v.size() != 1) return reject(error, "unreadable CalibrationIlluminant1");
    p.illuminant1 = uint16_t(v[0]);
    if (const auto* cm2 = find(entries, kColorMatrix2)) {
        std::array<double, 9> m{};
        if (!matrix(r, cm2, m, "ColorMatrix2", error)) return std::nullopt;
        const auto* il2 = find(entries, kCalibrationIlluminant2);
        if (!il2 || !numbers(r, *il2, v) || v.size() != 1)
            return reject(error, "ColorMatrix2 without CalibrationIlluminant2");
        p.colorMatrix2 = m;
        p.illuminant2 = uint16_t(v[0]);
    }
    for (const uint16_t id : {p.illuminant1, p.illuminant2}) {
        if (id && !knownIlluminant(id))
            return reject(error, "unsupported calibration illuminant " + std::to_string(id) +
                                   " (supported: Standard A/B/C, D50/D55/D65/D75, studio tungsten)");
    }
    if (p.colorMatrix2 && p.illuminant1 == p.illuminant2) {
        // Two sets at one illuminant can't be interpolated: keep the first.
        p.colorMatrix2.reset();
        p.illuminant2 = 0;
    }
    if (const auto* fm1 = find(entries, kForwardMatrix1)) {
        std::array<double, 9> m{};
        if (!matrix(r, fm1, m, "ForwardMatrix1", error)) return std::nullopt;
        p.forwardMatrix1 = m;
    }
    if (const auto* fm2 = find(entries, kForwardMatrix2); fm2 && p.colorMatrix2) {
        std::array<double, 9> m{};
        if (!matrix(r, fm2, m, "ForwardMatrix2", error)) return std::nullopt;
        p.forwardMatrix2 = m;
    }
    // The DNG spec pairs forward matrices: one without the other is unused.
    if (p.colorMatrix2 && bool(p.forwardMatrix1) != bool(p.forwardMatrix2)) {
        p.forwardMatrix1.reset();
        p.forwardMatrix2.reset();
    }
    if (!inv(p.colorMatrix1)) return reject(error, "ColorMatrix1 is singular");
    if (p.colorMatrix2 && !inv(*p.colorMatrix2)) return reject(error, "ColorMatrix2 is singular");

    if (const auto* d = find(entries, kProfileHueSatMapDims)) {
        if (!dims(r, d, p.hueSatMap1, "ProfileHueSatMapDims", error)) return std::nullopt;
        p.hueSatMap2.hues = p.hueSatMap1.hues;
        p.hueSatMap2.sats = p.hueSatMap1.sats;
        p.hueSatMap2.vals = p.hueSatMap1.vals;
        const auto* d1 = find(entries, kProfileHueSatMapData1);
        const auto* d2 = find(entries, kProfileHueSatMapData2);
        if (d1 && !table(r, d1, p.hueSatMap1, "ProfileHueSatMapData1", error)) return std::nullopt;
        if (d2 && p.colorMatrix2 && !table(r, d2, p.hueSatMap2, "ProfileHueSatMapData2", error)) return std::nullopt;
        // A dual-illuminant profile interpolates both maps; with only one, use it at both ends.
        if (p.hueSatMap1.empty() && !p.hueSatMap2.empty()) std::swap(p.hueSatMap1.data, p.hueSatMap2.data);
        if (!p.colorMatrix2) p.hueSatMap2.data.clear();
        if (const auto* enc = find(entries, kProfileHueSatMapEncoding); enc && numbers(r, *enc, v) && v.size() == 1)
            p.hueSatMap1.encoding = p.hueSatMap2.encoding = uint32_t(v[0]) == 1 ? 1 : 0;
    }
    if (const auto* d = find(entries, kProfileLookTableDims)) {
        if (!dims(r, d, p.lookTable, "ProfileLookTableDims", error)) return std::nullopt;
        if (const auto* data = find(entries, kProfileLookTableData);
            data && !table(r, data, p.lookTable, "ProfileLookTableData", error))
            return std::nullopt;
        if (const auto* enc = find(entries, kProfileLookTableEncoding); enc && numbers(r, *enc, v) && v.size() == 1)
            p.lookTable.encoding = uint32_t(v[0]) == 1 ? 1 : 0;
    }
    if (const auto* t = find(entries, kProfileToneCurve)) {
        if (!numbers(r, *t, v) || v.size() < 4 || v.size() % 2)
            return reject(error, "unreadable ProfileToneCurve");
        for (size_t i = 0; i < v.size(); ++i) {
            if (v[i] < 0.0 || v[i] > 1.0 || (i >= 2 && i % 2 == 0 && v[i] <= v[i - 2]))
                return reject(error, "ProfileToneCurve is not increasing in [0, 1]");
        }
        p.toneCurve.assign(v.begin(), v.end());
    }
    if (const auto* e = find(entries, kProfileEmbedPolicy); e && numbers(r, *e, v) && v.size() == 1)
        p.embedPolicy = uint32_t(v[0]);
    if (const auto* e = find(entries, kBaselineExposureOffset); e && numbers(r, *e, v) && v.size() == 1)
        p.baselineExposureOffset = v[0];
    if (const auto* e = find(entries, kDefaultBlackRender); e && numbers(r, *e, v) && v.size() == 1 && v[0] <= 1)
        p.defaultBlackRender = uint32_t(v[0]);
    if (const auto* e = find(entries, kProfileName)) p.name = ascii(r, *e);
    if (const auto* e = find(entries, kProfileCopyright)) p.copyright = ascii(r, *e);
    if (const auto* e = find(entries, kProfileCalibrationSignature)) p.calibrationSignature = ascii(r, *e);
    if (const auto* e = find(entries, kUniqueCameraModel)) p.uniqueCameraModel = ascii(r, *e);
    p.bytes = std::move(bytes);
    return p;
}

std::optional<DcpProfile> loadDcp(const std::string& path, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return reject(error, "cannot open " + path);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() > 64u * 1024u * 1024u) return reject(error, "file too large");
    return parseDcp(std::move(bytes), error);
}

std::optional<DcpProfile> loadEmbeddedDcp(const std::string& path, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    const auto read = [&](uint64_t offset, size_t count) {
        std::vector<uint8_t> out(count);
        in.seekg(static_cast<std::streamoff>(offset));
        if (!in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(count))) out.clear();
        return out;
    };
    auto header = read(0, 8);
    if (header.size() != 8) return reject(error, "truncated TIFF header");
    const bool big = header[0] == 'M' && header[1] == 'M';
    if (!big && !(header[0] == 'I' && header[1] == 'I')) return reject(error, "not a TIFF file");
    const Reader h(header, big);
    if (h.u16(2) != 42) return reject(error, "unsupported TIFF header");
    const auto ifd = h.u32(4);
    auto countBytes = read(ifd, 2);
    if (countBytes.size() != 2) return reject(error, "truncated TIFF directory");
    const size_t count = Reader(countBytes, big).u16(0);
    const auto directory = read(uint64_t(ifd) + 2, count * 12);
    if (directory.size() != count * 12) return reject(error, "truncated TIFF directory");
    const Reader r(directory, big);
    std::vector<size_t> entries;
    for (size_t i = 0; i < count; ++i) {
        switch (r.u16(i * 12)) {
            case kColorMatrix1: case kColorMatrix2: case kCalibrationIlluminant1: case kCalibrationIlluminant2:
            case kForwardMatrix1: case kForwardMatrix2: case kProfileName: case kProfileCopyright:
            case kProfileHueSatMapDims: case kProfileHueSatMapData1: case kProfileHueSatMapData2:
            case kProfileHueSatMapEncoding: case kProfileLookTableDims: case kProfileLookTableData:
            case kProfileLookTableEncoding: case kProfileToneCurve: case kProfileEmbedPolicy:
            case kBaselineExposureOffset: case kDefaultBlackRender:
                entries.push_back(i * 12); break;
            default: break;
        }
    }
    std::vector<uint8_t> bytes(8 + 2 + entries.size()*12 + 4, 0);
    const auto put = [&](size_t offset, uint32_t value, size_t size) {
        for (size_t i = 0; i < size; ++i) bytes[offset+i] = uint8_t(value >> (8*(big ? size-i-1 : i)));
    };
    bytes[0] = bytes[1] = big ? 'M' : 'I';
    put(2, 0x4352, 2); put(4, 8, 4); put(8, uint32_t(entries.size()), 2);
    for (size_t i = 0; i < entries.size(); ++i) {
        const size_t source = entries[i], target = 10 + 12*i;
        std::copy_n(directory.begin()+source, 12, bytes.begin()+target);
        const uint64_t size = uint64_t(typeSize(r.u16(source+2))) * r.u32(source+4);
        if (size <= 4) continue;
        if (size > 64u*1024u*1024u || bytes.size()+size > 64u*1024u*1024u)
            return reject(error, "embedded profile is too large");
        const auto payload = read(r.u32(source+8), size_t(size));
        if (payload.size() != size) return reject(error, "truncated embedded profile");
        put(target+8, uint32_t(bytes.size()), 4);
        bytes.insert(bytes.end(), payload.begin(), payload.end());
    }
    return parseDcp(std::move(bytes), error);
}

std::optional<std::array<double, 9>> forwardFromColorMatrix(const std::array<double, 9>& colorMatrix,
                                                            uint16_t illuminant) {
    if (!knownIlluminant(illuminant)) return std::nullopt;
    const Mat3d bradford{.8951, .2664, -.1614, -.7502, 1.7135, .0367, .0389, -.0685, 1.0296};
    const auto bradfordInverse = inv(bradford);
    const auto cameraToXyz = inv(colorMatrix);
    if (!bradfordInverse || !cameraToXyz) return std::nullopt;
    const Vec3d white = illuminantWhite(illuminant);
    const Vec3d from = mul(bradford, white);
    const Vec3d to = mul(bradford, Vec3d{0.96422, 1, 0.82521});
    Mat3d scale{};
    for (int i = 0; i < 3; ++i) scale[size_t(i) * 4] = to[size_t(i)] / from[size_t(i)];
    const Mat3d adapt = mul(*bradfordInverse, mul(scale, bradford));
    const Vec3d neutral = mul(colorMatrix, white);
    Mat3d diag{};
    for (int i = 0; i < 3; ++i) diag[size_t(i) * 4] = neutral[size_t(i)];
    return mul(adapt, mul(*cameraToXyz, diag));
}

int repairPlaceholderForwardMatrices(metadata::StaticColorCalibration& color) {
    // sRGB -> XYZ(D50), the DNG-spec / Bradford-adapted primaries.
    static constexpr std::array<double, 9> kSrgbD50{0.4360747, 0.3850649, 0.1430804, 0.2225045, 0.7168786,
                                                    0.0606169, 0.0139322, 0.0971045, 0.7141733};
    // HALs quantise the placeholder to 1/64 steps (max error ~0.004).
    constexpr double kTolerance = 0.01;
    const auto isPlaceholder = [&](const metadata::Matrix3x3& m) {
        if (!m.valid) return false;
        for (size_t i = 0; i < 9; ++i)
            if (std::abs(double(m.rowMajor[i]) - kSrgbD50[i]) > kTolerance) return false;
        return true;
    };
    int repaired = 0;
    const auto repair = [&](metadata::Matrix3x3& forward, const metadata::Matrix3x3& colorTransform, int32_t illuminant) {
        if (!isPlaceholder(forward) || !colorTransform.valid || illuminant < 0) return;
        std::array<double, 9> m{};
        for (size_t i = 0; i < 9; ++i) m[i] = colorTransform.rowMajor[i];
        if (const auto derived = forwardFromColorMatrix(m, uint16_t(illuminant))) {
            forward = toMatrix(*derived);
            ++repaired;
        }
    };
    repair(color.forwardMatrix1, color.colorTransform1, color.referenceIlluminant1);
    repair(color.forwardMatrix2, color.colorTransform2, color.referenceIlluminant2);
    return repaired;
}

std::optional<metadata::StaticColorCalibration> dcpColorCalibration(const DcpProfile& p) {
    const Mat3d identity{1, 0, 0, 0, 1, 0, 0, 0, 1};
    const auto forward1 = p.forwardMatrix1 ? p.forwardMatrix1 : forwardFromColorMatrix(p.colorMatrix1, p.illuminant1);
    if (!forward1) return std::nullopt;
    metadata::StaticColorCalibration c;
    c.referenceIlluminant1 = p.illuminant1;
    c.colorTransform1 = toMatrix(p.colorMatrix1);
    c.calibrationTransform1 = toMatrix(identity);
    c.forwardMatrix1 = toMatrix(*forward1);
    if (p.colorMatrix2) {
        const auto forward2 =
            p.forwardMatrix2 ? p.forwardMatrix2 : forwardFromColorMatrix(*p.colorMatrix2, p.illuminant2);
        if (!forward2) return std::nullopt;
        c.referenceIlluminant2 = p.illuminant2;
        c.colorTransform2 = toMatrix(*p.colorMatrix2);
        c.forwardMatrix2 = toMatrix(*forward2);
    } else {
        // Equal illuminants weigh set 1 fully (reciprocalTemperatureWeight1).
        c.referenceIlluminant2 = p.illuminant1;
        c.colorTransform2 = c.colorTransform1;
        c.forwardMatrix2 = c.forwardMatrix1;
    }
    c.calibrationTransform2 = c.calibrationTransform1;
    // The DCP stage returns profile RGB to the camera basis consumed by
    // tonemap/film; reject a profile that cannot support that inverse.
    if (!math::inverse(c.forwardMatrix1.rowMajor) || !math::inverse(c.forwardMatrix2.rowMajor)) return std::nullopt;
    return c;
}

std::string describe(const DcpProfile& p) {
    std::ostringstream s;
    s << "name=\"" << p.name << "\" illuminants=" << p.illuminant1 << ',' << p.illuminant2
      << " forward=" << (p.forwardMatrix1 ? "yes" : "derived");
    if (!p.hueSatMap1.empty())
        s << " hueSat=" << p.hueSatMap1.hues << 'x' << p.hueSatMap1.sats << 'x' << p.hueSatMap1.vals
          << (p.hueSatMap2.empty() ? "" : " dual");
    if (!p.lookTable.empty()) s << " look=" << p.lookTable.hues << 'x' << p.lookTable.sats << 'x' << p.lookTable.vals;
    if (!p.toneCurve.empty()) s << " toneCurve=" << p.toneCurve.size() / 2;
    return s.str();
}

}  // namespace rawrcam::color
