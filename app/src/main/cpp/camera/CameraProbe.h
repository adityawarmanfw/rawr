#pragma once

#include <string>
#include <vector>

namespace rawrcam::camera {

// Read-only camera inventory for the lens settings, as JSON:
// {"cameras":[{"id":"3","parentId":"","enumerated":false,"logical":false,
//   "physicalIds":["5"],"facing":0,"focalLengths":[6.2],"sensorSizeMm":[9.8,7.4],
//   "equivalentFocalMm":24.1,"rawStreams":[{"format":"RAW_SENSOR","width":4080,
//   "height":3064,"supported":true}],"black":[64,64,64,64],"white":1023,
//   "sessionKeys":[2147614720]}],"error":""}
// Covers enumerated ids, probed hidden numeric ids (see cameraDiscoveryCandidates), and the physical
// sub-cameras of logical multi-cameras (with parentId set). Does not open
// any camera.
[[nodiscard]] std::string probeCamerasJson();

// Opens the camera briefly to read its default request (TEMPLATE_PREVIEW),
// which reveals the type and default value of each request key it carries,
// and resolves the given key names to tag ids:
// {"tags":[{"tag":2147614720,"type":"int32","count":1,"values":[0]}],
//  "names":[{"name":"vivo.control.forceSensorMode","tag":2147614720}],
//  "sessionKeys":[...],"error":""}
// A name that can't be resolved has "tag":null. Fails with "error" set when
// the camera can't be opened (e.g. in use).
[[nodiscard]] std::string probeCameraKeysJson(const std::string& cameraId, const std::vector<std::string>& names);

}  // namespace rawrcam::camera
