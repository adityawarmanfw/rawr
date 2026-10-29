#pragma once
#include <tinydng.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace rawrcam::renderer {

// Capture EXIF a re-rendered DNG carries into its export. Each field reads
// the RAW image's IFD first and IFD0 second (the RAW may live in a SubIFD
// while ExifIFD hangs off IFD0). Empty/absent = the source has no value.
struct SourceCaptureExif {
    std::optional<tinydng_gps> gps;
    std::optional<std::array<uint8_t, 4>> gpsExifVersion;
    std::string make;
    std::string model;
    std::string lensMake;
    std::string lensModel;
    std::optional<std::array<double, 4>> lensSpecification;  // min/max focal, min/max f-number
    std::optional<float> fNumber;  // FNumber, else APEX ApertureValue
    std::optional<float> focalLengthMm;
    std::optional<std::uint16_t> focalLength35mm;
    std::optional<std::int64_t> exposureTimeNs;
    std::optional<std::int32_t> sensitivity;
    std::string dateTimeOriginal;  // "YYYY:MM:DD HH:MM:SS" as written
    std::string dateTimeDigitized;
    std::string offsetTimeOriginal;
    std::string subsecTimeOriginal;
};

// root may be null or alias image. uniqueCameraModel (DNG IFD0) stands in
// for Model only when neither IFD names one.
SourceCaptureExif readSourceCaptureExif(const tinydng_exif& image, const tinydng_exif* root,
                                        const char* uniqueCameraModel);

// Export camera identity: the source's make/model whenever it names a camera
// (a lone model keeps an empty make rather than borrowing the exporter's),
// the exporting device's only when the source names none.
struct ExportIdentity {
    std::string make;
    std::string model;
};
ExportIdentity exportIdentity(const SourceCaptureExif& source, const std::string& deviceMake,
                              const std::string& deviceModel);

}  // namespace rawrcam::renderer
