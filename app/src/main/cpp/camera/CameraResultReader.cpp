#include "camera/CameraResultReader.h"

#include <camera/NdkCameraMetadataTags.h>

#include "camera/Camera2PriorityCompat.h"

namespace rawrcam::camera {
namespace {

std::optional<ACameraMetadata_const_entry> entry(const ACameraMetadata* metadata, uint32_t tag) {
    if (!metadata) return std::nullopt;
    ACameraMetadata_const_entry value{};
    if (ACameraMetadata_getConstEntry(metadata, tag, &value) != ACAMERA_OK) return std::nullopt;
    return value;
}

}  // namespace

CameraRequestObservation readCameraRequestObservation(const ACaptureRequest* requestCopy) {
    CameraRequestObservation observation;
    if (!requestCopy) return observation;
    ACameraMetadata_const_entry value{};
    if (ACaptureRequest_getConstEntry(requestCopy, ACAMERA_SENSOR_EXPOSURE_TIME, &value) == ACAMERA_OK &&
        value.data.i64 && value.count >= 1)
        observation.exposureTimeNs = value.data.i64[0];
    value = {};
    if (ACaptureRequest_getConstEntry(requestCopy, ACAMERA_SENSOR_SENSITIVITY, &value) == ACAMERA_OK &&
        value.data.i32 && value.count >= 1)
        observation.sensitivity = value.data.i32[0];
    value = {};
    if (ACaptureRequest_getConstEntry(requestCopy, ACAMERA_CONTROL_AF_MODE, &value) == ACAMERA_OK && value.data.u8 &&
        value.count >= 1)
        observation.afMode = value.data.u8[0];
    void* userContext = nullptr;
    if (ACaptureRequest_getUserContext(requestCopy, &userContext) == ACAMERA_OK && userContext) {
        observation.provenance = *static_cast<const CameraRequestProvenance*>(userContext);
    }
    return observation;
}

CameraControlResult readCameraControlResult(const ACameraMetadata* result,
                                            const CameraControlCapabilities& capabilities,
                                            const metadata::SensorGeometry& geometry) {
    CameraControlResult observation;
    if (const auto e = entry(result, ACAMERA_CONTROL_AF_MODE); e && e->data.u8 && e->count >= 1)
        observation.afMode = e->data.u8[0];
    if (const auto e = entry(result, ACAMERA_LENS_STATE); e && e->data.u8 && e->count >= 1)
        observation.lensState = e->data.u8[0];
    if (const auto e = entry(result, ACAMERA_SENSOR_FRAME_DURATION); e && e->data.i64 && e->count >= 1)
        observation.frameDurationNs = e->data.i64[0];
    if (const auto e = entry(result, ACAMERA_CONTROL_AE_TARGET_FPS_RANGE); e && e->data.i32 && e->count >= 2)
        observation.targetFpsRange = std::array<int32_t, 2>{e->data.i32[0], e->data.i32[1]};
    if (const auto e = entry(result, camera2_priority::kAePriorityModeTag); e && e->data.u8 && e->count >= 1)
        observation.aePriority = e->data.u8[0];
    if (const auto e = entry(result, ACAMERA_CONTROL_AE_EXPOSURE_COMPENSATION); e && e->data.i32 && e->count >= 1)
        observation.evSteps = e->data.i32[0];
    if (const auto e = entry(result, ACAMERA_CONTROL_AF_STATE); e && e->data.u8 && e->count >= 1)
        observation.afState = e->data.u8[0];
    if (const auto e = entry(result, ACAMERA_LENS_FOCUS_DISTANCE); e && e->data.f && e->count >= 1)
        observation.focusDistance = e->data.f[0];
    if (capabilities.faceDetectSupported) {
        const auto rects = entry(result, ACAMERA_STATISTICS_FACE_RECTANGLES);
        const auto scores = entry(result, ACAMERA_STATISTICS_FACE_SCORES);
        if (rects && rects->data.i32) {
            observation.faces = normalizeCameraFaces(geometry.activeArray, rects->data.i32, rects->count,
                                                     scores ? scores->data.u8 : nullptr, scores ? scores->count : 0);
        }
    }
    return observation;
}

}  // namespace rawrcam::camera
