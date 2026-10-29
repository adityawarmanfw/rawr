// Renderer export EXIF: a re-rendered DNG keeps the source camera, lens and
// capture time. Each field reads the RAW image IFD first and IFD0 second; the
// exporting device's identity only fills in for a source that names none.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "renderer/SourceCaptureExif.h"
#include "metadata/GpsMetadata.h"

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}
char* s(const char* text) { return const_cast<char*>(text); }
tinydng_exif blank() {
    tinydng_exif e;
    std::memset(&e, 0, sizeof(e));
    return e;
}
}  // namespace

int main() {
    using rawrcam::renderer::exportIdentity;
    using rawrcam::renderer::readSourceCaptureExif;
    {
        tinydng_image_info image{}, root{};
        root.has_gps_directory = 1;
        root.gps = *rawrcam::metadata::makeGps(0, 0, 0.0, {}, 1793059200123LL);
        root.exif.has_exif_version = 1;
        std::memcpy(root.exif.exif_version, "0300", 4);
        const auto original = rawrcam::metadata::readSourceGps(image, &root);
        check(original.gps && original.gps->has_altitude && original.gps->altitude[0] == 0, "root GPS zero altitude");
        check(original.exifVersion && (*original.exifVersion)[1] == '3', "source GPS version context");
          image = root;
          std::memcpy(root.exif.exif_version, "0232", 4);
          check(!rawrcam::metadata::readSourceGps(image, &root).gps, "conflicting altitude version contexts omitted");
          image = {};
          const auto legacyGps = rawrcam::metadata::readSourceGps(image, &root);
          check(legacyGps.gps && legacyGps.gps->has_altitude && legacyGps.exifVersion &&
              (*legacyGps.exifVersion)[1] == '2', "supported legacy altitude keeps legacy version context");
          root.exif = {};
          root.gps.altitude_ref = 2;
          const auto mslGps = rawrcam::metadata::readSourceGps(image, &root);
          check(mslGps.gps && mslGps.gps->has_altitude && mslGps.exifVersion &&
              (*mslGps.exifVersion)[1] == '3', "versionless MSL uses a current output profile");
          root.gps.altitude_ref = 0;
          root.exif.has_exif_version = 1;
          std::memcpy(root.exif.exif_version, "0310", 4);
          const auto unknownVersion = rawrcam::metadata::readSourceGps(image, &root);
          check(unknownVersion.gps && !unknownVersion.gps->has_altitude && unknownVersion.exifVersion &&
              (*unknownVersion.exifVersion)[2] == '0', "unsupported source version does not establish altitude datum");
          std::memcpy(root.exif.exif_version, "0300", 4);
        image = root;
        image.gps.altitude[0] = 100;
        check(!rawrcam::metadata::readSourceGps(image, &root).gps, "conflicting GPS records omitted");
        image.gps.latitude[0][1] = 0;
        check(!rawrcam::metadata::readSourceGps(image, &root).gps, "invalid image GPS not replaced with root fields");
        image = {};
        root.gps.altitude_ref = 9;
        const auto malformed = rawrcam::metadata::readSourceGps(image, &root);
        check(malformed.gps && !malformed.gps->has_altitude, "malformed optional altitude omitted");
    }

    // RAW in a SubIFD, EXIF on IFD0: every field falls back to the root.
    {
        auto image = blank();
        auto root = blank();
        root.make = s("Xiaomi");
        root.model = s("23078PND5G");
        root.lens_make = s("Xiaomi");
        root.lens_model = s("Main 23mm");
        root.lens_spec[0] = 23;
        root.lens_spec[1] = 23;
        root.lens_spec[2] = 1.9;
        root.lens_spec[3] = 1.9;
        root.has_lens_spec = 1;
        root.fnumber[0] = 19;
        root.fnumber[1] = 10;
        root.has_fnumber = 1;
        root.focal_length[0] = 6900;
        root.focal_length[1] = 1000;
        root.has_focal_length = 1;
        root.focal_length_35mm = 23;
        root.has_focal_length_35mm = 1;
        root.exposure_time[0] = 1;
        root.exposure_time[1] = 100;
        root.has_exposure_time = 1;
        root.iso = 400;
        root.has_iso = 1;
        root.datetime_original = s("2026:05:01 12:34:56");
        root.datetime_digitized = s("2026:05:01 12:34:57");
        root.offset_time_original = s("+08:00");
        root.subsec_time_original = s("042");
        const auto x = readSourceCaptureExif(image, &root, nullptr);
        check(x.make == "Xiaomi" && x.model == "23078PND5G", "root make/model");
        check(x.lensMake == "Xiaomi" && x.lensModel == "Main 23mm", "root lens");
        check(x.lensSpecification && (*x.lensSpecification)[2] == 1.9, "root lens spec");
        check(x.fNumber && std::abs(*x.fNumber - 1.9f) < 1e-5f, "root fnumber");
        check(x.focalLengthMm && std::abs(*x.focalLengthMm - 6.9f) < 1e-5f, "root focal");
        check(x.focalLength35mm == 23, "root focal 35mm");
        check(x.exposureTimeNs == 10'000'000, "root exposure");
        check(x.sensitivity == 400, "root iso");
        check(x.dateTimeOriginal == "2026:05:01 12:34:56", "root datetime original");
        check(x.dateTimeDigitized == "2026:05:01 12:34:57", "root datetime digitized");
        check(x.offsetTimeOriginal == "+08:00", "root offset");
        check(x.subsecTimeOriginal == "042", "root subsec");
        const auto id = exportIdentity(x, "vivo", "X300 Ultra");
        check(id.make == "Xiaomi" && id.model == "23078PND5G", "source identity beats the exporter");
    }

    // Per field: the image IFD wins where it has a value, IFD0 fills the rest.
    {
        auto image = blank();
        auto root = blank();
        image.model = s("Image Model ");
        root.make = s("Root Make");
        root.model = s("Root Model");
        image.iso = 100;
        image.has_iso = 1;
        root.iso = 800;
        root.has_iso = 1;
        root.exposure_time[0] = 1;
        root.exposure_time[1] = 50;
        root.has_exposure_time = 1;
        const auto x = readSourceCaptureExif(image, &root, "Unique Model");
        check(x.make == "Root Make" && x.model == "Image Model", "mixed make/model, trimmed");
        check(x.sensitivity == 100 && x.exposureTimeNs == 20'000'000, "mixed numeric fallback");
    }

    // FNumber beats APEX ApertureValue; APEX alone still converts.
    {
        auto image = blank();
        image.fnumber[0] = 28;
        image.fnumber[1] = 10;
        image.has_fnumber = 1;
        image.aperture_value[0] = 2;  // APEX 2 = f/2
        image.aperture_value[1] = 1;
        image.has_aperture_value = 1;
        auto x = readSourceCaptureExif(image, nullptr, nullptr);
        check(x.fNumber && std::abs(*x.fNumber - 2.8f) < 1e-5f, "fnumber preferred");
        image.has_fnumber = 0;
        x = readSourceCaptureExif(image, nullptr, nullptr);
        check(x.fNumber && std::abs(*x.fNumber - 2.0f) < 1e-4f, "apex fallback");
    }

    // A tagless source: nothing invented, the exporter's identity fills in
    // and the writer falls back to the export time (empty dateTimeOriginal).
    {
        auto image = blank();
        image.datetime_original = s("    :  :     :  :  ");
        image.offset_time_original = s("+07:00");
        const auto x = readSourceCaptureExif(image, &image, nullptr);
        check(x.make.empty() && x.model.empty() && x.lensModel.empty(), "no identity");
        check(!x.fNumber && !x.focalLengthMm && !x.exposureTimeNs && !x.sensitivity && !x.lensSpecification,
              "no numeric tags");
        check(x.dateTimeOriginal.empty() && x.offsetTimeOriginal.empty(), "blank date is absent, offset dropped");
        const auto id = exportIdentity(x, "vivo", "X300 Ultra");
        check(id.make == "vivo" && id.model == "X300 Ultra", "device identity fallback");
    }

    // UniqueCameraModel names the camera when Make/Model are missing; the
    // exporter's make is not paired with someone else's model.
    {
        auto image = blank();
        image.datetime_original = s("0000:00:00 00:00:00");
        const auto x = readSourceCaptureExif(image, nullptr, "Some Camera");
        check(x.model == "Some Camera" && x.make.empty(), "unique camera model");
        check(x.dateTimeOriginal.empty(), "zeroed date is absent");
        const auto id = exportIdentity(x, "vivo", "X300 Ultra");
        check(id.make.empty() && id.model == "Some Camera", "no mixed identity");
    }

    // Malformed time qualifiers are dropped; the date itself is kept.
    {
        auto image = blank();
        image.datetime_original = s("2025:12:31 23:59:59");
        image.offset_time_original = s("GMT+1");
        image.subsec_time_original = s("12a");
        const auto x = readSourceCaptureExif(image, nullptr, nullptr);
        check(x.dateTimeOriginal == "2025:12:31 23:59:59" && x.dateTimeDigitized.empty(), "date kept");
        check(x.offsetTimeOriginal.empty() && x.subsecTimeOriginal.empty(), "bad qualifiers dropped");
    }

    if (failures == 0) std::printf("renderer_source_exif_test: ok\n");
    return failures == 0 ? 0 : 1;
}
