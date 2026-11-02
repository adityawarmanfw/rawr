#pragma once

#include <string>

namespace rawrcam::camera {

struct CameraKeySubmission {
    std::string failure;
    bool warnUnlistedSession = false;
};

// Advertisement affects diagnostics, not whether a resolved key is submitted.
// The framework may still filter session parameters before they reach the HAL.
template <class Setter>
CameraKeySubmission submitCameraKey(bool resolved, bool hasValues, bool unlistedSession,
                                   const std::string& lookupError, Setter&& setter) {
    if (!resolved) return {"lookup:" + lookupError, false};
    if (!hasValues) return {"no_values", false};
    const int status = setter();
    if (status != 0) return {"set_status=" + std::to_string(status), false};
    return {{}, unlistedSession};
}

}  // namespace rawrcam::camera
