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
    std::string lensSerial;
    std::optional<std::array<double, 4>> lensSpecification;  // focal bounds, widest opening at each endpoint
    std::optional<double> apertureValue;
    std::optional<double> maxApertureValue;
    std::optional<float> fNumber;  // FNumber, else APEX ApertureValue
    std::optional<float> focalLengthMm;
    std::optional<std::uint16_t> focalLength35mm;
    std::optional<std::array<std::uint32_t, 2>> digitalZoomRatio;
    std::optional<std::int64_t> exposureTimeNs;
    std::optional<std::int32_t> sensitivity;
    std::string dateTimeOriginal;  // "YYYY:MM:DD HH:MM:SS" as written
    std::string dateTimeDigitized;
    std::string offsetTimeOriginal;
    std::string subsecTimeOriginal;
    std::string offsetTimeDigitized;
    std::string subsecTimeDigitized;
};

// root may be null or alias image. uniqueCameraModel (DNG IFD0) stands in
// for Model only when neither IFD names one.
SourceCaptureExif readSourceCaptureExif(const tinydng_exif& image, const tinydng_exif* root,
                                        const char* uniqueCameraModel, const tinydng_raw_info* rootRaw = nullptr);

void preserveSourceLensExif(tinydng_exif& output, const tinydng_exif& image,
                            const tinydng_exif* root, const tinydng_raw_info* rootRaw);

// Export camera identity stays with the source, including unknown values.
struct ExportIdentity {
    std::string make;
    std::string model;
};
ExportIdentity exportIdentity(const SourceCaptureExif& source);

}  // namespace rawrcam::renderer
