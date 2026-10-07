#include "encoding/dng/DngMetadataAdapter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <vector>

#include "encoding/dng/DngExif.h"
#include "encoding/dng/DngGainMapOpcode.h"
#include "encoding/dng/DngProvenance.h"
#include "geometry/CfaPattern.h"

namespace rawrcam::encoding::dng {
namespace {

using rawrcam::metadata::rectHeight;
using rawrcam::metadata::rectWidth;
using rawrcam::metadata::SensorGeometry;
using rawrcam::metadata::validRect;

// EXIF LightSource codes accepted as DNG CalibrationIlluminant.
std::optional<uint16_t> illuminant(int32_t value) {
    if ((value >= 1 && value <= 4) || (value >= 9 && value <= 24)) return static_cast<uint16_t>(value);
    return std::nullopt;
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
    const auto& ctx = *frame.metadata.cameraContext;
    const auto sensorCode = static_cast<uint32_t>(ctx.camera2Cfa);
    if (!rawrcam::geometry::cfaPatternFromU32(sensorCode)) {
        if (error) *error = "unsupported Camera2 CFA";
        return std::nullopt;
    }
    using rawrcam::geometry::shiftCfaPatternCode;
    const uint32_t storedPattern = shiftCfaPatternCode(sensorCode, gm->sensorOriginX, gm->sensorOriginY);
    const uint32_t activePattern = shiftCfaPatternCode(storedPattern, gm->activeLeft, gm->activeTop);

    const auto& color = ctx.color;
    const auto illum1 = illuminant(color.referenceIlluminant1);
    if (!illum1 || !color.colorTransform1.valid) {
        if (error) *error = "missing valid primary DNG color calibration";
        return std::nullopt;
    }

    TinyDngWriteParams p{};

    // --- CFA ---
    p.cfa.present = 1;
    p.cfa.pattern_dim[0] = 2;
    p.cfa.pattern_dim[1] = 2;
    const auto bytes = rawrcam::geometry::cfaColorIndices(activePattern);
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
    const auto black = rawrcam::geometry::reorderRggbByCode(frame.metadata.blackLevelPhysicalRggb, activePattern);
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
        // BlackLevel is exact; a converter's automatic black subtraction
        // would be amplified by the lift and crush the shadows.
        if (raw.baseline_exposure > 0.0) {
            raw.default_black_render = 1;  // None
            raw.has_default_black_render = 1;
        }
    }

    // Lens (IFD0 LensInfo; the EXIF LensSpecification lives in DngExif).
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
        const DngGainMapArea area{gm->activeTop, gm->activeLeft, gm->activeBottom - gm->activeTop,
                                  gm->activeRight - gm->activeLeft, storedPattern};
        if (!appendLensShadingGainMaps(p, frame.metadata, area, error)) return std::nullopt;
    }

    fillDngExif(p, frame, captureContext);

    p.privateData = buildDngProvenance(frame, captureContext, gm->mode);
    raw.rawr_private_data = reinterpret_cast<char*>(p.privateData.data());
    raw.rawr_private_size = p.privateData.size();

    // --- Bind owned strings (rebind() also repairs pointers after moves) ---
    p.rebind();

    if (!bindDngPixelPayload(p, frame, captureContext.compression, error)) return std::nullopt;
    return p;
}

}  // namespace rawrcam::encoding::dng
