#include "encoding/dng/DngMetadataAdapter.h"

#include <tinydng.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <vector>

#include "develop/render/DenoiseProfile.h"
#include "geometry/CfaPattern.h"
#include "geometry/OrientationTransform.h"

namespace rawrcam::encoding::dng {
namespace {

// Bayer codes match the legacy BayerPattern order and CfaPattern u32 codes:
// RGGB=0, GRBG=1, GBRG=2, BGGR=3.
using rawrcam::metadata::RectI;
using rawrcam::metadata::SensorGeometry;

bool validRect(const RectI& r) { return r.valid && r.left >= 0 && r.top >= 0 && r.right > r.left && r.bottom > r.top; }
uint32_t rectWidth(const RectI& r) { return static_cast<uint32_t>(r.right - r.left); }
uint32_t rectHeight(const RectI& r) { return static_cast<uint32_t>(r.bottom - r.top); }

// The merge replay text contains tabs/newlines; private extension values must
// be printable ASCII. A JSON string preserves those delimiters for replay.
std::string mergeReplayJson(const std::string& text) {
    std::ostringstream out;
    out << "{\"version\":1,\"replayText\":\"";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\')
            out << '\\' << c;
        else if (c < 0x20 || c >= 0x7f)
            out << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << unsigned(c);
        else
            out << c;
    }
    out << "\"}";
    return out.str();
}

std::string tiffAsciiDescription(std::string text) {
    for (char& ch : text) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c > 0x7f || (c < 0x20 && c != '\n' && c != '\r' && c != '\t') || c == 0x7f) {
            ch = '?';
        }
    }
    return text;
}

std::string exifDateTime(int y, unsigned mo, unsigned d, unsigned h, unsigned mi, unsigned s) {
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%04d:%02u:%02u %02u:%02u:%02u", y, mo, d, h, mi, s);
    return std::string(buf);
}

std::string exifOffset(std::int16_t minutes) {
    const char sign = minutes < 0 ? '-' : '+';
    const int absolute = minutes < 0 ? -static_cast<int>(minutes) : static_cast<int>(minutes);
    char buf[16]{};
    std::snprintf(buf, sizeof(buf), "%c%02d:%02d", sign, absolute / 60, absolute % 60);
    return std::string(buf);
}

std::string exifSubseconds(std::uint32_t nanosecond) {
    char buf[10]{};
    std::snprintf(buf, sizeof(buf), "%09u", nanosecond);
    std::string out(buf);
    while (out.size() > 1 && out.back() == '0') out.pop_back();
    return out;
}

// Quantize a float to {round(v*1e6), 1e6} int32 pair (matches legacy writer).
void quantizeSRational(float value, int32_t& num, int32_t& den) {
    constexpr double denD = 1000000.0;
    const double scaled = std::round(static_cast<double>(value) * denD);
    num = static_cast<int32_t>(
        std::clamp(scaled, double(std::numeric_limits<int32_t>::min()), double(std::numeric_limits<int32_t>::max())));
    den = 1000000;
}

void quantizeURational(float value, uint32_t& num, uint32_t& den) {
    constexpr double denD = 1000000.0;
    const double scaled = std::round(std::max(0.0, static_cast<double>(value)) * denD);
    num = static_cast<uint32_t>(std::min(scaled, double(std::numeric_limits<uint32_t>::max())));
    den = 1000000;
}

std::optional<uint16_t> illuminant(int32_t value) {
    switch (value) {
        case 1:
            return 1;
        case 2:
            return 2;
        case 3:
            return 3;
        case 4:
            return 4;
        case 9:
            return 9;
        case 10:
            return 10;
        case 11:
            return 11;
        case 12:
            return 12;
        case 13:
            return 13;
        case 14:
            return 14;
        case 15:
            return 15;
        case 16:
            return 16;
        case 17:
            return 17;
        case 18:
            return 18;
        case 19:
            return 19;
        case 20:
            return 20;
        case 21:
            return 21;
        case 22:
            return 22;
        case 23:
            return 23;
        case 24:
            return 24;
        default:
            return std::nullopt;
    }
}

std::optional<uint32_t> basePatternCode(int32_t cfa) {
    const auto code = static_cast<std::uint32_t>(cfa);
    if (!rawrcam::geometry::cfaPatternFromU32(code)) return std::nullopt;
    return code;
}

uint32_t shiftedPattern(uint32_t p, uint32_t x, uint32_t y) {
    const bool sx = (x & 1u) != 0;
    const bool sy = (y & 1u) != 0;
    if (!sx && !sy) return p;
    // RGGB=0, GRBG=1, GBRG=2, BGGR=3.
    switch (p) {
        case 0:
            return sy ? (sx ? 3 : 2) : 1;  // RGGB
        case 1:
            return sy ? (sx ? 2 : 3) : 0;  // GRBG
        case 2:
            return sy ? (sx ? 1 : 0) : 3;  // GBRG
        default:
            return sy ? (sx ? 0 : 1) : 2;  // BGGR
    }
}

// 2x2 CFA bytes (0=R,1=G,2=B) row-major for a pattern code.
std::array<uint8_t, 4> cfaBytes(uint32_t p) {
    switch (p) {
        case 0:
            return {0, 1, 1, 2};  // RGGB
        case 1:
            return {1, 0, 2, 1};  // GRBG
        case 2:
            return {1, 2, 0, 1};  // GBRG
        default:
            return {2, 1, 1, 0};  // BGGR
    }
}

std::array<float, 4> blackAtPattern(const std::array<float, 4>& physicalRggb, uint32_t code) {
    return rawrcam::geometry::reorderRggbByCode(physicalRggb, code);
}

struct GeometryMap {
    uint32_t sensorOriginX = 0;
    uint32_t sensorOriginY = 0;
    uint32_t activeTop = 0, activeLeft = 0, activeBottom = 0, activeRight = 0;
    const char* mode = nullptr;
};

struct RectU32 {
    uint32_t top = 0, left = 0, bottom = 0, right = 0;
};

std::optional<GeometryMap> mapGeometry(const rawrcam::imaging::RawSnapshot& frame, bool reconstructedGeometry,
                                       std::string* error) {
    const SensorGeometry& g = frame.metadata.cameraContext->geometry;
    if (reconstructedGeometry) {
        return GeometryMap{0, 0, 0, 0, frame.height, frame.width, "multiframe_reconstruction"};
    }
    if (!validRect(g.preCorrectionActiveArray)) {
        if (error) *error = "missing pre-correction active array";
        return std::nullopt;
    }
    const auto makeIntersection = [&](uint32_t ox, uint32_t oy) -> std::optional<RectU32> {
        const int64_t left = std::max<int64_t>(g.preCorrectionActiveArray.left, ox);
        const int64_t top = std::max<int64_t>(g.preCorrectionActiveArray.top, oy);
        const int64_t right =
            std::min<int64_t>(g.preCorrectionActiveArray.right, static_cast<int64_t>(ox) + frame.width);
        const int64_t bottom =
            std::min<int64_t>(g.preCorrectionActiveArray.bottom, static_cast<int64_t>(oy) + frame.height);
        if (right <= left || bottom <= top) return std::nullopt;
        return RectU32{static_cast<uint32_t>(top - oy), static_cast<uint32_t>(left - ox),
                       static_cast<uint32_t>(bottom - oy), static_cast<uint32_t>(right - ox)};
    };

    if (g.pixelArrayWidth == frame.width && g.pixelArrayHeight == frame.height) {
        auto a = makeIntersection(0, 0);
        if (!a) {
            if (error) *error = "pre-correction active array does not intersect full stored RAW";
            return std::nullopt;
        }
        return GeometryMap{0, 0, a->top, a->left, a->bottom, a->right, "full_pixel_array"};
    }
    if (rectWidth(g.preCorrectionActiveArray) == frame.width &&
        rectHeight(g.preCorrectionActiveArray) == frame.height) {
        return GeometryMap{static_cast<uint32_t>(g.preCorrectionActiveArray.left),
                           static_cast<uint32_t>(g.preCorrectionActiveArray.top),
                           0,
                           0,
                           frame.height,
                           frame.width,
                           "rebased_pre_correction_active"};
    }
    if (validRect(frame.metadata.rawCropRegion) && rectWidth(frame.metadata.rawCropRegion) == frame.width &&
        rectHeight(frame.metadata.rawCropRegion) == frame.height) {
        const uint32_t ox = static_cast<uint32_t>(frame.metadata.rawCropRegion.left);
        const uint32_t oy = static_cast<uint32_t>(frame.metadata.rawCropRegion.top);
        auto a = makeIntersection(ox, oy);
        if (!a) {
            if (error) *error = "RAW crop does not intersect pre-correction active array";
            return std::nullopt;
        }
        return GeometryMap{ox, oy, a->top, a->left, a->bottom, a->right, "scaler_raw_crop_region"};
    }
    if (error) {
        std::ostringstream s;
        s << "unresolved stored RAW geometry: raw=" << frame.width << "x" << frame.height
          << " pixelArray=" << g.pixelArrayWidth << "x" << g.pixelArrayHeight
          << " preCorrection=" << g.preCorrectionActiveArray.left << "," << g.preCorrectionActiveArray.top << ","
          << g.preCorrectionActiveArray.right << "," << g.preCorrectionActiveArray.bottom;
        *error = s.str();
    }
    return std::nullopt;
}

// --- GainMap opcode params (big-endian), ported from the retired native
// DNG writer (opcode serializer + Camera2 gain-map adapter). ---
void be32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xffu));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xffu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xffu));
    out.push_back(static_cast<uint8_t>(v & 0xffu));
}
void be64(std::vector<uint8_t>& out, uint64_t v) {
    for (int s = 56; s >= 0; s -= 8) out.push_back(static_cast<uint8_t>((v >> s) & 0xffu));
}
void beFloat(std::vector<uint8_t>& out, float v) {
    uint32_t u;
    std::memcpy(&u, &v, 4);
    be32(out, u);
}
void beDouble(std::vector<uint8_t>& out, double v) {
    uint64_t u;
    std::memcpy(&u, &v, 8);
    be64(out, u);
}

std::array<uint8_t, 4> cfaMatrix(uint32_t p) {
    switch (p) {
        case 0:
            return {0, 1, 1, 2};
        case 1:
            return {1, 0, 2, 1};
        case 2:
            return {1, 2, 0, 1};
        default:
            return {2, 1, 1, 0};
    }
}

// GainMap params for one channel; returns false on invalid input.
bool appendGainMapChannel(std::vector<uint8_t>& params, uint32_t activeTop, uint32_t activeLeft, uint32_t activeH,
                          uint32_t activeW, uint32_t storedPattern, int channel, uint32_t mapRows, uint32_t mapCols,
                          const std::vector<float>& interleaved) {
    const auto src = cfaMatrix(storedPattern);
    // Phase of `channel` (0=R,1=Ge,2=Go,3=B) within ActiveArea origin.
    int want = -1;
    uint32_t pr = 0, pc = 0;
    for (uint32_t r = 0; r < 2; ++r)
        for (uint32_t c = 0; c < 2; ++c) {
            const uint8_t color = src[((r + (activeTop & 1u)) & 1u) * 2u + ((c + (activeLeft & 1u)) & 1u)];
            if (channel == 0 && color == 0) {
                want = 0;
                pr = r;
                pc = c;
            }
            if (channel == 3 && color == 2) {
                want = 3;
                pr = r;
                pc = c;
            }
            if (channel == 1 && color == 1 && r == 0) {
                want = 1;
                pr = r;
                pc = c;
            }
            if (channel == 2 && color == 1 && r == 1) {
                want = 2;
                pr = r;
                pc = c;
            }
        }
    if (want != channel) return false;
    const double spacingH = 1.0 / double(mapCols - 1u);
    const double spacingV = 1.0 / double(mapRows - 1u);
    be32(params, pr);
    be32(params, pc);
    be32(params, activeH);
    be32(params, activeW);
    be32(params, 0);  // plane
    be32(params, 1);  // planes
    be32(params, 2);  // rowPitch
    be32(params, 2);  // colPitch
    be32(params, mapRows);
    be32(params, mapCols);
    beDouble(params, spacingV);
    beDouble(params, spacingH);
    beDouble(params, 0.0);
    beDouble(params, 0.0);
    be32(params, 1);  // mapPlanes
    for (uint32_t y = 0; y < mapRows; ++y)
        for (uint32_t x = 0; x < mapCols; ++x)
            beFloat(params, interleaved[(size_t(y) * mapCols + x) * 4u + size_t(channel)]);
    return true;
}

// --- DNGPrivateData provenance (byte-identical schema to the legacy writer;
// the renderer keys on the identifier, so it is intentionally unchanged). ---
void appendJsonString(std::string& out, const std::string& s) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out.push_back(static_cast<char>(c));
                break;
        }
    }
    out.push_back('"');
}

struct Provenance {
    std::optional<std::string> applicationName, applicationVersion, applicationBuild, processingPipelineVersion;
    std::optional<std::string> logicalCameraId, physicalCameraId, sensorProfile, calibrationProfileVersion;
    std::optional<std::string> sensorMode, dcgMode, nominalBitDepthMode, vendorCaptureMode, iszMode;
    std::vector<std::string> captureFlags;
    std::vector<std::pair<std::string, std::string>> extensions;
};

std::vector<uint8_t> serializeProvenance(const Provenance& app) {
    const std::string identifier = "RawrCam tinydng provenance";
    std::string json = "{\"schema\":\"tinydng.provenance\",\"version\":1";
    bool first = false;
    const auto field = [&](const char* key, const std::optional<std::string>& v) {
        if (!v) return;
        if (!first) json.push_back(',');
        first = false;
        appendJsonString(json, key);
        json.push_back(':');
        appendJsonString(json, *v);
    };
    field("applicationName", app.applicationName);
    field("applicationVersion", app.applicationVersion);
    field("applicationBuild", app.applicationBuild);
    field("processingPipelineVersion", app.processingPipelineVersion);
    field("logicalCameraId", app.logicalCameraId);
    field("physicalCameraId", app.physicalCameraId);
    field("sensorProfile", app.sensorProfile);
    field("calibrationProfileVersion", app.calibrationProfileVersion);
    field("sensorMode", app.sensorMode);
    field("dcgMode", app.dcgMode);
    field("nominalBitDepthMode", app.nominalBitDepthMode);
    field("vendorCaptureMode", app.vendorCaptureMode);
    field("iszMode", app.iszMode);
    if (!app.captureFlags.empty()) {
        json.push_back(',');
        json += "\"captureFlags\":[";
        for (size_t i = 0; i < app.captureFlags.size(); ++i) {
            if (i) json.push_back(',');
            appendJsonString(json, app.captureFlags[i]);
        }
        json.push_back(']');
    }
    if (!app.extensions.empty()) {
        json.push_back(',');
        json += "\"extensions\":{";
        for (size_t i = 0; i < app.extensions.size(); ++i) {
            if (i) json.push_back(',');
            appendJsonString(json, app.extensions[i].first);
            json.push_back(':');
            appendJsonString(json, app.extensions[i].second);
        }
        json.push_back('}');
    }
    json.push_back('}');
    std::vector<uint8_t> out(identifier.begin(), identifier.end());
    out.push_back(0);
    out.insert(out.end(), json.begin(), json.end());
    return out;
}

}  // namespace

void TinyDngWriteParams::rebind() noexcept {
    raw.unique_camera_model = uniqueModel.empty() ? nullptr : uniqueModel.data();
    exif.make = make.empty() ? nullptr : make.data();
    exif.model = model.empty() ? nullptr : model.data();
    exif.software = software.empty() ? nullptr : software.data();
    exif.datetime = datetime.empty() ? nullptr : datetime.data();
    exif.image_description = description.empty() ? nullptr : description.data();
    exif.datetime_original = dateOriginal.empty() ? nullptr : dateOriginal.data();
    exif.datetime_digitized = dateDigitized.empty() ? nullptr : dateDigitized.data();
    exif.offset_time = offsetTime.empty() ? nullptr : offsetTime.data();
    exif.offset_time_original = offsetOrig.empty() ? nullptr : offsetOrig.data();
    exif.offset_time_digitized = offsetDig.empty() ? nullptr : offsetDig.data();
    exif.subsec_time = subsec.empty() ? nullptr : subsec.data();
    exif.subsec_time_original = subsecOrig.empty() ? nullptr : subsecOrig.data();
    exif.subsec_time_digitized = subsecDig.empty() ? nullptr : subsecDig.data();
    exif.lens_make = lensMake.empty() ? nullptr : lensMake.data();
    exif.lens_model = lensModel.empty() ? nullptr : lensModel.data();
    image.cfa = &cfa;
    image.raw = &raw;
    image.exif = &exif;
    if (!opcodes.empty()) {
        for (size_t i = 0; i < opcodes.size() && i < opcodeParams.size(); ++i) {
            opcodes[i].params = opcodeParams[i].data();
            opcodes[i].params_size = opcodeParams[i].size();
        }
        raw.opcodes = opcodes.data();
    }
    if (!privateData.empty()) {
        raw.rawr_private_data = reinterpret_cast<char*>(privateData.data());
    }
    if (!packedPixels.empty()) {
        pixelData = packedPixels.data();
        image.data = packedPixels.data();
    }
}

tinydng_status writeTinyDngPayload(tinydng_writer* writer, const TinyDngWriteParams& params, tinydng_error* err) {
    const uint32_t width = params.image.width, height = params.image.height;
    const uint64_t tightRow = uint64_t(width) * 2u;
    tinydng_status st = TINYDNG_OK;
    if (params.tiling.tile_width > 0u && params.tiling.tile_length > 0u) {
        const uint32_t tw = params.tiling.tile_width, th = params.tiling.tile_length;
        const uint32_t across = (width + tw - 1u) / tw, down = (height + th - 1u) / th;
        std::vector<uint8_t> tile(size_t(tw) * th * 2u);
        for (uint32_t t = 0; t < across * down && st == TINYDNG_OK; ++t) {
            const uint32_t x = (t % across) * tw, y = (t / across) * th;
            const uint32_t w = std::min(tw, width - x), h = std::min(th, height - y);
            for (uint32_t r = 0; r < h; ++r)
                std::memcpy(tile.data() + size_t(r) * w * 2u, params.pixelData + uint64_t(y + r) * tightRow + uint64_t(x) * 2u,
                            size_t(w) * 2u);
            st = tinydng_writer_write_tile(writer, t, tile.data(), err);
        }
        return st;
    }
    const uint32_t rowsPerStrip = params.tiling.rows_per_strip ? params.tiling.rows_per_strip : height;
    const uint32_t stripCount = (height + rowsPerStrip - 1u) / rowsPerStrip;
    for (uint32_t s = 0; s < stripCount && st == TINYDNG_OK; ++s)
        st = tinydng_writer_write_strip(writer, s, params.pixelData + uint64_t(s) * rowsPerStrip * tightRow, err);
    return st;
}

std::optional<TinyDngWriteParams> makeTinyDngWriteParams(const rawrcam::imaging::RawSnapshot& frame,
                                                         const DngCaptureContext& captureContext, std::string* error) {
    if (!frame.metadata.cameraContext) {
        if (error) *error = "missing pinned camera context";
        return std::nullopt;
    }
    if (frame.raw16.empty() || frame.width == 0 || frame.height == 0) {
        if (error) *error = "empty captured RAW";
        return std::nullopt;
    }
    const auto gm = mapGeometry(frame, captureContext.reconstructedGeometry, error);
    if (!gm) return std::nullopt;
    const auto sensorPattern = basePatternCode(frame.metadata.cameraContext->camera2Cfa);
    if (!sensorPattern) {
        if (error) *error = "unsupported Camera2 CFA";
        return std::nullopt;
    }
    const uint32_t storedPattern = shiftedPattern(*sensorPattern, gm->sensorOriginX, gm->sensorOriginY);
    const uint32_t activePattern = shiftedPattern(storedPattern, gm->activeLeft, gm->activeTop);

    const auto& color = frame.metadata.cameraContext->color;
    const auto illum1 = illuminant(color.referenceIlluminant1);
    if (!illum1 || !color.colorTransform1.valid) {
        if (error) *error = "missing valid primary DNG color calibration";
        return std::nullopt;
    }

    TinyDngWriteParams p{};
    const auto& ctx = *frame.metadata.cameraContext;

    // --- CFA ---
    p.cfa.present = 1;
    p.cfa.pattern_dim[0] = 2;
    p.cfa.pattern_dim[1] = 2;
    const auto bytes = cfaBytes(activePattern);
    for (int i = 0; i < 4; ++i) p.cfa.pattern[i] = bytes[size_t(i)];
    p.cfa.pattern_size = 4;
    p.cfa.plane_color[0] = 0;
    p.cfa.plane_color[1] = 1;
    p.cfa.plane_color[2] = 2;
    p.cfa.plane_color_count = 3;
    p.cfa.layout = 1;

    // --- RAW core ---
    tinydng_raw_info& raw = p.raw;
    raw.has_dng_version = 1;
    raw.dng_version[0] = 1;
    raw.dng_version[1] = 7;
    raw.has_dng_backward_version = 1;
    raw.dng_backward_version[0] = 1;
    raw.dng_backward_version[1] = 1;
    p.uniqueModel =
        captureContext.deviceMake + " " + captureContext.deviceModel + " / " + ctx.lensId + " / camera " + ctx.cameraId;
    raw.has_black_level_exact = 1;
    const auto black = blackAtPattern(frame.metadata.blackLevelPhysicalRggb, activePattern);
    for (int i = 0; i < 4; ++i) raw.black_level_exact[i] = black[size_t(i)];
    raw.black_level_present = 1;
    raw.has_black_level_repeat = 1;
    raw.black_level_repeat[0] = 2;
    raw.black_level_repeat[1] = 2;
    raw.white_level_present = 1;
    raw.white_level[0] = std::max(1, int(std::round(frame.metadata.effectiveWhiteLevel)));
    raw.has_active_area = 1;
    raw.active_area[0] = gm->activeTop;
    raw.active_area[1] = gm->activeLeft;
    raw.active_area[2] = gm->activeBottom;
    raw.active_area[3] = gm->activeRight;
    raw.has_crop = 1;
    raw.crop_origin[0] = 0;
    raw.crop_origin[1] = 0;
    raw.crop_size[0] = double(gm->activeRight - gm->activeLeft);
    raw.crop_size[1] = double(gm->activeBottom - gm->activeTop);
    raw.has_default_scale = 1;
    raw.default_scale[0] = 1.0;
    raw.default_scale[1] = 1.0;
    raw.color_matrix_present = 1;
    for (int i = 0; i < 9; ++i) raw.color_matrix1[i] = color.colorTransform1.rowMajor[size_t(i)];
    if (color.calibrationTransform1.valid) {
        for (int i = 0; i < 9; ++i) raw.camera_calibration1[i] = color.calibrationTransform1.rowMajor[size_t(i)];
        raw.camera_calibration_present = 1;
    }
    if (color.forwardMatrix1.valid) {
        for (int i = 0; i < 9; ++i) raw.forward_matrix1[i] = color.forwardMatrix1.rowMajor[size_t(i)];
        raw.has_forward_matrix1 = 1;
    }
    const auto illum2 = illuminant(color.referenceIlluminant2);
    if (illum2 && color.colorTransform2.valid) {
        raw.calibration_illuminant2 = *illum2;
        for (int i = 0; i < 9; ++i) raw.color_matrix2[i] = color.colorTransform2.rowMajor[size_t(i)];
        raw.has_color_matrix2 = 1;
        if (color.calibrationTransform2.valid) {
            for (int i = 0; i < 9; ++i) raw.camera_calibration2[i] = color.calibrationTransform2.rowMajor[size_t(i)];
            raw.has_camera_calibration2 = 1;
        }
        if (color.forwardMatrix2.valid) {
            for (int i = 0; i < 9; ++i) raw.forward_matrix2[i] = color.forwardMatrix2.rowMajor[size_t(i)];
            raw.has_forward_matrix2 = 1;
        }
    }
    raw.calibration_illuminant1 = *illum1;
    raw.has_analog_balance = 1;
    raw.analog_balance[0] = raw.analog_balance[1] = raw.analog_balance[2] = 1.0;

    // AsShotNeutral (metadata only): RAW bytes are never white-balanced.
    const bool manualWb = frame.metadata.awbMode == 0;
    bool manualNeutralWritten = false;
    if (manualWb) {
        const auto& g = frame.metadata.colorCorrectionGainsRggb;
        const float green = 0.5f * (g[1] + g[2]);
        if (g[0] > 1.0e-6f && green > 1.0e-6f && g[3] > 1.0e-6f && std::isfinite(g[0]) && std::isfinite(green) &&
            std::isfinite(g[3])) {
            const float invR = 1.0f / g[0];
            const float invG = 1.0f / green;
            const float invB = 1.0f / g[3];
            const float peak = std::max({invR, invG, invB});
            if (peak > 1.0e-6f && std::isfinite(peak)) {
                raw.as_shot_neutral[0] = double(invR / peak);
                raw.as_shot_neutral[1] = double(invG / peak);
                raw.as_shot_neutral[2] = double(invB / peak);
                raw.has_as_shot_neutral = 1;
                manualNeutralWritten = true;
            }
        }
    }
    if (!manualNeutralWritten && frame.metadata.hasNeutralColorPoint) {
        for (int i = 0; i < 3; ++i) raw.as_shot_neutral[i] = frame.metadata.neutralColorPoint[size_t(i)];
        raw.has_as_shot_neutral = 1;
    }
    if (captureContext.baselineExposureEV && std::isfinite(*captureContext.baselineExposureEV)) {
        raw.baseline_exposure = *captureContext.baselineExposureEV;
        raw.has_baseline_exposure = 1;
    }

    // Lens (IFD0 LensInfo + EXIF sidecar).
    if (!ctx.availableFocalLengthsMm.empty()) {
        const auto [fminIt, fmaxIt] =
            std::minmax_element(ctx.availableFocalLengthsMm.begin(), ctx.availableFocalLengthsMm.end());
        const float amin = ctx.availableApertures.empty()
                               ? 0.0f
                               : *std::min_element(ctx.availableApertures.begin(), ctx.availableApertures.end());
        const float amax = ctx.availableApertures.empty()
                               ? 0.0f
                               : *std::max_element(ctx.availableApertures.begin(), ctx.availableApertures.end());
        raw.lens_info[0] = *fminIt;
        raw.lens_info[1] = *fmaxIt;
        raw.lens_info[2] = amin > 0 ? amin : 0;
        raw.lens_info[3] = amax > 0 ? amax : 0;
        raw.has_lens_info = 1;
    }
    if (!captureContext.deviceMake.empty()) p.lensMake = captureContext.deviceMake;
    if (!captureContext.deviceModel.empty()) p.lensModel = captureContext.deviceModel;

    if (frame.metadata.sensorNoiseProfile.size() >= 8) {
        const auto& n = frame.metadata.sensorNoiseProfile;
        std::array<std::array<double, 2>, 4> physical{};
        const auto assign = [&](int p0, int p1, int p2, int p3) {
            physical[0] = {n[size_t(2 * p0)], n[size_t(2 * p0 + 1)]};
            physical[1] = {n[size_t(2 * p1)], n[size_t(2 * p1 + 1)]};
            physical[2] = {n[size_t(2 * p2)], n[size_t(2 * p2 + 1)]};
            physical[3] = {n[size_t(2 * p3)], n[size_t(2 * p3 + 1)]};
        };
        switch (ctx.camera2Cfa) {
            case 0:
                assign(0, 1, 2, 3);
                break;
            case 1:
                assign(1, 0, 3, 2);
                break;
            case 2:
                assign(2, 3, 0, 1);
                break;
            case 3:
                assign(3, 2, 1, 0);
                break;
            default:
                break;
        }
        raw.noise_profile[0] = physical[0][0];
        raw.noise_profile[1] = physical[0][1];
        raw.noise_profile[2] = (physical[1][0] + physical[2][0]) * 0.5;
        raw.noise_profile[3] = (physical[1][1] + physical[2][1]) * 0.5;
        raw.noise_profile[4] = physical[3][0];
        raw.noise_profile[5] = physical[3][1];
        raw.noise_profile_count = 6;
    }

    // GainMap OpcodeList2 from the Camera2 lens-shading map.
    const bool shadingAlreadyApplied = ctx.hasLensShadingApplied && ctx.lensShadingApplied;
    if (!frame.metadata.lensShadingMap.empty() && !shadingAlreadyApplied) {
        const auto& ls = frame.metadata.lensShadingMap;
        const uint32_t cols = frame.metadata.lensShadingMapWidth;
        const uint32_t rows = frame.metadata.lensShadingMapHeight;
        const uint64_t points = uint64_t(cols) * uint64_t(rows);
        bool valid = cols >= 2 && rows >= 2 && points <= 65536u && ls.size() == size_t(points) * 4u;
        if (valid)
            for (float v : ls)
                if (!std::isfinite(v) || v < 1.0f) {
                    valid = false;
                    break;
                }
        if (!valid) {
            if (error) *error = "lens shading: invalid gain map";
            return std::nullopt;
        }
        const uint32_t activeW = gm->activeRight - gm->activeLeft;
        const uint32_t activeH = gm->activeBottom - gm->activeTop;
        for (int ch = 0; ch < 4; ++ch) {
            std::vector<uint8_t> params;
            if (!appendGainMapChannel(params, gm->activeTop, gm->activeLeft, activeH, activeW, storedPattern, ch, rows,
                                      cols, ls)) {
                if (error) *error = "lens shading: CFA phase resolution failed";
                return std::nullopt;
            }
            p.opcodeParams.push_back(std::move(params));
        }
        p.opcodes.reserve(4);
        for (int ch = 0; ch < 4; ++ch) {
            tinydng_opcode op{};
            op.list = 2;
            op.id = 9;
            op.version = 0x01030000u;
            op.flags = 1;  // optional, matches legacy writer policy
            op.params = p.opcodeParams[size_t(ch)].data();
            op.params_size = p.opcodeParams[size_t(ch)].size();
            p.opcodes.push_back(op);
        }
        raw.opcodes = p.opcodes.data();
        raw.opcode_count = p.opcodes.size();
    }

    // --- EXIF sidecar ---
    tinydng_exif& ex = p.exif;
    if (!captureContext.deviceMake.empty()) p.make = captureContext.deviceMake;
    if (!captureContext.deviceModel.empty()) p.model = captureContext.deviceModel;
    p.software = "RawrCam";
    p.description = tiffAsciiDescription(captureContext.imageDescription.empty() ? "Captured with Rawr"
                                                                                 : captureContext.imageDescription);
    switch (rawrcam::geometry::presentationQuarterTurns(ctx.geometry.sensorOrientationDegrees,
                                                        captureContext.deviceRotationDegrees)) {
        case 1u:
            ex.orientation = 6;
            break;  // RightTop
        case 2u:
            ex.orientation = 3;
            break;  // BottomRight
        case 3u:
            ex.orientation = 8;
            break;  // LeftBottom
        default:
            ex.orientation = 1;
            break;  // TopLeft
    }
    if (frame.metadata.exposureTimeNs > 0) {
        const uint64_t us = static_cast<uint64_t>(frame.metadata.exposureTimeNs + 500) / 1000u;
        if (us <= std::numeric_limits<uint32_t>::max()) {
            ex.exposure_time[0] = int32_t(us);
            ex.exposure_time[1] = 1000000;
            ex.has_exposure_time = 1;
            const double seconds = double(frame.metadata.exposureTimeNs) * 1.0e-9;
            if (seconds > 0.0) {
                quantizeSRational(float(-std::log2(seconds)), ex.shutter_speed[0], ex.shutter_speed[1]);
                ex.has_shutter_speed = 1;
            }
        }
    }
    if (frame.metadata.sensitivity > 0 && frame.metadata.sensitivity <= 65534) {
        ex.iso = frame.metadata.sensitivity;
        ex.has_iso = 1;
    }
    if (frame.metadata.aperture && *frame.metadata.aperture > 0.0f) {
        quantizeURational(*frame.metadata.aperture, ex.fnumber_rational[0], ex.fnumber_rational[1]);
        ex.has_fnumber = 1;
        quantizeURational(float(2.0 * std::log2(*frame.metadata.aperture)), ex.max_aperture_rational[0],
                          ex.max_aperture_rational[1]);
        // ApertureValue written from the same float path as legacy (unsigned).
        quantizeSRational(float(2.0 * std::log2(*frame.metadata.aperture)), ex.aperture_value[0], ex.aperture_value[1]);
        ex.has_aperture_value = 1;
    }
    if (!ctx.availableApertures.empty()) {
        const float amin = *std::min_element(ctx.availableApertures.begin(), ctx.availableApertures.end());
        if (amin > 0.0f) {
            quantizeURational(float(2.0 * std::log2(amin)), ex.max_aperture_rational[0], ex.max_aperture_rational[1]);
            ex.has_max_aperture_value = 1;
        }
    }
    if (frame.metadata.focalLengthMm && *frame.metadata.focalLengthMm > 0.0f) {
        quantizeURational(*frame.metadata.focalLengthMm, ex.focal_length_rational[0], ex.focal_length_rational[1]);
        ex.has_focal_length = 1;
        const double sw = ctx.sensorPhysicalSizeMm[0], sh = ctx.sensorPhysicalSizeMm[1];
        if (sw > 0.0 && sh > 0.0) {
            const double eq =
                *frame.metadata.focalLengthMm * std::sqrt(36.0 * 36.0 + 24.0 * 24.0) / std::sqrt(sw * sw + sh * sh);
            if (eq >= 1.0 && eq <= 65535.0) {
                ex.focal_length_35mm = uint16_t(std::lround(eq));
                ex.has_focal_length_35mm = 1;
            }
        }
    }
    if (frame.metadata.focusDistanceDiopters && *frame.metadata.focusDistanceDiopters > 0.000001f) {
        quantizeURational(1.0f / *frame.metadata.focusDistanceDiopters, ex.subject_distance_rational[0],
                          ex.subject_distance_rational[1]);
        ex.has_subject_distance = 1;
    }
    ex.exposure_mode = uint16_t(frame.metadata.aeMode == 0 ? 1 : 0);
    ex.has_exposure_mode = 1;
    ex.white_balance = uint16_t(frame.metadata.awbMode == 0 ? 1 : 0);
    ex.has_white_balance = 1;
    ex.sensing_method = 2;
    ex.has_sensing_method = 1;
    if (frame.metadata.flashState == 3 || frame.metadata.flashState == 4) {
        ex.flash = 1;
    } else {
        ex.flash = 0;
    }
    ex.has_flash = 1;
    if (validRect(frame.metadata.scalerCropRegion) && validRect(ctx.geometry.activeArray)) {
        const double fullW = rectWidth(ctx.geometry.activeArray);
        const double cropW = rectWidth(frame.metadata.scalerCropRegion);
        if (cropW > 0.0 && fullW >= cropW) {
            quantizeURational(float(fullW / cropW), ex.digital_zoom_rational[0], ex.digital_zoom_rational[1]);
            ex.has_digital_zoom = 1;
        }
    }
    if (captureContext.wallClockUnixMillis > 0) {
        const time_t localSeconds = static_cast<time_t>(captureContext.wallClockUnixMillis / 1000 +
                                                        int64_t(captureContext.utcOffsetMinutes) * 60);
        struct tm local{};
        if (gmtime_r(&localSeconds, &local)) {
            p.datetime = exifDateTime(local.tm_year + 1900, unsigned(local.tm_mon + 1), unsigned(local.tm_mday),
                                      unsigned(local.tm_hour), unsigned(local.tm_min), unsigned(local.tm_sec));
            p.dateOriginal = p.datetime;
            p.dateDigitized = p.datetime;
            // Legacy writer emits OffsetTime* only when the offset is non-zero.
            if (captureContext.utcOffsetMinutes) {
                p.offsetTime = exifOffset(captureContext.utcOffsetMinutes);
                p.offsetOrig = p.offsetTime;
                p.offsetDig = p.offsetTime;
            }
            const uint32_t ns = uint32_t(captureContext.wallClockUnixMillis % 1000) * 1000000u;
            if (ns) {
                p.subsec = exifSubseconds(ns);
                p.subsecOrig = p.subsec;
                p.subsecDig = p.subsec;
            }
        }
    }
    if (!ctx.availableFocalLengthsMm.empty()) {
        const auto [fminIt, fmaxIt] =
            std::minmax_element(ctx.availableFocalLengthsMm.begin(), ctx.availableFocalLengthsMm.end());
        ex.lens_spec[0] = *fminIt;
        ex.lens_spec[1] = *fmaxIt;
        if (!ctx.availableApertures.empty()) {
            const float amin = *std::min_element(ctx.availableApertures.begin(), ctx.availableApertures.end());
            const float amax = *std::max_element(ctx.availableApertures.begin(), ctx.availableApertures.end());
            ex.lens_spec[2] = amin > 0 ? amin : 0;
            ex.lens_spec[3] = amax > 0 ? amax : 0;
        }
        ex.has_lens_spec = 1;
    }

    // --- DNGPrivateData provenance ---
    Provenance prov{};
    prov.applicationName = "RawrCam";
    prov.processingPipelineVersion = "rawr-still-1";
    prov.logicalCameraId = ctx.cameraId;
    prov.physicalCameraId = ctx.cameraId;
    prov.sensorMode = gm->mode;
    prov.captureFlags = captureContext.reconstructedGeometry
                            ? std::vector<std::string>{"gpu_multiframe_reconstruction", "paired_sensor_timestamp"}
                            : std::vector<std::string>{"cpu_snapshot", "paired_sensor_timestamp"};
    if (!captureContext.processingRecipe.empty())
        prov.extensions.emplace_back("com.rawrcam.processing.recipe.v1", captureContext.processingRecipe);
    if (!captureContext.resolvedRecipe.empty())
        prov.extensions.emplace_back("com.rawrcam.processing.resolved.v1", captureContext.resolvedRecipe);
    prov.extensions.emplace_back("com.rawrcam.processing.source_role", captureContext.sourceRole);
    prov.extensions.emplace_back("com.rawrcam.processing.applied",
                                 captureContext.sourceRole == "merged" ? "multiframe_merge" : "sensor_raw");
    if (!captureContext.mergeReplayMetadata.empty())
        prov.extensions.emplace_back("com.rawrcam.processing.merge.v1",
                                     mergeReplayJson(captureContext.mergeReplayMetadata));
    {
        std::ostringstream frameRecipe;
        frameRecipe.imbue(std::locale::classic());
        frameRecipe << std::setprecision(9) << "{\"version\":1,\"timestampNs\":" << frame.timestampNs
                    << ",\"whiteBalanceRggb\":[";
        for (size_t i = 0; i < frame.colorState.baselineWbRggb.size(); ++i)
            frameRecipe << (i ? "," : "") << frame.colorState.baselineWbRggb[i];
        frameRecipe << "],\"cameraToLinearSrgbRowMajor\":[";
        for (size_t i = 0; i < frame.colorState.cameraToLinearSrgbRowMajor.size(); ++i)
            frameRecipe << (i ? "," : "") << frame.colorState.cameraToLinearSrgbRowMajor[i];
        frameRecipe << "]}";
        prov.extensions.emplace_back("com.rawrcam.processing.frame.v1", frameRecipe.str());
    }
    if (captureContext.denoiseEnabled) {
        float denoiseA = 0.0f, denoiseB = 0.0f;
        std::ostringstream denoise;
        denoise.imbue(std::locale::classic());
        denoise << std::setprecision(4) << "{\"version\":1,\"enabled\":";
        if (develop::rendered::resolveDenoiseNoise(frame.metadata, denoiseA, denoiseB)) {
            denoise << "true,\"strength\":" << captureContext.denoiseStrength
                    << ",\"detail\":" << captureContext.denoiseDetail << std::setprecision(9) << ",\"a\":" << denoiseA
                    << ",\"b\":" << denoiseB << "}";
        } else {
            denoise << "false,\"strength\":" << captureContext.denoiseStrength
                    << ",\"detail\":" << captureContext.denoiseDetail << "}";
        }
        prov.extensions.emplace_back("com.rawrcam.processing.denoise.v1", denoise.str());
    }
    prov.extensions.emplace_back("com.rawrcam.sensor.timestamp_ns", std::to_string(frame.timestampNs));
    prov.extensions.emplace_back("com.rawrcam.source.row_stride_bytes", std::to_string(frame.sourceRowStrideBytes));
    prov.extensions.emplace_back("com.rawrcam.geometry.mapping", gm->mode);
    prov.extensions.emplace_back("com.rawrcam.white.effective_source", rawrcam::metadata::effectiveWhiteLevelSourceName(
                                                                           frame.metadata.effectiveWhiteLevelSource));
    prov.extensions.emplace_back("com.rawrcam.white.effective", std::to_string(frame.metadata.effectiveWhiteLevel));
    if (frame.metadata.reportedDynamicWhiteLevel)
        prov.extensions.emplace_back("com.rawrcam.white.reported_dynamic",
                                     std::to_string(*frame.metadata.reportedDynamicWhiteLevel));
    if (ctx.maxAnalogSensitivity > 0)
        prov.extensions.emplace_back("com.rawrcam.sensor.max_analog_sensitivity",
                                     std::to_string(ctx.maxAnalogSensitivity));
    if (frame.metadata.requestedSensitivity)
        prov.extensions.emplace_back("com.rawrcam.sensitivity.requested",
                                     std::to_string(*frame.metadata.requestedSensitivity));
    prov.extensions.emplace_back("com.rawrcam.sensitivity.reported", std::to_string(frame.metadata.sensitivity));
    if (ctx.hasLensShadingApplied)
        prov.extensions.emplace_back("com.rawrcam.shading.applied", ctx.lensShadingApplied ? "true" : "false");
    if (!frame.metadata.lensShadingMap.empty() && shadingAlreadyApplied)
        prov.extensions.emplace_back("com.rawrcam.shading.gainmap_skipped", "already_applied");
    if (ctx.hasGreenSplit)
        prov.extensions.emplace_back("com.rawrcam.sensor.green_split", std::to_string(ctx.greenSplit));
    if (!ctx.opticalBlackRegions.empty())
        prov.extensions.emplace_back("com.rawrcam.optical_black.regions",
                                     std::to_string(ctx.opticalBlackRegions.size()));
    if (frame.metadata.flashState >= 0)
        prov.extensions.emplace_back("com.rawrcam.flash.state", std::to_string(frame.metadata.flashState));
    if (ctx.hasFlashInfoAvailable)
        prov.extensions.emplace_back("com.rawrcam.flash.available", ctx.flashInfoAvailable ? "true" : "false");
    if (!ctx.lensDistortion.empty()) prov.extensions.emplace_back("com.rawrcam.lens.distortion_present", "true");
    if (!ctx.lensIntrinsicCalibration.empty())
        prov.extensions.emplace_back("com.rawrcam.lens.intrinsic_present", "true");
    if (!frame.metadata.hotPixelMap.empty())
        prov.extensions.emplace_back("com.rawrcam.hot_pixel.count",
                                     std::to_string(frame.metadata.hotPixelMap.size() / 2));
    p.privateData = serializeProvenance(prov);
    raw.rawr_private_data = reinterpret_cast<char*>(p.privateData.data());
    raw.rawr_private_size = p.privateData.size();

    // --- Bind owned strings (rebind() also repairs pointers after moves) ---
    p.rebind();

    // --- Pixels (tighten stride when padded) ---
    const uint64_t tightRow = uint64_t(frame.width) * 2u;
    if (frame.packedRowStrideBytes == tightRow) {
        p.pixelData = frame.raw16.data();
        p.pixelBytes = frame.raw16.size();
    } else {
        if (frame.packedRowStrideBytes < tightRow) {
            if (error) *error = "packed row stride smaller than row";
            return std::nullopt;
        }
        p.packedPixels.resize(size_t(tightRow) * frame.height);
        for (uint32_t y = 0; y < frame.height; ++y) {
            std::memcpy(p.packedPixels.data() + size_t(y) * size_t(tightRow),
                        frame.raw16.data() + size_t(y) * size_t(frame.packedRowStrideBytes), size_t(tightRow));
        }
        p.pixelData = p.packedPixels.data();
        p.pixelBytes = p.packedPixels.size();
    }
    if (p.pixelBytes < size_t(tightRow) * frame.height) {
        if (error) *error = "RAW buffer smaller than geometry";
        return std::nullopt;
    }

    // --- Image + options ---
    tinydng_write_image& img = p.image;
    img.width = frame.width;
    img.height = frame.height;
    img.samples_per_pixel = 1;
    img.bits_per_sample = 16;
    img.photometric = 0;  // auto -> CFA
    img.data = p.pixelData;
    img.data_size = size_t(tightRow) * frame.height;
    img.cfa = &p.cfa;
    img.raw = &p.raw;
    img.exif = &p.exif;

    const bool ljpeg = captureContext.compression != DngCompression::Uncompressed;
    p.options.big_endian = 0;
    p.options.as_dng = 1;
    p.options.compression = ljpeg ? 7 : 1;
    p.options.ljpeg_predictor = 1;
    const uint32_t rowBytes = uint32_t(tightRow);
    if (ljpeg) {
        // Lossless JPEG uses 256x256 tiles like Adobe DNG Converter: valid
        // multi-strip LJPEG decodes in the Adobe SDK, but LibRaw/RawTherapee
        // only read the first strip of a compressed DNG.
        p.tiling.tile_width = kLjpegTileSize;
        p.tiling.tile_length = kLjpegTileSize;
        p.tiling.rows_per_strip = 0;
    } else {
        // Uncompressed keeps the legacy 64KB/strip layout.
        p.tiling.tile_width = 0;
        p.tiling.tile_length = 0;
        p.tiling.rows_per_strip = std::max<uint32_t>(1u, 65536u / rowBytes);
        if (p.tiling.rows_per_strip > frame.height) p.tiling.rows_per_strip = frame.height;
    }

    return p;
}

}  // namespace rawrcam::encoding::dng
