#pragma once

#include <array>
#include <optional>

#include "camera/CameraControlTypes.h"

namespace rawrcam::camera {

struct SensorPoint {
    float x = 0.5f;
    float y = 0.5f;
};
using CameraMeteringRegion = std::array<int32_t, 5>;  // left, top, right, bottom, weight

// Distortion correction is OFF: requests use the pre-correction array when
// advertised, otherwise the ordinary active array. Geometry stays native.
std::optional<CameraMeteringRegion> tapFocusRegion(const metadata::SensorGeometry& geometry, SensorPoint point);
std::optional<CameraMeteringRegion> faceFocusRegion(const metadata::SensorGeometry& geometry,
                                                    const FaceDetection& face);
// A region back in the normalized coordinates its point/face came in
// (x0, y0, x1, y1 in [0,1]).
std::optional<std::array<float, 4>> normalizedRegion(const metadata::SensorGeometry& geometry,
                                                     const CameraMeteringRegion& region);

class CameraSpotMeteringControls {
   public:
    bool requestTarget(CameraControlState& state, bool active, SensorPoint point);
    SensorPoint point() const { return point_; }
    std::optional<CameraMeteringRegion> region(const CameraControlState& state,
                                               const metadata::SensorGeometry* geometry) const;

   private:
    SensorPoint point_;
};

}  // namespace rawrcam::camera
