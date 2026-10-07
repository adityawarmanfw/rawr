#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>

#include "camera/CameraResultState.h"

using namespace rawrcam::camera;
using rawrcam::metadata::FrameMetadataSnapshot;

CameraControlState controls(ExposureControlMode mode = ExposureControlMode::Auto) {
    CameraControlState state;
    state.exposureMode = mode;
    state.requestedExposureTimeNs = 10000000;
    state.requestedSensitivity = 100;
    state.capabilities.priorityAeTargetFpsMin = 15;
    state.capabilities.priorityAeTargetFpsMax = 30;
    return state;
}

FrameMetadataSnapshot frame(uint64_t timestamp = 1000000000) {
    FrameMetadataSnapshot result;
    result.timestampNs = timestamp;
    result.exposureTimeNs = 10000000;
    result.sensitivity = 100;
    result.awbMode = 1;
    result.colorCorrectionGainsRggb = {2, 1, 1.1f, 1.5f};
    result.hasNeutralColorPoint = true;
    result.neutralColorPoint = {0.5f, 1, 0.7f};
    result.colorCorrectionTransform.valid = true;
    result.colorCorrectionTransform.rowMajor = {1.2f, 0, 0, 0, 1, 0, 0, 0, 0.8f};
    return result;
}

CameraControlResult echoes() {
    CameraControlResult result;
    result.targetFpsRange = std::array<int32_t, 2>{15, 30};
    result.evSteps = 0;
    result.afState = 0;
    result.focusDistance = 0;
    result.faces = {FaceDetection{0.2f, 0.3f, 0.1f, 0.2f, 80}};
    return result;
}

void requestProvenance() {
    auto state = controls(ExposureControlMode::ShutterPriority);
    const auto request = CameraRequestProvenance::from(state, 42);
    state.requestedExposureTimeNs = 20000000;
    assert(request.requestSerial == 42 && request.requestedExposureTimeNs == 10000000);
    assert(request.requestedSensitivity == 100 && request.targetFpsMin == 15 && request.targetFpsMax == 30);
    assert(request.expectedAePriority == camera2_priority::kSensorExposureTimePriority);
    assert(CameraRequestProvenance::from(controls(ExposureControlMode::IsoPriority), 1).expectedAePriority ==
           camera2_priority::kSensorSensitivityPriority);
    assert(CameraRequestProvenance::from(controls(ExposureControlMode::Manual), 1).expectedAePriority ==
           camera2_priority::kOff);
    assert(request.optimizedStillRequestId == 0);
}

void repeatingTelemetryAndOneShotIsolation() {
    CameraResultState results;
    auto state = controls();
    auto request = CameraRequestProvenance::from(state, 1);
    auto observedFrame = frame();
    auto result = echoes();
    observedFrame.sensitivity = 250;
    observedFrame.exposureTimeNs = 16000000;
    observedFrame.postRawSensitivityBoost = 50;
    result.frameDurationNs = 40000000;
    // An older repeating request is still a delivered preview frame.
    assert(!results.observe(state, observedFrame, result, &request, 2, false));
    assert(state.appliedSensitivity == 250 && state.appliedExposureTimeNs == 16000000);
    assert(state.appliedPostRawSensitivityBoost == 100 && state.appliedRawFps == 25);
    assert(state.appliedEvSteps == 0 && state.afState == 0 && state.appliedFocusDistance == 0);
    assert(state.faceDetections.size() == 1 && state.faceDetections.front().score == 80);
    assert(state.requestedSensitivity == 100 && state.requestedExposureTimeNs == 10000000);
    assert(state.hasLastHalAwbGains && state.hasLastHalNeutral && state.hasManualWhiteBalanceTransformSeed);
    const auto seed = state.lastHalAwbGains;

    request.optimizedStillRequestId = 99;
    observedFrame.sensitivity = 800;
    observedFrame.exposureTimeNs = 30000000;
    observedFrame.colorCorrectionGainsRggb = {3, 3, 3, 3};
    result.evSteps = 3;
    result.afState = 4;
    result.focusDistance = 2;
    result.faces.clear();
    result.frameDurationNs = 100000000;
    results.observe(state, observedFrame, result, &request, 1, false);
    assert(state.appliedSensitivity == 250 && state.appliedExposureTimeNs == 16000000);
    assert(state.appliedRawFps == 25 && state.appliedEvSteps == 0 && state.afState == 0 &&
           state.appliedFocusDistance == 0);
    assert(state.faceDetections.size() == 1 && state.lastHalAwbGains == seed);

    request.optimizedStillRequestId = 0;
    results.observe(state, observedFrame, {}, &request, 1, false);
    assert(state.faceDetections.empty());
    // Optional absent tags retain their last available telemetry values.
    assert(state.appliedRawFps == 25 && state.appliedEvSteps == 0 && state.afState == 0 &&
           state.appliedFocusDistance == 0);
}

void frameRateObservation() {
    CameraResultState results;
    auto state = controls();
    auto observedFrame = frame();
    auto result = echoes();
    result.frameDurationNs = 40000000;
    results.observe(state, observedFrame, result, nullptr, 0, false);
    assert(!state.measuredViewfinderFps && state.appliedRawFps == 25);
    observedFrame.timestampNs += 40000000;
    results.observe(state, observedFrame, result, nullptr, 0, false);
    assert(state.measuredViewfinderFps == 25);
    observedFrame.timestampNs += 20000000;
    result.frameDurationNs = 0;
    results.observe(state, observedFrame, result, nullptr, 0, false);
    assert(state.appliedRawFps == 30);
    assert(std::abs(*state.measuredViewfinderFps - 27.5) < 1e-8);
    observedFrame.timestampNs += 500000;  // Tiny deltas do not create extreme FPS.
    results.observe(state, observedFrame, result, nullptr, 0, false);
    assert(std::abs(*state.measuredViewfinderFps - 27.5) < 1e-8);
    observedFrame.timestampNs += 1000000001;
    results.observe(state, observedFrame, result, nullptr, 0, false);
    assert(!state.measuredViewfinderFps);
    observedFrame.timestampNs += 50000000;
    results.observe(state, observedFrame, result, nullptr, 0, false);
    assert(state.measuredViewfinderFps == 20);
    results.reset();
    state = controls();
    results.observe(state, frame(), {}, nullptr, 0, false);
    assert(!state.measuredViewfinderFps && !state.appliedRawFps);
    results.observe(state, frame(1040000000), {}, nullptr, 0, false);
    assert(state.measuredViewfinderFps == 25);
}

void whiteBalanceFreezing() {
    CameraResultState results;
    auto state = controls();
    auto observedFrame = frame();
    results.observe(state, observedFrame, {}, nullptr, 0, false);
    const auto gains = state.lastHalAwbGains;
    const auto neutral = state.lastHalNeutral;
    const auto transform = state.manualWhiteBalanceTransformSeed;
    state.whiteBalanceMode = WhiteBalanceControlMode::ManualTempTint;
    observedFrame.colorCorrectionGainsRggb = {4, 4, 4, 4};
    observedFrame.neutralColorPoint = {0.2f, 0.2f, 0.2f};
    observedFrame.colorCorrectionTransform.rowMajor.fill(2);
    results.observe(state, observedFrame, {}, nullptr, 0, false);
    assert(state.lastHalAwbGains == gains && state.lastHalNeutral == neutral &&
           state.manualWhiteBalanceTransformSeed == transform);
    state.whiteBalanceMode = WhiteBalanceControlMode::Daylight;
    observedFrame.awbMode = 0;  // Old manual echo cannot become a gains/neutral seed.
    results.observe(state, observedFrame, {}, nullptr, 0, false);
    assert(state.lastHalAwbGains == gains && state.lastHalNeutral == neutral);
    observedFrame.awbMode = 5;
    observedFrame.colorCorrectionGainsRggb[0] = std::numeric_limits<float>::quiet_NaN();
    observedFrame.neutralColorPoint[1] = -1;
    observedFrame.colorCorrectionTransform.valid = false;
    results.observe(state, observedFrame, {}, nullptr, 0, false);
    assert(state.lastHalAwbGains == gains && state.lastHalNeutral == neutral);
    observedFrame.colorCorrectionGainsRggb = {3, 1, 1, 2};
    observedFrame.neutralColorPoint = {0.3f, 1, 0.5f};
    results.observe(state, observedFrame, {}, nullptr, 0, false);
    assert(state.lastHalAwbGains == observedFrame.colorCorrectionGainsRggb &&
           state.lastHalNeutral == observedFrame.neutralColorPoint);
}

void sensitivityCoordinate() {
    CameraResultState results;
    auto state = controls();
    auto observedFrame = frame();
    observedFrame.sensitivity = 800;
    auto request = CameraRequestProvenance::from(state, 1);
    results.observe(state, observedFrame, echoes(), &request, 1, true);
    assert(!results.sensitivityReportedPerRequest());  // Auto does not own ISO.
    request.exposureMode = ExposureControlMode::ShutterPriority;
    results.observe(state, observedFrame, echoes(), &request, 1, true);
    assert(!results.sensitivityReportedPerRequest());
    request.exposureMode = ExposureControlMode::IsoPriority;
    auto unknown = results.observe(state, observedFrame, echoes(), &request, 1, true);
    assert(unknown && !unknown->ownedAxisMismatch && !unknown->expectedReportedSensitivity);
    assert(!results.sensitivityReportedPerRequest());  // I does not train mapping.
    request.exposureMode = ExposureControlMode::Manual;
    results.observe(state, observedFrame, echoes(), &request, 1, true);
    assert(results.sensitivityReportedPerRequest() == 8);
    request.exposureMode = ExposureControlMode::IsoPriority;
    request.requestedSensitivity = 200;
    observedFrame.sensitivity = 1600;
    auto valid = results.observe(state, observedFrame, echoes(), &request, 1, true);
    assert(valid && !valid->ownedAxisMismatch && valid->expectedReportedSensitivity == 1600 &&
           valid->sensitivityTolerance == 32);
    observedFrame.sensitivity = 1632;
    assert(!results.observe(state, observedFrame, echoes(), &request, 1, true)->ownedAxisMismatch);
    observedFrame.sensitivity = 1633;
    assert(results.observe(state, observedFrame, echoes(), &request, 1, true)->ownedAxisMismatch);
    assert(results.observe(state, observedFrame, echoes(), &request, 1, true)->fallbackToAuto);
    results.reset();
    assert(!results.sensitivityReportedPerRequest());
    request.requestedSensitivity = 100;
    observedFrame.sensitivity = 101;
    assert(!results.observe(state, observedFrame, echoes(), &request, 1, false)->ownedAxisMismatch);
    observedFrame.sensitivity = 102;
    assert(results.observe(state, observedFrame, echoes(), &request, 1, false)->ownedAxisMismatch);
    request.exposureMode = ExposureControlMode::Manual;
    request.requestedSensitivity = 0;
    results.observe(state, observedFrame, {}, &request, 1, false);
    assert(!results.sensitivityReportedPerRequest());
}

void sensitivityEndpoints() {
    CameraResultState results;
    auto state = controls();
    state.capabilities.sensitivityMin = 50;
    state.capabilities.sensitivityMax = 3200;
    auto request = CameraRequestProvenance::from(state, 1);
    request.exposureMode = ExposureControlMode::Manual;
    request.optimizedStillRequestId = 99;
    auto observed = frame();
    request.requestedSensitivity = 50;
    observed.sensitivity = 800;  // Floor would incorrectly train 16x.
    results.observe(state, observed, {}, &request, 1, true);
    assert(!results.sensitivityReportedPerRequest());
    request.requestedSensitivity = 400;
    observed.sensitivity = 3200;
    results.observe(state, observed, {}, &request, 1, true);
    assert(results.sensitivityReportedPerRequest() == 8);
    request.requestedSensitivity = 50;
    observed.sensitivity = 800;
    results.observe(state, observed, {}, &request, 1, true);
    assert(results.sensitivityReportedPerRequest() == 8);  // Dark frame cannot poison it.
    request.requestedSensitivity = 3200;
    observed.sensitivity = 12800;
    results.observe(state, observed, {}, &request, 1, true);
    assert(results.sensitivityReportedPerRequest() == 8);
    request.requestedSensitivity = 800;
    observed.sensitivity = 6400;  // Report coordinate exceeds manual request max.
    results.observe(state, observed, {}, &request, 1, true);
    assert(results.sensitivityReportedPerRequest() == 8);
    request.exposureMode = ExposureControlMode::Auto;
    request.optimizedStillRequestId = 0;
    observed.sensitivity = 25600;  // AE is allowed an even wider range.
    results.observe(state, observed, {}, &request, 1, true);
    assert(state.appliedSensitivity == 25600);
    assert(results.sensitivityReportedPerRequest() == 8);
}

void priorityContract() {
    CameraResultState results;
    auto state = controls(ExposureControlMode::ShutterPriority);
    auto request = CameraRequestProvenance::from(state, 7);
    auto observedFrame = frame();
    auto result = echoes();
    auto valid = results.observe(state, observedFrame, result, &request, 7, false);
    assert(valid && !valid->priorityMismatch && !valid->ownedAxisMismatch && !valid->fpsMismatch);
    assert(valid->shouldLog && !valid->fallbackToAuto);  // Missing priority echo is allowed.
    observedFrame.exposureTimeNs += 100000;
    assert(!results.observe(state, observedFrame, result, &request, 7, false)->ownedAxisMismatch);
    observedFrame.exposureTimeNs += 1;
    assert(!results.observe(state, observedFrame, result, &request, 7, false)->fallbackToAuto);
    auto failure = results.observe(state, observedFrame, result, &request, 7, false);
    assert(failure->fallbackToAuto && failure->ownedAxisMismatch);
    assert(state.exposureMode == ExposureControlMode::ShutterPriority);  // Caller executes returned action.
    const auto message = describeCameraPriorityAudit(*failure);
    assert(message.find("CAMERA_AE_PRIORITY_VIOLATION serial=7 latest=true") == 0);
    assert(message.find("reasons=owned_axis,") != std::string::npos);
    assert(message.find("resultPriority=missing") != std::string::npos);

    results.reset();
    observedFrame = frame();
    result.targetFpsRange.reset();
    for (int i = 0; i < 4; ++i) {
        const auto missingFps = results.observe(state, observedFrame, result, &request, 7, false);
        assert(missingFps->fpsMismatch && !missingFps->fallbackToAuto);  // FPS alone is not a hard failure.
    }
    results.reset();
    result = echoes();
    result.aePriority = camera2_priority::kOff;
    for (int i = 0; i < 4; ++i) {
        const auto stale = results.observe(state, observedFrame, result, &request, 8, false);
        assert(stale->priorityMismatch && !stale->latestRequest && !stale->fallbackToAuto);
    }
    assert(!results.observe(state, observedFrame, result, &request, 7, false)->fallbackToAuto);
    result.aePriority = request.expectedAePriority;
    assert(
        !results.observe(state, observedFrame, result, &request, 7, false)->fallbackToAuto);  // Success clears streak.
    result.aePriority = camera2_priority::kOff;
    assert(!results.observe(state, observedFrame, result, &request, 7, false)->fallbackToAuto);
    assert(results.observe(state, observedFrame, result, &request, 7, false)->fallbackToAuto);
    ++request.requestSerial;  // A new submitted request starts a fresh audit.
    assert(!results.observe(state, observedFrame, result, &request, 8, false)->fallbackToAuto);
    results.reset();
    result.aePriority = request.expectedAePriority;
    for (int i = 0; i < 12; ++i) assert(results.observe(state, observedFrame, result, &request, 8, false)->shouldLog);
    assert(!results.observe(state, observedFrame, result, &request, 8, false)->shouldLog);
}

void faceNormalization() {
    const rawrcam::metadata::RectI active{100, 200, 1100, 2200, true};
    const int32_t rectangles[] = {50,  100, 600, 1200, 600, 1200, 1200, 2400, 500,  500,
                                  400, 600, 200, 400,  400, 800,  100,  200,  1100, 2200};
    const uint8_t scores[] = {20, 90, 100, 0, 100};
    const auto faces = normalizeCameraFaces(active, rectangles, 20, scores, 5);
    assert(faces.size() == 3);  // Invalid rectangle removed; fifth exceeds capture cap.
    assert(faces[0].score == 90 && faces[1].score == 50 && faces[2].score == 20);
    assert(faces[2].x == 0 && faces[2].y == 0 && faces[2].w == 0.5f && faces[2].h == 0.5f);
    assert(faces[0].x == 0.5f && faces[0].y == 0.5f && faces[0].w == 0.5f && faces[0].h == 0.5f);
    assert(normalizeCameraFaces(active, rectangles, 3, nullptr, 0).empty());
    assert(normalizeCameraFaces({}, rectangles, 20, scores, 5).empty());
    assert(normalizeCameraFaces(active, nullptr, 20, scores, 5).empty());
    assert(normalizeCameraFaces(active, rectangles, 4, nullptr, 0).front().score == 50);
}

int main() {
    requestProvenance();
    repeatingTelemetryAndOneShotIsolation();
    frameRateObservation();
    whiteBalanceFreezing();
    sensitivityCoordinate();
    sensitivityEndpoints();
    priorityContract();
    faceNormalization();
    std::cout << "CAMERA_RESULT_STATE_PASS\n";
}
