#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "metadata/CameraContextMetadata.h"

namespace rawrcam::color {

// DNG camera profile tables: hue shift (degrees), saturation scale, value
// scale per (value, hue, saturation) division, saturation varying fastest
// (DNG SDK dng_hue_sat_map layout). Value divisions of 1 make it 2D.
struct DcpHueSatTable {
    uint32_t hues = 0;
    uint32_t sats = 0;
    uint32_t vals = 0;
    std::vector<float> data;  // hues * sats * vals * 3
    // ProfileHueSatMapEncoding / ProfileLookTableEncoding: 0 linear, 1 sRGB
    // gamma for the value dimension lookup.
    uint32_t encoding = 0;

    [[nodiscard]] bool empty() const noexcept { return data.empty(); }
};

// A DNG camera profile (.dcp) for one lens: replaces the camera's colour
// calibration in the DNG and the app's render. Matrices are row-major,
// XYZ -> camera (ColorMatrix) and white-balanced camera -> D50 XYZ
// (ForwardMatrix), like the DNG tags. Three-colour profiles only.
struct DcpProfile {
    std::string name;
    std::string copyright;
    std::string calibrationSignature;
    std::string uniqueCameraModel;
    // EXIF LightSource codes. illuminant2 is 0 for a single-illuminant profile.
    uint16_t illuminant1 = 0;
    uint16_t illuminant2 = 0;
    std::array<double, 9> colorMatrix1{};
    std::optional<std::array<double, 9>> colorMatrix2;
    std::optional<std::array<double, 9>> forwardMatrix1;
    std::optional<std::array<double, 9>> forwardMatrix2;
    // HueSatMap1/2 share dims and encoding; map2 is empty for a single map.
    DcpHueSatTable hueSatMap1;
    DcpHueSatTable hueSatMap2;
    DcpHueSatTable lookTable;
    std::vector<float> toneCurve;  // (x, y) pairs in [0, 1]
    std::optional<uint32_t> embedPolicy;
    std::optional<double> baselineExposureOffset;
    std::optional<uint32_t> defaultBlackRender;
    // The file as read: capture jobs persist it and re-parse on load.
    std::vector<uint8_t> bytes;

    [[nodiscard]] bool dualIlluminant() const noexcept { return colorMatrix2.has_value(); }
    // ProfileEmbedPolicy 2 ("embed never"): the DNG can't carry it, so the
    // app doesn't use it either (the render and the DNG must agree).
    [[nodiscard]] bool embeddable() const noexcept { return embedPolicy.value_or(0) != 2; }
};

using DcpProfilePtr = std::shared_ptr<const DcpProfile>;

[[nodiscard]] std::optional<DcpProfile> parseDcp(std::vector<uint8_t> bytes, std::string* error = nullptr);
[[nodiscard]] std::optional<DcpProfile> loadDcp(const std::string& path, std::string* error = nullptr);
// Reads only profile tags from IFD0, without loading a DNG's pixel payload.
[[nodiscard]] std::optional<DcpProfile> loadEmbeddedDcp(const std::string& path, std::string* error = nullptr);

// DNG-spec ForwardMatrix equivalent of a ColorMatrix (XYZ -> camera) at its
// calibration illuminant: camera neutral (1,1,1) lands on D50 via Bradford.
[[nodiscard]] std::optional<std::array<double, 9>> forwardFromColorMatrix(const std::array<double, 9>& colorMatrix,
                                                                          uint16_t illuminant);

// Some HALs (OPPO/OnePlus Snapdragon builds) publish a ForwardMatrix that is
// just the sRGB primaries in D50 XYZ, i.e. "this camera is already sRGB",
// next to a real ColorMatrix. Trusting it applies no camera-to-sRGB matrix and
// leaves the image desaturated. Replaces each such ForwardMatrix with the one
// derived from its ColorMatrix; returns how many were replaced.
int repairPlaceholderForwardMatrices(metadata::StaticColorCalibration& color);

// True for a matrix that is just the sRGB primaries in D50 XYZ.
[[nodiscard]] bool isSrgbPlaceholderMatrix(const metadata::Matrix3x3& m);

// True when either ForwardMatrix is still the sRGB placeholder, i.e. the HAL
// publishes no usable camera colour for this camera (the repair above could not
// derive one either).
[[nodiscard]] bool hasPlaceholderForwardMatrix(const metadata::StaticColorCalibration& color);

// The camera calibration the render solves with when the profile is active:
// the profile's matrices with identity CameraCalibration (a DCP describes the
// camera it was measured on), a derived ForwardMatrix where the profile has
// none, and a single-illuminant profile's set repeated in both slots.
[[nodiscard]] std::optional<metadata::StaticColorCalibration> dcpColorCalibration(const DcpProfile& profile);

// One line for logs: name, illuminants and which tables are present.
[[nodiscard]] std::string describe(const DcpProfile& profile);

}  // namespace rawrcam::color
