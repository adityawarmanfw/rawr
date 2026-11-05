#include "SourceCaptureExif.h"
#include "metadata/ExifTime.h"

#include <cmath>

namespace rawrcam::renderer {
namespace {
std::string text(const char* image, const char* root) {
    auto value = metadata::trimExifText(image);
    return value.empty() ? metadata::trimExifText(root) : value;
}
std::optional<double> ratio(const int32_t (&value)[2], bool present) {
    if (!present || value[0] <= 0 || value[1] <= 0) return std::nullopt;
    return double(value[0]) / double(value[1]);
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
    if (const auto f = ratio(e.fnumber, e.has_fnumber); f && *f <= 1000.0) return float(*f);
    if (!e.has_aperture_value || e.aperture_value[1] <= 0) return std::nullopt;
    const double f = std::pow(2.0, double(e.aperture_value[0]) / double(e.aperture_value[1]) / 2.0);
    if (!std::isfinite(f) || f <= 0.0 || f > 1000.0) return std::nullopt;
    return float(f);
}
std::optional<float> focal(const tinydng_exif& e) {
    const auto mm = ratio(e.focal_length, e.has_focal_length);
    if (!mm || *mm > 100000.0) return std::nullopt;
    return float(*mm);
}
std::optional<std::uint16_t> focal35(const tinydng_exif& e) {
    if (!e.has_focal_length_35mm || e.focal_length_35mm == 0) return std::nullopt;
    return e.focal_length_35mm;
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
}  // namespace

SourceCaptureExif readSourceCaptureExif(const tinydng_exif& image, const tinydng_exif* root,
                                        const char* uniqueCameraModel) {
    const auto rootText = [&](char* tinydng_exif::*field) { return root ? root->*field : nullptr; };
    SourceCaptureExif out;
    out.make = text(image.make, rootText(&tinydng_exif::make));
    out.model = text(image.model, rootText(&tinydng_exif::model));
    if (out.model.empty()) out.model = metadata::trimExifText(uniqueCameraModel);
    out.lensMake = text(image.lens_make, rootText(&tinydng_exif::lens_make));
    out.lensModel = text(image.lens_model, rootText(&tinydng_exif::lens_model));
    out.lensSpecification = either<std::array<double, 4>>(image, root, lensSpec);
    out.fNumber = either<float>(image, root, fNumber);
    out.focalLengthMm = either<float>(image, root, focal);
    out.focalLength35mm = either<std::uint16_t>(image, root, focal35);
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

ExportIdentity exportIdentity(const SourceCaptureExif& source, const std::string& deviceMake,
                              const std::string& deviceModel) {
    if (source.make.empty() && source.model.empty()) return {deviceMake, deviceModel};
    return {source.make, source.model};
}

}  // namespace rawrcam::renderer
