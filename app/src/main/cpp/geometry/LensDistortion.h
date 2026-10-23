#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace rawrcam::geometry {

// Lens distortion calibration in the OpenCV rational model, for a lens whose
// camera reports none (Camera2 LENS_DISTORTION all zero) but whose vendor
// publishes one. Pixel coordinates are pixel indices on a width x height frame
// (pixel centres at integers, as OpenCV and Camera2 use).
//
// The model maps a corrected output pixel to its source in the distorted
// frame: normalize by the output focal (fx, fy scaled by outputScale) around
// (cx, cy), apply
//   radial = (1 + k1 r^2 + k2 r^4 + k3 r^6) / (1 + k4 r^2 + k5 r^4 + k6 r^6)
//   x' = x radial + 2 p1 x y + p2 (r^2 + 2 x^2)
//   y' = y radial + p1 (r^2 + 2 y^2) + 2 p2 x y
// and denormalize by (fx, fy). outputScale < 1 widens the output to fill the
// frame (no empty corners) instead of cropping into it.
struct LensDistortionModel {
    uint32_t width = 0;
    uint32_t height = 0;
    double fx = 0.0, fy = 0.0, cx = 0.0, cy = 0.0;
    // [k1, k2, p1, p2, k3, k4, k5, k6] (OpenCV order).
    std::array<double, 8> k{};
    double outputScale = 1.0;
};

[[nodiscard]] bool isValid(const LensDistortionModel& model) noexcept;

// The model on a width x height frame of the same aspect (a binned or
// full-size readout, or a super-resolution output); nullopt when the aspect
// differs by more than half a pixel.
[[nodiscard]] std::optional<LensDistortionModel> scaledTo(const LensDistortionModel& model, uint32_t width,
                                                          uint32_t height) noexcept;

// Source pixel (distorted frame) for corrected output pixel (x, y).
[[nodiscard]] std::array<double, 2> sourcePixel(const LensDistortionModel& model, double x, double y) noexcept;

// The same mapping in normalized source UV (0..1 over the frame, pixel
// centres at (i + 0.5) / size), resolution independent: corrected -> distorted.
[[nodiscard]] std::array<double, 2> sourceUv(const LensDistortionModel& model, double u, double v) noexcept;
// Inverse (distorted -> corrected UV), by fixed-point iteration.
[[nodiscard]] std::array<double, 2> correctedUv(const LensDistortionModel& model, double u, double v) noexcept;

// Output scale for undistorting only a centred crop of the frame (video):
// the smallest one >= model.outputScale whose crop output samples sources
// inside the crop (pixel indices x..x+width-1, y..y+height-1), since only the
// crop is demosaiced. Equals model.outputScale when that already fits.
[[nodiscard]] double containedOutputScale(const LensDistortionModel& model, uint32_t x, uint32_t y, uint32_t width,
                                          uint32_t height);

// DNG WarpRectilinear (opcode 1) parameters for one plane approximating the
// model on a width x height active area: kr0..kr3 radial, kt0/kt1 tangential,
// centre as a fraction of the area, in the DNG spec's continuous coordinates
// (pixel centres at +0.5) normalized by the centre's farthest corner.
struct WarpRectilinearFit {
    std::array<double, 4> kr{};
    std::array<double, 2> kt{};
    std::array<double, 2> center{};
    double meanErrorPx = 0.0;
    double maxErrorPx = 0.0;
};
[[nodiscard]] std::optional<WarpRectilinearFit> fitWarpRectilinear(const LensDistortionModel& model, uint32_t width,
                                                                   uint32_t height);

// Lens profile mirror of the corrected image, in the sensor frame (before
// display rotation): renders flip it after undistort, the DNG keeps the
// sensor pixels and composes it into Orientation.
inline constexpr uint32_t kLensMirrorHorizontal = 1u;
inline constexpr uint32_t kLensMirrorVertical = 2u;
inline constexpr uint32_t kLensMirrorMask = kLensMirrorHorizontal | kLensMirrorVertical;

// Normalized corrected-image UV (0..1) through the mirror; its own inverse.
[[nodiscard]] constexpr std::array<double, 2> mirroredUv(uint32_t mirror, double u, double v) noexcept {
    return {(mirror & kLensMirrorHorizontal) ? 1.0 - u : u, (mirror & kLensMirrorVertical) ? 1.0 - v : v};
}

// EXIF/TIFF Orientation (1..8) of an image stored unmirrored that displays
// like the mirrored image stored with [orientation]. Out-of-range values act as 1.
[[nodiscard]] uint16_t mirroredOrientation(uint16_t orientation, uint32_t mirror) noexcept;

}  // namespace rawrcam::geometry
