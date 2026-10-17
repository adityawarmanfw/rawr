#include "camera/CameraMeteringRegions.h"

#include <algorithm>
#include <cmath>

namespace rawrcam::camera {
namespace {

const metadata::RectI* meteringArray(const metadata::SensorGeometry& geometry) {
    const auto& base =
        geometry.preCorrectionActiveArray.valid ? geometry.preCorrectionActiveArray : geometry.activeArray;
    if (!base.valid || base.right <= base.left || base.bottom <= base.top) return nullptr;
    return &base;
}

std::optional<CameraMeteringRegion> pointRegion(const metadata::SensorGeometry& geometry, SensorPoint point,
                                                int divisor) {
    const auto* base = meteringArray(geometry);
    if (!base || !std::isfinite(point.x) || !std::isfinite(point.y)) return std::nullopt;
    const int32_t width = base->right - base->left;
    const int32_t height = base->bottom - base->top;
    const int32_t cx = base->left + static_cast<int32_t>(std::lround(std::clamp(point.x, 0.0f, 1.0f) * (width - 1)));
    const int32_t cy = base->top + static_cast<int32_t>(std::lround(std::clamp(point.y, 0.0f, 1.0f) * (height - 1)));
    const int32_t rw = std::max(32, width / divisor);
    const int32_t rh = std::max(32, height / divisor);
    return CameraMeteringRegion{std::clamp(cx - rw / 2, base->left, base->right - 1),
                                std::clamp(cy - rh / 2, base->top, base->bottom - 1),
                                std::clamp(cx + rw / 2, base->left + 1, base->right),
                                std::clamp(cy + rh / 2, base->top + 1, base->bottom), 1000};
}

}  // namespace

std::optional<CameraMeteringRegion> tapFocusRegion(const metadata::SensorGeometry& geometry, SensorPoint point) {
    return pointRegion(geometry, point, 10);
}

std::optional<CameraMeteringRegion> faceFocusRegion(const metadata::SensorGeometry& geometry,
                                                    const FaceDetection& face) {
    const auto* base = meteringArray(geometry);
    if (!base || !std::isfinite(face.x) || !std::isfinite(face.y) || !std::isfinite(face.w) || !std::isfinite(face.h) ||
        face.x < 0 || face.x > 1 || face.y < 0 || face.y > 1 || face.w <= 0 || face.w > 1 || face.h <= 0 || face.h > 1)
        return std::nullopt;
    const float width = static_cast<float>(base->right - base->left);
    const float height = static_cast<float>(base->bottom - base->top);
    const float cx = base->left + (face.x + face.w * 0.5f) * width;
    const float cy = base->top + (face.y + face.h * 0.5f) * height;
    const float rw = std::max(32.0f, face.w * width * 1.25f);
    const float rh = std::max(32.0f, face.h * height * 1.25f);
    return CameraMeteringRegion{
        std::clamp(static_cast<int32_t>(std::lround(cx - rw * 0.5f)), base->left, base->right - 1),
        std::clamp(static_cast<int32_t>(std::lround(cy - rh * 0.5f)), base->top, base->bottom - 1),
        std::clamp(static_cast<int32_t>(std::lround(cx + rw * 0.5f)), base->left + 1, base->right),
        std::clamp(static_cast<int32_t>(std::lround(cy + rh * 0.5f)), base->top + 1, base->bottom), 1000};
}

bool CameraSpotMeteringControls::requestTarget(CameraControlState& state, bool active, SensorPoint point) {
    if (active &&
        (state.exposureMode != ExposureControlMode::Auto || !std::isfinite(point.x) || !std::isfinite(point.y)))
        return false;
    state.spotAeActive = active;
    if (active) point_ = {std::clamp(point.x, 0.0f, 1.0f), std::clamp(point.y, 0.0f, 1.0f)};
    return true;
}

std::optional<CameraMeteringRegion> CameraSpotMeteringControls::region(const CameraControlState& state,
                                                                       const metadata::SensorGeometry* geometry) const {
    // rawr AE meters the spot itself (AE is off on its requests).
    if (!geometry || !state.spotAeActive || state.exposureMode != ExposureControlMode::Auto || state.rawrAeActive ||
        state.capabilities.maxAeRegions <= 0)
        return std::nullopt;
    // Same 1/8 footprint as the RAW spot reference.
    return pointRegion(*geometry, point_, 8);
}

}  // namespace rawrcam::camera
