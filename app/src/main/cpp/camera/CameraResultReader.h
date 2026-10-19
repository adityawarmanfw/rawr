#pragma once

#include <camera/NdkCameraMetadata.h>
#include <camera/NdkCaptureRequest.h>

#include "camera/CameraControlResult.h"
#include "camera/CameraRequestProvenance.h"

namespace rawrcam::camera {

struct CameraRequestObservation {
    std::optional<int64_t> exposureTimeNs;
    std::optional<int32_t> sensitivity;
    std::optional<uint8_t> afMode;
    std::optional<CameraRequestProvenance> provenance;
};

// Thin NDK decoding. The caller first validates callback/session generation and
// retains the controller lock while copying the request's provenance context.
CameraRequestObservation readCameraRequestObservation(const ACaptureRequest* requestCopy);
CameraControlResult readCameraControlResult(const ACameraMetadata* result,
                                            const CameraControlCapabilities& capabilities,
                                            const metadata::SensorGeometry& geometry);

}  // namespace rawrcam::camera
