#include "SourceCaptureExif.h"
#include "metadata/ExifTime.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string_view>

namespace rawrcam::renderer {
namespace {
std::string text(const char* image, const char* root) {
    auto value = metadata::trimExifText(image);
    return value.empty() ? metadata::trimExifText(root) : value;
}
std::optional<double> ratio(const int32_t (&value)[2], bool present, bool allowZero = false) {
    if (!present || value[0] < 0 || (!allowZero && value[0] == 0) || value[1] <= 0) return std::nullopt;
    return double(value[0]) / double(value[1]);
}
std::optional<double> unsignedRatio(const uint32_t (&value)[2], const int32_t (&legacy)[2],
                                   bool present, bool allowZero = false) {
    if (!present) return std::nullopt;
    if (!value[1]) return ratio(legacy, present, allowZero);
    if (!allowZero && !value[0]) return std::nullopt;
    return double(value[0]) / double(value[1]);
}
void copyUnsignedRatio(uint32_t (&output)[2], const uint32_t (&value)[2], const int32_t (&legacy)[2]) {
    if (value[1]) std::copy_n(value, 2, output);
    else std::copy_n(legacy, 2, output);
}
std::optional<std::int64_t> exposure(const tinydng_exif& e) {
    const auto seconds = ratio(e.exposure_time, e.has_exposure_time);
    if (!seconds || !std::isfinite(*seconds)) return std::nullopt;
    return std::int64_t(*seconds * 1.0e9);
}
std::optional<std::int32_t> iso(const tinydng_exif& e) {
    if (!e.has_iso || e.iso == 0 || e.iso > 65535) return std::nullopt;
    return std::int32_t(e.iso);
}
std::optional<float> fNumber(const tinydng_exif& e) {
    if (const auto f = unsignedRatio(e.fnumber_rational, e.fnumber, e.has_fnumber); f && *f <= 1000.0) return float(*f);
    if (!e.has_aperture_value || e.aperture_value[1] <= 0) return std::nullopt;
    const double f = std::pow(2.0, double(e.aperture_value[0]) / double(e.aperture_value[1]) / 2.0);
    if (!std::isfinite(f) || f <= 0.0 || f > 1000.0) return std::nullopt;
    return float(f);
}
std::optional<float> focal(const tinydng_exif& e) {
    const auto mm = unsignedRatio(e.focal_length_rational, e.focal_length, e.has_focal_length);
    if (!mm || *mm > 100000.0) return std::nullopt;
    return float(*mm);
}
std::optional<std::uint16_t> focal35(const tinydng_exif& e) {
    if (!e.has_focal_length_35mm || e.focal_length_35mm == 0) return std::nullopt;
    return e.focal_length_35mm;
}
std::optional<std::array<std::uint32_t, 2>> digitalZoom(const tinydng_exif& e) {
    if (!e.has_digital_zoom || e.digital_zoom_rational[1] == 0) return std::nullopt;
    return std::array<std::uint32_t, 2>{e.digital_zoom_rational[0], e.digital_zoom_rational[1]};
}
std::optional<std::array<double, 4>> lensSpec(const tinydng_exif& e) {
    if (!e.has_lens_spec) return std::nullopt;
    bool any = false;
    for (double v : e.lens_spec) {
        if (!std::isfinite(v) || v < 0.0) return std::nullopt;
        any |= v > 0.0;
    }
    if (!any) return std::nullopt;
    return std::array<double, 4>{e.lens_spec[0], e.lens_spec[1], e.lens_spec[2], e.lens_spec[3]};
}
template <typename T, typename F>
std::optional<T> either(const tinydng_exif& image, const tinydng_exif* root, F read) {
    if (auto value = read(image)) return value;
    return root ? read(*root) : std::nullopt;
}

std::optional<std::array<double, 4>> sourceLensSpec(const tinydng_exif& image, const tinydng_exif* root,
                                                  const tinydng_raw_info* rootRaw) {
    if (auto specification = either<std::array<double, 4>>(image, root, lensSpec)) return specification;
    if (!rootRaw || !rootRaw->has_lens_info) return std::nullopt;
    tinydng_exif fallback{};
    std::copy_n(rootRaw->lens_info, 4, fallback.lens_spec);
    fallback.has_lens_spec = 1;
    return lensSpec(fallback);
}
}  // namespace

SourceCaptureExif readSourceCaptureExif(const tinydng_exif& image, const tinydng_exif* root,
                                        const char* uniqueCameraModel, const tinydng_raw_info* rootRaw) {
    const auto rootText = [&](char* tinydng_exif::*field) { return root ? root->*field : nullptr; };
    SourceCaptureExif out;
    out.make = text(image.make, rootText(&tinydng_exif::make));
    out.model = text(image.model, rootText(&tinydng_exif::model));
    if (out.model.empty()) out.model = metadata::trimExifText(uniqueCameraModel);
    out.lensMake = text(image.lens_make, rootText(&tinydng_exif::lens_make));
    out.lensModel = text(image.lens_model, rootText(&tinydng_exif::lens_model));
    out.lensSerial = text(image.lens_serial, rootText(&tinydng_exif::lens_serial));
    out.lensSpecification = sourceLensSpec(image, root, rootRaw);
    out.apertureValue = either<double>(image, root, [](const auto& exif) {
        return ratio(exif.aperture_value, exif.has_aperture_value, true);
    });
    out.maxApertureValue = either<double>(image, root, [](const auto& exif) {
        return unsignedRatio(exif.max_aperture_rational, exif.max_aperture_value, exif.has_max_aperture_value, true);
    });
    out.fNumber = either<float>(image, root, fNumber);
    out.focalLengthMm = either<float>(image, root, focal);
    out.focalLength35mm = either<std::uint16_t>(image, root, focal35);
    out.digitalZoomRatio = either<std::array<std::uint32_t, 2>>(image, root, digitalZoom);
    out.exposureTimeNs = either<std::int64_t>(image, root, exposure);
    out.sensitivity = either<std::int32_t>(image, root, iso);
    auto original = metadata::readExifTime(image, root, &tinydng_exif::datetime_original,
                                          &tinydng_exif::offset_time_original, &tinydng_exif::subsec_time_original);
    out.dateTimeOriginal = std::move(original.dateTime);
    out.offsetTimeOriginal = std::move(original.offset);
    out.subsecTimeOriginal = std::move(original.subsec);
    auto digitized = metadata::readExifTime(image, root, &tinydng_exif::datetime_digitized,
                                           &tinydng_exif::offset_time_digitized, &tinydng_exif::subsec_time_digitized);
    out.dateTimeDigitized = std::move(digitized.dateTime);
    out.offsetTimeDigitized = std::move(digitized.offset);
    out.subsecTimeDigitized = std::move(digitized.subsec);
    return out;
}

void preserveSourceLensExif(tinydng_exif& output, const tinydng_exif& image,
                            const tinydng_exif* root, const tinydng_raw_info* rootRaw) {
    const auto select = [&](auto present) -> const tinydng_exif* {
        if (present(image)) return &image;
        return root && present(*root) ? root : nullptr;
    };
    const auto sourceText = [&](char* tinydng_exif::*field) {
        const auto* source = select([&](const auto& exif) {
            const char* value = exif.*field;
            return value && std::string_view(value).find_first_not_of(" \t\r\n") != std::string_view::npos;
        });
        return source ? source->*field : nullptr;
    };
    output.lens_make = sourceText(&tinydng_exif::lens_make);
    output.lens_model = sourceText(&tinydng_exif::lens_model);
    output.lens_serial = sourceText(&tinydng_exif::lens_serial);
    const auto specification = sourceLensSpec(image, root, rootRaw);
    output.has_lens_spec = specification.has_value();
    if (specification) std::copy(specification->begin(), specification->end(), output.lens_spec);
    const auto* source = select([](const auto& exif) {
        return unsignedRatio(exif.fnumber_rational, exif.fnumber, exif.has_fnumber).has_value();
    });
    output.has_fnumber = source != nullptr;
    if (source) {
        std::copy_n(source->fnumber, 2, output.fnumber);
        copyUnsignedRatio(output.fnumber_rational, source->fnumber_rational, source->fnumber);
    }
    source = select([](const auto& exif) { return focal(exif).has_value(); });
    output.has_focal_length = source != nullptr;
    if (source) {
        std::copy_n(source->focal_length, 2, output.focal_length);
        copyUnsignedRatio(output.focal_length_rational, source->focal_length_rational, source->focal_length);
    }
    source = select([](const auto& exif) { return focal35(exif).has_value(); });
    output.has_focal_length_35mm = source != nullptr;
    if (source) output.focal_length_35mm = source->focal_length_35mm;
    const auto zoom = either<std::array<std::uint32_t, 2>>(image, root, digitalZoom);
    output.has_digital_zoom = zoom.has_value();
    if (zoom) std::copy(zoom->begin(), zoom->end(), output.digital_zoom_rational);
    source = select([](const auto& exif) { return ratio(exif.aperture_value, exif.has_aperture_value, true).has_value(); });
    output.has_aperture_value = source != nullptr;
    if (source) std::copy_n(source->aperture_value, 2, output.aperture_value);
    source = select([](const auto& exif) {
        return unsignedRatio(exif.max_aperture_rational, exif.max_aperture_value,
                             exif.has_max_aperture_value, true).has_value();
    });
    output.has_max_aperture_value = source != nullptr;
    if (source) {
        std::copy_n(source->max_aperture_value, 2, output.max_aperture_value);
        copyUnsignedRatio(output.max_aperture_rational, source->max_aperture_rational, source->max_aperture_value);
    }
}

ExportIdentity exportIdentity(const SourceCaptureExif& source) {
    return {source.make, source.model};
}

}  // namespace rawrcam::renderer
