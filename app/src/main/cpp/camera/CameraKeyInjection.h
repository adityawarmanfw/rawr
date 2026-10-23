#pragma once

#include <camera/NdkCameraMetadata.h>
#include <camera/NdkCaptureRequest.h>

#include <functional>
#include <string>
#include <vector>

#include "camera/CameraRouting.h"

namespace rawrcam::camera {

struct CameraKeyInjectionResult {
    size_t applied = 0;
    // "<tag>:<reason>" for every enabled key of the scope that wasn't set.
    std::vector<std::string> failures;
};

// Applies the enabled profile keys of one scope to a request (the session
// parameters for Session, the capture request for Request). Session keys must
// be advertised in REQUEST_AVAILABLE_SESSION_KEYS. Every key is logged; the
// caller decides whether a failure rejects the session.
CameraKeyInjectionResult applyCameraKeySettings(ACaptureRequest* request, const ACameraMetadata* characteristics,
                                                const std::vector<CameraKeySetting>& keys, const char* cameraId,
                                                CameraKeySetting::Scope scope, const char* origin,
                                                const std::function<void(const std::string&)>& diag);

// Logs every entry of a request, one line per tag ("CAMERA_REQUEST_DUMP
// kind=<kind> tag=0x... type=... count=... values=..."), for comparing what a
// request actually carries (diagnostics only).
void dumpCaptureRequest(const ACaptureRequest* request, const char* kind,
                        const std::function<void(const std::string&)>& diag);

}  // namespace rawrcam::camera
