#pragma once

#include <camera/NdkCaptureRequest.h>

#include <array>
#include <optional>

#include "camera/CameraControlTypes.h"
#include "camera/CameraFocusControls.h"

namespace rawrcam::camera {

struct CameraControlApplyResult {
    camera_status_t focusStatus = ACAMERA_OK;
    // The request asks the HAL for S/I priority (not rawr AE, not Auto fallback).
    bool halPriorityMode = false;
    bool aePriorityRequested = false;
    camera_status_t aePriorityStatus = ACAMERA_OK;
    bool aeTargetFpsRequested = false;
    camera_status_t aeTargetFpsStatus = ACAMERA_OK;
    // White-balance outcome: whether a ManualTempTint request fell back to
    // Auto (unsupported or no HAL transform seed yet).
    bool wbManualRequested = false;
    bool wbManualFallbackToAuto = false;
    camera_status_t wbStatus = ACAMERA_OK;
};

struct CameraSessionCadenceApplyResult {
    std::optional<std::array<int32_t, 2>> fpsRange;
    camera_status_t status = ACAMERA_OK;
};

// Serializes RawrCam control state into an existing Camera2 request.
// Focus/metering intent comes from CameraFocusControls/CameraSpotMeteringControls.
// Session submission, diagnostics and resource lifetime remain in the controller.
void applyPreviewRequestDefaults(ACaptureRequest* request, const metadata::CameraContextMetadata& context);
CameraSessionCadenceApplyResult applyCameraSessionCadence(ACaptureRequest* request, const CameraControlState& state);
CameraControlApplyResult applyCameraControlState(ACaptureRequest* request, const CameraControlState& state,
                                                 const CameraMeteringRequest& metering);

}  // namespace rawrcam::camera
