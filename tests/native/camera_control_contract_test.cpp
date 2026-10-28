#include "camera/CameraControlSerialization.h"
#include <cassert>
#include <string>

int main() {
    rawrcam::camera::CameraControlState s{};
    s.capabilities.generation = 7;
    s.capabilities.cameraId = "5";
    s.capabilities.lensId = "tele";
    s.capabilities.sensitivityMin = 50;
    s.capabilities.sensitivityMax = 6400;
    s.capabilities.exposureTimeMinNs = 100000;
    s.capabilities.exposureTimeMaxNs = 30000000000LL;
    s.capabilities.evMinSteps = -6;
    s.capabilities.evMaxSteps = 6;
    s.capabilities.evStepNumerator = 1;
    s.capabilities.evStepDenominator = 3;
    s.capabilities.tapAfSupported = true;
    s.capabilities.manualFocusSupported = true;
    s.capabilities.minimumFocusDistance = 10.0f;
    s.requestedSensitivity = 200;
    s.requestedExposureTimeNs = 10000000;
    s.appliedSensitivity = 196;
    s.appliedExposureTimeNs = 9999992;
    s.appliedPostRawSensitivityBoost = 150;
    s.appliedRawFps = 30.0;
    s.measuredViewfinderFps = 29.85;
    const std::string json = rawrcam::camera::serializeCameraControlState(s);
    assert(json.find("\"generation\":7") != std::string::npos);
    assert(json.find("\"cameraId\":\"5\"") != std::string::npos);
    assert(json.find("\"appliedSensitivity\":196") != std::string::npos);
    assert(json.find("\"appliedPostRawSensitivityBoost\":150") != std::string::npos);
    assert(json.find("\"appliedRawFps\":30") != std::string::npos);
    assert(json.find("\"measuredViewfinderFps\":29.85") != std::string::npos);
    assert(json.find("\"tapAfSupported\":true") != std::string::npos);
    assert(json.find("rawAutoExposure") == std::string::npos);
    // A refused lens profile is reported, quotes and all, and is empty otherwise.
    assert(json.find("\"profileError\":\"\"") != std::string::npos);
    s.capabilities.profileError = "invalid \"opMode\" for lens X";
    const std::string refused = rawrcam::camera::serializeCameraControlState(s);
    assert(refused.find("\"profileError\":\"invalid \\\"opMode\\\" for lens X\"") != std::string::npos);
    return 0;
}
