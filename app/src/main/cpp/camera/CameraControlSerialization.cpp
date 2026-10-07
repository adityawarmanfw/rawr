#include "camera/CameraControlSerialization.h"

#include "camera/CameraProfileJson.h"

#include <iomanip>
#include <sstream>

namespace rawrcam::camera {
namespace {
std::string quoted(const std::string& value) {
    std::string out;
    appendJsonString(out, value);
    return out;
}
void optInt64(std::ostringstream& s, const std::optional<int64_t>& v) {
    if (v)
        s << *v;
    else
        s << "null";
}
void optInt32(std::ostringstream& s, const std::optional<int32_t>& v) {
    if (v)
        s << *v;
    else
        s << "null";
}
void optU8(std::ostringstream& s, const std::optional<uint8_t>& v) {
    if (v)
        s << static_cast<int>(*v);
    else
        s << "null";
}
void optFloat(std::ostringstream& s, const std::optional<float>& v) {
    if (v)
        s << std::setprecision(7) << *v;
    else
        s << "null";
}
void optDouble(std::ostringstream& s, const std::optional<double>& v) {
    if (v)
        s << std::setprecision(7) << *v;
    else
        s << "null";
}
}  // namespace

std::string serializeCameraControlState(const CameraControlState& st) {
    const auto& c = st.capabilities;
    std::ostringstream s;
    s << '{' << "\"generation\":" << c.generation << ",\"cameraId\":" << quoted(c.cameraId)
      << ",\"lensId\":" << quoted(c.lensId) << ",\"rawWidth\":" << c.rawWidth << ",\"rawHeight\":" << c.rawHeight
      << ",\"cameraProfile\":" << quoted(c.cameraProfileId)
      << ",\"profileLenses\":[";
    for (size_t i = 0; i < c.profileLenses.size(); ++i)
        s << (i ? "," : "") << "{\"id\":" << quoted(c.profileLenses[i].first)
          << ",\"label\":" << quoted(c.profileLenses[i].second) << '}';
    s << ']'
      << ",\"sensitivityMin\":" << c.sensitivityMin << ",\"sensitivityMax\":" << c.sensitivityMax
      << ",\"exposureTimeMinNs\":" << c.exposureTimeMinNs << ",\"exposureTimeMaxNs\":" << c.exposureTimeMaxNs
      << ",\"evMinSteps\":" << c.evMinSteps << ",\"evMaxSteps\":" << c.evMaxSteps
      << ",\"evStepNumerator\":" << c.evStepNumerator << ",\"evStepDenominator\":" << c.evStepDenominator
      << ",\"manualExposureSupported\":" << (c.manualExposureSupported ? "true" : "false")
      << ",\"shutterPrioritySupported\":" << (c.shutterPrioritySupported ? "true" : "false")
      << ",\"isoPrioritySupported\":" << (c.isoPrioritySupported ? "true" : "false")
       << ",\"tapAfSupported\":" << (c.tapAfSupported ? "true" : "false")
       << ",\"faceDetectSupported\":" << (c.faceDetectSupported ? "true" : "false")
      << ",\"manualFocusSupported\":" << (c.manualFocusSupported ? "true" : "false")
      << ",\"focusDistanceReadoutTrustworthy\":" << (c.focusDistanceReadoutTrustworthy ? "true" : "false")
      << ",\"oisSupported\":" << (c.oisSupported ? "true" : "false")
      << ",\"minimumFocusDistance\":" << std::setprecision(7) << c.minimumFocusDistance
      << ",\"hyperfocalDistance\":" << std::setprecision(7) << c.hyperfocalDistance
      << ",\"maxAfRegions\":" << c.maxAfRegions << ",\"maxAeRegions\":" << c.maxAeRegions
       << ",\"exposureMode\":" << static_cast<int>(st.exposureMode)
       << ",\"focusMode\":" << static_cast<int>(st.focusMode)
       << ",\"tapAfActive\":" << (st.tapAfActive ? "true" : "false")
       << ",\"focusRequestId\":" << st.focusRequestId
       << ",\"spotAeActive\":" << (st.spotAeActive ? "true" : "false")
       << ",\"videoMode\":" << (st.videoMode ? "true" : "false")
       << ",\"videoPreviewFps\":" << st.videoPreviewFps
       << ",\"whiteBalanceMode\":" << static_cast<int>(st.whiteBalanceMode)
       << ",\"whiteBalanceTemperatureK\":" << st.requestedWhiteBalanceTemperatureK
       << ",\"whiteBalanceTint\":" << st.requestedWhiteBalanceTint
       << ",\"manualGainsSupported\":" << (c.manualGainsSupported ? "true" : "false")
       << ",\"hasAutoWbGains\":" << (st.hasLastHalAwbGains ? "true" : "false")
       // Raw gains remain diagnostic telemetry; all estimation is native.
       << ",\"autoWbGainR\":" << std::setprecision(7) << st.lastHalAwbGains[0]
       << ",\"autoWbGainG\":" << std::setprecision(7) << (0.5f * (st.lastHalAwbGains[1] + st.lastHalAwbGains[2]))
       << ",\"autoWbGainB\":" << std::setprecision(7) << st.lastHalAwbGains[3]
       << ",\"hasAutoWbEstimate\":" << (st.hasAutoWbEstimate ? "true" : "false")
       << ",\"autoWbTemperatureK\":" << st.autoWbTemperatureK
       << ",\"autoWbTint\":" << st.autoWbTint
       << ",\"autoWbEstimateCalibrated\":" << (st.autoWbEstimateCalibrated ? "true" : "false")
       << ",\"whiteBalanceRequestId\":" << st.whiteBalanceRequestId
       << ",\"supportedAwbModes\":[";
    for (size_t i = 0; i < c.supportedAwbModes.size(); ++i) {
        if (i) s << ',';
        s << static_cast<int>(c.supportedAwbModes[i]);
    }
    s << ']' << ",\"requestedShutterAngleDegrees\":";
    optDouble(s, st.requestedShutterAngleDegrees);
    s << ",\"shutterAngleChoices\":[";
    for (size_t i = 0; i < st.shutterAngleChoices.size(); ++i) {
        if (i) s << ',';
        const auto& choice = st.shutterAngleChoices[i];
        s << "{\"degrees\":" << choice.degrees << ",\"exposureTimeNs\":" << choice.exposureTimeNs << '}';
    }
    s << ']' << ",\"recordingFps\":" << st.recordingFps
      << ",\"recordingExposureLimitNs\":"
      << (st.recordingFps > 0 ? 1'000'000'000LL / st.recordingFps : 0)
      << ",\"requestedExposureTimeNs\":" << st.requestedExposureTimeNs
      << ",\"requestedSensitivity\":" << st.requestedSensitivity << ",\"requestedEvSteps\":" << st.requestedEvSteps
      << ",\"requestedManualFocusNormalized\":" << st.requestedManualFocusNormalized
      << ",\"oisEnabled\":" << (st.oisEnabled ? "true" : "false") << ",\"appliedExposureTimeNs\":";
    optInt64(s, st.appliedExposureTimeNs);
    s << ",\"appliedSensitivity\":";
    optInt32(s, st.appliedSensitivity);
    s << ",\"appliedEvSteps\":";
    optInt32(s, st.appliedEvSteps);
    s << ",\"afState\":";
    optU8(s, st.afState);
    s << ",\"appliedFocusDistance\":";
    optFloat(s, st.appliedFocusDistance);
    s << ",\"appliedPostRawSensitivityBoost\":";
    optInt32(s, st.appliedPostRawSensitivityBoost);
    s << ",\"appliedRawFps\":";
    optDouble(s, st.appliedRawFps);
    s << ",\"measuredViewfinderFps\":";
    optDouble(s, st.measuredViewfinderFps);
    s << '}';
    return s.str();
}
}  // namespace rawrcam::camera
