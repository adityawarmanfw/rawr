#pragma once

#include <camera/NdkCameraManager.h>

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "geometry/RawGeometry.h"

namespace rawrcam::camera {

using CameraDiagnostic = std::function<void(const std::string&)>;

// Diagnostic discovery and static RAW-stream capability inspection only.
// Does not own a camera device/session and does not choose an application lens route.
void emitCameraDiscovery(ACameraManager* manager, const CameraDiagnostic& diagnostic);
// Ids from ACameraManager_getCameraIdList (the publicly enumerated cameras).
std::set<std::string> enumeratedCameraIds(ACameraManager* manager);
// Public IDs plus the existing hidden-provider probe range; routing stays native.
std::vector<std::string> cameraDiscoveryCandidates(ACameraManager* manager);
// RAW16 (RAW_SENSOR) and RAW10 output streams from the default stream configuration table.
std::vector<geometry::RawStreamOption> rawOutputStreams(const ACameraMetadata* characteristics);
// Smallest YUV_420_888 output of at least minWidth x minHeight; {0, 0} when there is none.
std::pair<int32_t, int32_t> smallestYuvOutput(const ACameraMetadata* characteristics, int32_t minWidth,
                                              int32_t minHeight);

}  // namespace rawrcam::camera
