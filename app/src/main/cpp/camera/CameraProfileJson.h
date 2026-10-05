#pragma once

#include <optional>
#include <string>

#include "camera/CameraRouting.h"

namespace rawrcam::camera {

// JSON form of a camera profile, shared with the app's lens settings:
// {"id":"user","lenses":[{"id":"35","cameraId":"3","physicalCameraId":"",
//   "stream":{"format":"RAW16","width":4080,"height":3064},
//   "levels":{"static":true,"black":[64,64,64,64],"white":1023},
//   "keys":[{"tag":"vivo.control.forceSensorMode","type":"int32","scope":"session","values":[31],"enabled":true}]}]}
// Stream width/height 0 means the largest size. Unknown fields are ignored.
[[nodiscard]] std::string serializeCameraProfile(const CameraProfile& profile);
// Returns nullopt (with a reason) for malformed JSON or a profile without lenses.
[[nodiscard]] std::optional<CameraProfile> parseCameraProfile(const std::string& json, std::string* error = nullptr);

// A built-in device profile (camera/profiles/*.json): the profile JSON above
// plus "name", "version" and a "match" block:
// "match":{"models":["V2562"],"verifiedModels":["V2562"],
//          "propertyPrefixes":{"ro.vivo.product.model":"PD2547"}}
// Rejects a reserved id ("user", "generic") or a profile that matches nothing.
[[nodiscard]] std::optional<BuiltInProfile> parseBuiltInProfile(const std::string& json, std::string* error = nullptr);

void appendJsonString(std::string& out, const std::string& value);

}  // namespace rawrcam::camera
