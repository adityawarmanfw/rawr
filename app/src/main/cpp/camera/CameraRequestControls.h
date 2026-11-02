#pragma once

#include "camera/CameraRequest.h"

#include <array>
#include <optional>

#include "camera/CameraControlTypes.h"
#include "camera/CameraFocusControls.h"

namespace rawrcam::camera {

struct CameraControlApplyResult {
    RequestStatus focusStatus = kRequestOk;
    // The request asks the HAL for S/I priority (not rawr AE, not Auto fallback).
    bool halPriorityMode = false;
    bool aePriorityRequested = false;
    RequestStatus aePriorityStatus = kRequestOk;
    bool aeTargetFpsRequested = false;
    RequestStatus aeTargetFpsStatus = kRequestOk;
    // White-balance outcome: whether a ManualTempTint request fell back to
    // Auto (unsupported or no HAL transform seed yet).
    bool wbManualRequested = false;
    bool wbManualFallbackToAuto = false;
    RequestStatus wbStatus = kRequestOk;
};

struct CameraSessionCadenceApplyResult {
    std::optional<std::array<int32_t, 2>> fpsRange;
    RequestStatus status = kRequestOk;
};

// Serializes RawrCam control state into an existing Camera2 request.
// Focus/metering intent comes from CameraFocusControls/CameraSpotMeteringControls.
// Session submission, diagnostics and resource lifetime remain in the controller.
// minimal: leave distortion correction, hot-pixel mode and the hot-pixel map to the template. The lens shading
// map stays on when the camera offers one: the app's shading correction reads it.
void applyPreviewRequestDefaults(CameraRequest* request, const metadata::CameraContextMetadata& context,
                                 bool minimal = false);
CameraSessionCadenceApplyResult applyCameraSessionCadence(CameraRequest* request, const CameraControlState& state);
CameraControlApplyResult applyCameraControlState(CameraRequest* request, const CameraControlState& state,
                                                 const CameraMeteringRequest& metering);

}  // namespace rawrcam::camera
