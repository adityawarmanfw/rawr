#include <cassert>
#include <iostream>
#include <limits>
#include <string>

#include "camera/CameraCadencePolicy.h"
#include "camera/CameraControlSerialization.h"
#include "camera/TapFocusPolicy.h"

using namespace rawrcam::camera;
constexpr int64_t kMinNs = 100000;
constexpr int64_t kMaxNs = 1000000000;

void angleIntent() {
    CameraCadencePolicy policy;
    assert(policy.shutterChoices(kMinNs, kMaxNs).empty());
    assert(!policy.selectShutterAngle(180, kMinNs, kMaxNs));
    policy.setVideoMode(true, 24);
    const auto choices = policy.shutterChoices(kMinNs, kMaxNs);
    assert(choices.size() == 12);
    assert(choices.front().exposureTimeNs == kMinNs && choices.back().degrees == 360);
    assert(choices[9].exposureTimeNs == 20833333);
    assert(choices.back().exposureTimeNs == 41666666);
    assert(policy.selectShutterAngle(270, kMinNs, kMaxNs));
    assert(policy.heldExposure(10000000, kMinNs, kMaxNs) == 31250000);
    policy.setVideoMode(true, 30);
    assert(policy.shutterAngle() == 270);
    assert(policy.heldExposure(31250000, kMinNs, kMaxNs) == 25000000);
    policy.setVideoMode(true, 24);
    assert(policy.heldExposure(25000000, kMinNs, kMaxNs) == 31250000);
    policy.setRecordingFps(60);
    assert(policy.heldExposure(31250000, kMinNs, kMaxNs) == 12500000);
    policy.setPhotoFloor(10);
    policy.setVideoMode(true, 30);
    assert(policy.heldExposure(12500000, kMinNs, kMaxNs) == 12500000);
    policy.setRecordingFps(0);
    assert(policy.heldExposure(12500000, kMinNs, kMaxNs) == 25000000);
    policy.setVideoMode(false, 30);
    assert(!policy.shutterAngle());
    assert(policy.previewFloor() == 10);
    assert(policy.heldExposure(800000000, kMinNs, kMaxNs) == 800000000);
}

void angleLegality() {
    CameraCadencePolicy policy;
    policy.setVideoMode(true, 30);
    policy.bindShutter(16000000, kMinNs, kMaxNs);
    assert(policy.shutterAngle() == 180);
    assert(!policy.selectShutterAngle(std::numeric_limits<double>::quiet_NaN(), kMinNs, kMaxNs));
    assert(!policy.selectShutterAngle(std::numeric_limits<double>::infinity(), kMinNs, kMaxNs));
    assert(!policy.selectShutterAngle(-1, kMinNs, kMaxNs));
    assert(!policy.selectShutterAngle(361, kMinNs, kMaxNs));
    assert(!policy.selectShutterAngle(100, kMinNs, kMaxNs));
    assert(policy.shutterAngle() == 180);  // rejected input keeps accepted intent
    const auto bounded = policy.shutterChoices(10000000, 20000000);
    assert(bounded.size() == 3 && bounded[0].exposureTimeNs == 10000000 && bounded[1].degrees == 135 && bounded[2].degrees == 180);
    assert(!policy.selectShutterAngle(360, 10000000, 20000000));
    assert(!policy.selectShutterAngle(45, 10000000, 20000000));
    assert(policy.shutterChoices(40000000, kMaxNs).empty());  // minimum exceeds frame period
    policy.bindShutter(40000000, 40000000, kMaxNs);
    assert(!policy.shutterAngle());
    assert(policy.heldExposure(40000000, 40000000, kMaxNs) == 40000000);
    assert(policy.shutterChoices(0, kMaxNs).empty());
    assert(policy.shutterChoices(20, 10).empty());
    policy.setVideoMode(true, 0);
    assert(policy.videoFps() == 30);
    policy.bindShutter(1000000000, kMinNs, kMaxNs);
    assert(policy.shutterAngle() == 360);
    policy.clearShutterAngle();
    assert(policy.heldExposure(1000000000, kMinNs, kMaxNs) == 33333333);
    assert(policy.selectShutterAngle(180, kMinNs, kMaxNs));
    policy.setVideoMode(true, 60);
    // A held angle outside a new sensor range moves to a legal native choice.
    assert(policy.resolveHeldExposure(16666666, 10000000, kMaxNs) == 10000000);
    assert(policy.shutterAngle() == 216);
}

void fastAngles() {
    CameraCadencePolicy policy;
    policy.setVideoMode(true, 30);
    for (double angle : {22.5, 11.25, 5.625, 2.8125, 1.40625}) {
        assert(policy.selectShutterAngle(angle, kMinNs, kMaxNs));
        assert(policy.heldExposure(0, kMinNs, kMaxNs) ==
               static_cast<int64_t>((1000000000.0 / 30) * (angle / 360)));
    }
    const auto fastest = policy.shutterChoices(123457, kMaxNs).front();
    assert(policy.selectShutterAngle(fastest.degrees, 123457, kMaxNs));
    assert(policy.heldExposure(0, 123457, kMaxNs) == 123457);
    policy.setVideoMode(true, 60);
    assert(policy.resolveHeldExposure(123457, 123457, kMaxNs) == 123457);
    const auto duplicate = policy.shutterChoices(520833, kMaxNs);
    assert(duplicate[0].exposureTimeNs == 520833);
    assert(duplicate[1].exposureTimeNs > 520833);
}

void tapFocus() {
    TapFocusPolicy policy;
    assert(!policy.shouldClear(100000));
    policy.acceptedTap(true, 100);
    assert(!policy.shouldClear(4099));
    assert(policy.shouldClear(4100));
    assert(!policy.shouldClear(4200));
    policy.acceptedTap(true, 5000);
    policy.acceptedTap(true, 8000);  // retap replaces deadline
    assert(!policy.shouldClear(9000));
    assert(!policy.shouldClear(11999));
    assert(policy.shouldClear(12000));
    policy.acceptedTap(true, 13000);
    policy.acceptedTap(false, 14000);  // locked AF does not auto-clear
    assert(!policy.shouldClear(100000));
    policy.acceptedTap(true, 100000);
    policy.clear();  // explicit clear / mode change / session retirement
    assert(!policy.shouldClear(104000));
}

CameraControlState snapshot() {
    CameraCadencePolicy policy;
    policy.setVideoMode(true, 24);
    policy.selectShutterAngle(270, kMinNs, kMaxNs);
    CameraControlState state;
    state.capabilities.cameraId = "0";
    state.capabilities.lensId = "main";
    state.videoMode = policy.videoMode();
    state.videoPreviewFps = policy.videoFps();
    state.tapAfActive = true;
    state.focusRequestId = 42;
    state.spotAeActive = true;
    state.requestedShutterAngleDegrees = policy.shutterAngle();
    state.shutterAngleChoices = policy.shutterChoices(kMinNs, kMaxNs);
    state.requestedExposureTimeNs = policy.heldExposure(10000000, kMinNs, kMaxNs);
    return state;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--snapshot") {
        std::cout << serializeCameraControlState(snapshot());
        return 0;
    }
    angleIntent();
    angleLegality();
    fastAngles();
    tapFocus();
    std::cout << "CAMERA_INTERACTION_POLICY_PASS\n";
}
