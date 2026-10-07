#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>

#include "camera/CameraFocusControls.h"
#include "camera/CameraRequestProvenance.h"
#include "camera/CameraResultState.h"

using namespace rawrcam::camera;

CameraControlState controls() {
    CameraControlState state;
    state.capabilities.tapAfSupported = true;
    state.capabilities.manualFocusSupported = true;
    state.capabilities.minimumFocusDistance = 10;
    state.capabilities.faceDetectSupported = true;
    state.capabilities.maxAfRegions = 1;
    state.capabilities.maxAeRegions = 1;
    state.capabilities.priorityAeTargetFpsMin = 5;
    state.capabilities.priorityAeTargetFpsMax = 30;
    state.requestedSensitivity = 100;
    state.requestedExposureTimeNs = 10000000;
    return state;
}

rawrcam::metadata::SensorGeometry geometry() {
    rawrcam::metadata::SensorGeometry geometry;
    geometry.activeArray = {0, 0, 1000, 2000, true};
    geometry.preCorrectionActiveArray = {100, 200, 1100, 2200, true};
    return geometry;
}

void regionGeometry() {
    auto g = geometry();
    const auto tap = tapFocusRegion(g, {0.5f, 0.5f});
    assert(tap && *tap == (CameraMeteringRegion{550, 1100, 650, 1300, 1000}));
    assert(*tapFocusRegion(g, {0, 0}) == (CameraMeteringRegion{100, 200, 150, 300, 1000}));
    assert(*tapFocusRegion(g, {1, 1}) == (CameraMeteringRegion{1049, 2099, 1100, 2200, 1000}));
    assert(tapFocusRegion(g, {-10, 20}) == tapFocusRegion(g, {0, 1}));
    assert(!tapFocusRegion(g, {std::numeric_limits<float>::quiet_NaN(), 0.5f}));
    assert(!tapFocusRegion(g, {0.5f, std::numeric_limits<float>::infinity()}));
    const FaceDetection face{0.4f, 0.4f, 0.2f, 0.2f, 80};
    // Regression: a non-zero array origin must be added for face regions too.
    assert(*faceFocusRegion(g, face) == (CameraMeteringRegion{475, 950, 725, 1450, 1000}));
    g.preCorrectionActiveArray.valid = false;
    assert(*faceFocusRegion(g, face) == (CameraMeteringRegion{375, 750, 625, 1250, 1000}));
    assert(*tapFocusRegion(g, {0.5f, 0.5f}) == (CameraMeteringRegion{450, 900, 550, 1100, 1000}));
    g.activeArray = {10, 20, 11, 21, true};
    assert(*tapFocusRegion(g, {0.5f, 0.5f}) == (CameraMeteringRegion{10, 20, 11, 21, 1000}));
    g.activeArray = {10, 20, 10, 21, true};
    assert(!tapFocusRegion(g, {0.5f, 0.5f}));
    assert(!faceFocusRegion(g, face));
    assert(!faceFocusRegion(geometry(), FaceDetection{0.5f, 0.5f, 0, 0.1f, 80}));
    assert(!faceFocusRegion(geometry(), FaceDetection{0.5f, 0.5f, std::numeric_limits<float>::max(), 0.1f, 80}));
}

void modeAndManualFocus() {
    CameraFocusControls focus;
    auto state = controls();
    state.capabilities.manualFocusSupported = false;
    assert(!focus.requestMode(state, FocusControlMode::Manual, 1));
    assert(state.focusMode == FocusControlMode::Continuous && state.focusRequestId == 1);
    state.capabilities.tapAfSupported = false;
    assert(!focus.requestMode(state, FocusControlMode::LockedAuto, 2));
    assert(state.focusRequestId == 2);
    state.capabilities.manualFocusSupported = true;
    state.appliedFocusDistance = 4;
    assert(focus.requestMode(state, FocusControlMode::Manual, 3));
    assert(std::abs(state.requestedManualFocusNormalized - 0.6f) < 1e-6f);
    assert(focus.request(std::nullopt).trigger == CameraFocusTrigger::Cancel);
    assert(!focus.request(std::nullopt).afRegion && !state.tapAfActive);
    assert(focus.requestManualFocus(state, 2, 4));
    assert(state.requestedManualFocusNormalized == 1);
    assert(focus.requestManualFocus(state, -2, 5));
    assert(state.requestedManualFocusNormalized == 0);
    assert(!focus.requestManualFocus(state, std::numeric_limits<float>::quiet_NaN(), 6));
    assert(state.focusRequestId == 6 && state.requestedManualFocusNormalized == 0);
    assert(focus.finishTrigger() && !focus.finishTrigger());
}

void tapSequenceAndExpiry() {
    CameraFocusControls focus;
    auto state = controls();
    const auto first = focus.planTap(state, geometry(), {0.2f, 0.2f});
    assert(first && !first->cancelFirst);
    focus.acceptTap(state, *first, 100);
    assert(state.tapAfActive && focus.request(std::nullopt).trigger == CameraFocusTrigger::Start);
    assert(focus.request(std::nullopt).afRegion == first->region);
    assert(!focus.expireTap(state, 4099));
    const auto retap = focus.planTap(state, geometry(), {0.8f, 0.8f});
    assert(retap && retap->cancelFirst);
    focus.cancelTrigger();
    assert(focus.request(std::nullopt).trigger == CameraFocusTrigger::Cancel);
    assert(focus.request(std::nullopt).afRegion == first->region);  // CANCEL retains old region until START commit.
    focus.acceptTap(state, *retap, 3000);
    assert(focus.request(std::nullopt).afRegion == retap->region);
    assert(focus.request(std::nullopt).trigger == CameraFocusTrigger::Start);
    assert(focus.finishTrigger() && focus.request(std::nullopt).trigger == CameraFocusTrigger::Idle);
    assert(!focus.expireTap(state, 6999));
    assert(focus.expireTap(state, 7000));
    assert(!state.tapAfActive && !focus.request(std::nullopt).afRegion);
    assert(focus.request(std::nullopt).trigger == CameraFocusTrigger::Cancel);
    assert(!focus.expireTap(state, 8000));
    const auto afterCancel = focus.planTap(state, geometry(), {0.5f, 0.5f});
    assert(afterCancel && afterCancel->cancelFirst);  // Pending CANCEL still needs an ordered new START.
    focus.reset(state);
    assert(focus.request(std::nullopt).trigger == CameraFocusTrigger::Idle);
    assert(!focus.request(std::nullopt).afRegion && !focus.expireTap(state, 100000));
    state.capabilities.tapAfSupported = false;
    assert(!focus.planTap(state, geometry(), {0.5f, 0.5f}));
}

void lockedTapAndModeCleanup() {
    CameraFocusControls focus;
    auto state = controls();
    assert(focus.requestMode(state, FocusControlMode::LockedAuto, 1));
    auto locked = focus.planTap(state, geometry(), {0.5f, 0.5f});
    assert(locked && locked->cancelFirst);
    focus.acceptTap(state, *locked, 100);
    assert(!focus.expireTap(state, 1000000));
    assert(focus.requestMode(state, FocusControlMode::Manual, 2));
    assert(!state.tapAfActive && !focus.request(std::nullopt).afRegion && !focus.expireTap(state, 1000000));
    assert(focus.requestMode(state, FocusControlMode::Continuous, 3));
    assert(focus.request(std::nullopt).trigger == CameraFocusTrigger::Cancel);
    focus.finishTrigger();
    const auto tap = focus.planTap(state, geometry(), {0.5f, 0.5f});
    focus.acceptTap(state, *tap, 100);
    focus.clearTap(state);
    assert(!state.tapAfActive && !focus.expireTap(state, 1000000));
}

void tapLeavesManualFocus() {
    CameraFocusControls focus;
    auto state = controls();
    assert(focus.requestManualFocus(state, 1.0f, 1));
    assert(state.focusMode == FocusControlMode::Manual);
    const auto tap = focus.planTap(state, geometry(), {0.3f, 0.6f});
    assert(tap);
    focus.acceptTap(state, *tap, 100);
    assert(state.focusMode == FocusControlMode::Continuous && state.tapAfActive);
    assert(focus.request(std::nullopt).afRegion == tap->region);
    assert(focus.expireTap(state, 4100) && !state.tapAfActive);
}

void faceHysteresisAndTapOwnership() {
    CameraFocusControls focus;
    auto state = controls();
    auto g = geometry();
    state.faceDetections = {FaceDetection{0.4f, 0.4f, 0.2f, 0.2f, 80}};
    assert(focus.steerFaces(state, &g, true) == FaceFocusChange::Updated);
    assert(focus.request(std::nullopt).trigger == CameraFocusTrigger::Idle);  // Face tracking never starts a trigger.
    const auto initial = focus.request(std::nullopt).afRegion;
    state.faceDetections.front().x += 0.01f;
    assert(focus.steerFaces(state, &g, true) == FaceFocusChange::None);
    assert(focus.request(std::nullopt).afRegion == initial);
    state.faceDetections.front().x += 0.03f;
    assert(focus.steerFaces(state, &g, true) == FaceFocusChange::Updated);
    state.faceDetections.clear();
    assert(focus.steerFaces(state, &g, true) == FaceFocusChange::Cleared);
    assert(!focus.request(std::nullopt).afRegion);
    assert(focus.steerFaces(state, &g, true) == FaceFocusChange::None);
    state.faceDetections = {FaceDetection{0.4f, 0.4f, 0.2f, 0.2f, 80}};
    const auto tap = focus.planTap(state, g, {0.2f, 0.2f});
    focus.acceptTap(state, *tap, 100);
    assert(focus.steerFaces(state, &g, true) == FaceFocusChange::None);
    assert(focus.request(std::nullopt).afRegion == tap->region);
    focus.expireTap(state, 4100);
    assert(focus.steerFaces(state, &g, true) == FaceFocusChange::Updated);
    assert(focus.requestMode(state, FocusControlMode::Manual, 1));
    assert(focus.steerFaces(state, &g, true) == FaceFocusChange::None);
    assert(!focus.request(std::nullopt).afRegion);
    focus.reset(state);
    state.focusMode = FocusControlMode::Continuous;
    assert(focus.steerFaces(state, nullptr, true) == FaceFocusChange::None);
    assert(focus.steerFaces(state, &g, false) == FaceFocusChange::None);
    state.capabilities.maxAfRegions = 0;
    assert(focus.steerFaces(state, &g, true) == FaceFocusChange::None);
}

void spotMeteringOwnership() {
    CameraSpotMeteringControls spot;
    auto state = controls();
    auto g = geometry();
    assert(!spot.region(state, &g));
    assert(spot.requestTarget(state, true, {0.5f, 0.5f}));
    assert(*spot.region(state, &g) == (CameraMeteringRegion{538, 1075, 662, 1325, 1000}));
    assert(!spot.region(state, nullptr));
    state.capabilities.maxAeRegions = 0;
    assert(!spot.region(state, &g));
    state.capabilities.maxAeRegions = 1;
    state.exposureMode = ExposureControlMode::ShutterPriority;
    assert(!spot.region(state, &g));
    assert(!spot.requestTarget(state, true, {0, 0}));
    assert(spot.point().x == 0.5f);
    assert(spot.requestTarget(state, false, {std::numeric_limits<float>::quiet_NaN(), 0}));
    assert(!state.spotAeActive);
    state.exposureMode = ExposureControlMode::Auto;
    assert(!spot.requestTarget(state, true, {std::numeric_limits<float>::infinity(), 0}));
    assert(!state.spotAeActive);
    assert(spot.requestTarget(state, true, {-2, 2}));
    assert(spot.point().x == 0 && spot.point().y == 1);
}

void sharedRequestCadence() {
    auto state = controls();
    assert(cameraTargetFpsRange(state, false) == (std::array<int32_t, 2>{15, 30}));
    state.autoMinFps = 30;
    assert(!cameraTargetFpsRange(state, false));
    state.exposureMode = ExposureControlMode::ShutterPriority;
    assert(cameraTargetFpsRange(state, true) == (std::array<int32_t, 2>{5, 30}));
    state.autoMinFps = 15;
    assert(cameraTargetFpsRange(state, true) == (std::array<int32_t, 2>{15, 30}));
    auto request = CameraRequestProvenance::from(state, 1);
    assert(request.targetFpsMin == 15 && request.targetFpsMax == 30);  // Actual floor, not raw capability minimum.
    state.capabilities.priorityAeTargetFpsMax = 10;
    assert(cameraTargetFpsRange(state, true) == (std::array<int32_t, 2>{10, 10}));
    state.recordingFps = 24;
    assert(cameraTargetFpsRange(state, true) == (std::array<int32_t, 2>{24, 24}));
    request = CameraRequestProvenance::from(state, 2);
    assert(request.targetFpsMin == 24 && request.targetFpsMax == 24);
    CameraResultState results;
    rawrcam::metadata::FrameMetadataSnapshot frame;
    frame.exposureTimeNs = request.requestedExposureTimeNs;
    frame.sensitivity = request.requestedSensitivity;
    CameraControlResult result;
    result.targetFpsRange = cameraTargetFpsRange(state, true);
    const auto audit = results.observe(state, frame, result, &request, 2, false);
    assert(audit && !audit->fpsMismatch && !audit->fallbackToAuto);
    state.recordingFps = 0;
    state.exposureMode = ExposureControlMode::Manual;
    assert(!cameraTargetFpsRange(state, false));
    state.exposureMode = ExposureControlMode::Auto;
    state.capabilities.priorityAeTargetFpsMax = 0;
    assert(cameraTargetFpsRange(state, false) == (std::array<int32_t, 2>{15, 30}));
}

int main() {
    regionGeometry();
    modeAndManualFocus();
    tapLeavesManualFocus();
    tapSequenceAndExpiry();
    lockedTapAndModeCleanup();
    faceHysteresisAndTapOwnership();
    spotMeteringOwnership();
    sharedRequestCadence();
    std::cout << "CAMERA_FOCUS_CONTROLS_PASS\n";
}
