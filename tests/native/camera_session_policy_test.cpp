#include <cassert>
#include <iostream>

#include "camera/CameraCadencePolicy.h"
#include "camera/CameraSessionPolicy.h"

using Policy = rawrcam::camera::CameraSessionPolicy;
using Action = Policy::Action;

Policy started(int64_t now = 0) {
    Policy policy;
    policy.setForeground(true, now);
    policy.setSurfaceReady(true);
    policy.setConfigReady();
    assert(policy.advance(now, 0) == Action::Start);
    return policy;
}

void startupOrdering() {
    Policy policy;
    policy.setForeground(true, 100);
    policy.setZeroCopy(true, 110);
    assert(policy.advance(200, 0) == Action::None);
    policy.setSurfaceReady(true);
    assert(policy.advance(300, 0) == Action::None);
    policy.setConfigReady();
    assert(policy.advance(400, 0) == Action::Start);
    policy.setZeroCopy(true, 410);
    assert(policy.advance(900, 900) == Action::None);
    policy.setForeground(false, 1000);
    assert(policy.advance(1000, 900) == Action::Stop);
    policy.setForeground(true, 1100);
    assert(policy.advance(1100, 900) == Action::Start);

    // Config arriving before lifecycle/surface cannot create a session.
    Policy early;
    early.setConfigReady();
    early.setSurfaceReady(true);
    assert(early.advance(0, 0) == Action::None);
    early.setForeground(true, 100);
    assert(early.advance(100, 0) == Action::Start);
    early.setSurfaceReady(false);
    assert(early.advance(200, 0) == Action::Stop);
    assert(early.advance(300, 0) == Action::None);
}

void settingsTimeout() {
    Policy policy;
    policy.setSurfaceReady(true);
    policy.setForeground(true, 10);
    policy.setForeground(true, 1000);  // duplicate lifecycle does not extend it
    assert(policy.advance(1509, 0) == Action::None);
    assert(policy.advance(1510, 0) == Action::Start);
    // Timeout does not start an app that has since backgrounded.
    Policy hidden;
    hidden.setForeground(true, 0);
    hidden.setSurfaceReady(true);
    hidden.setForeground(false, 1000);
    assert(hidden.advance(2000, 0) == Action::None);
}

void coalescedRestart() {
    auto policy = started();
    policy.setZeroCopy(false, 10);
    policy.setZeroCopy(true, 100);
    policy.setZeroCopy(false, 200);
    assert(policy.advance(409, 400) == Action::None);
    assert(policy.advance(410, 400) == Action::Restart);
    assert(policy.advance(800, 800) == Action::None);
    policy.setZeroCopy(false, 900);  // repeat does not restart
    assert(policy.advance(1400, 1400) == Action::None);
    policy.setZeroCopy(true, 1500);
    policy.setForeground(false, 1600);
    assert(policy.advance(1600, 1400) == Action::Stop);
    assert(policy.advance(2000, 1400) == Action::None);
    policy.setForeground(true, 2100);
    assert(policy.advance(2100, 1400) == Action::Start);
    assert(policy.advance(2500, 2500) == Action::None);
}

void captureLease() {
    auto policy = started();
    policy.beginStill();
    policy.beginStill();
    policy.setForeground(false, 100);
    policy.setSurfaceReady(false);
    assert(!policy.canRecoverStills());
    assert(policy.advance(100, 0) == Action::None);
    policy.endStill();
    assert(policy.advance(200, 0) == Action::None);
    policy.endStill();
    assert(policy.advance(300, 0) == Action::Stop);
    policy.endStill();  // unmatched completion is harmless
    assert(policy.advance(400, 0) == Action::None);
    policy.setSurfaceReady(true);
    policy.setForeground(true, 500);
    assert(policy.canRecoverStills());
    assert(policy.advance(500, 0) == Action::Start);
    policy.beginStill();
    policy.setZeroCopy(true, 600);
    assert(policy.advance(1000, 0) == Action::None);
    policy.endStill();
    assert(policy.advance(1100, 0) == Action::Restart);
    policy.beginStill();
    policy.forceStopped();  // teardown overrides outstanding leases and timers
    assert(!policy.canRecoverStills());
    assert(policy.advance(100000, 0) == Action::None);
    policy.setForeground(true, 100100);
    assert(policy.advance(100100, 0) == Action::Start);  // replay kept attachment/config facts
}

void frameRecovery() {
    auto policy = started(100);
    assert(policy.advance(8099, 0) == Action::None);
    assert(policy.advance(8100, 0) == Action::None);  // arm recovery debounce
    assert(policy.advance(8499, 0) == Action::None);
    assert(policy.advance(8500, 0) == Action::Restart);
    // Retirement/configure may take seconds. Grace starts after completion.
    policy.activationCompleted(10000);
    assert(policy.advance(17999, 0) == Action::None);
    assert(policy.advance(18000, 17900) == Action::None);
    assert(policy.advance(25899, 17900) == Action::None);
    assert(policy.advance(25900, 17900) == Action::None);
    assert(policy.advance(26300, 17900) == Action::Restart);  // later stalled flow
    policy.activationCompleted(27000);
    assert(policy.advance(35000, 34000) == Action::None);
    assert(policy.advance(42000, 34000) == Action::None);
    policy.setForeground(false, 42100);  // cancel pending recovery
    assert(policy.advance(42100, 34000) == Action::Stop);
    assert(policy.advance(50000, 34000) == Action::None);

    auto recovered = started();
    assert(recovered.advance(8000, 0) == Action::None);
    assert(recovered.advance(8200, 8100) == Action::None);  // flow resumes before bounce
    assert(recovered.advance(8400, 8100) == Action::None);
    assert(recovered.advance(16100, 8100) == Action::None);
    recovered.setZeroCopy(true, 16200);  // config change still requires restart
    assert(recovered.advance(16500, 16400) == Action::Restart);

    auto queued = started();
    queued.setStillLeases(1);  // immediate lease observed by an already queued tick
    queued.setForeground(false, 100);
    assert(queued.advance(100, 0) == Action::None);
    queued.setStillLeases(0);
    assert(queued.advance(200, 0) == Action::Stop);

    auto cancelled = started();
    cancelled.setZeroCopy(true, 100);
    assert(cancelled.advance(500, 0) == Action::Restart);
    cancelled.setForeground(false, 600);  // background received during retirement
    assert(cancelled.advance(600, 0) == Action::Stop);
    assert(cancelled.advance(700, 0) == Action::None);
}

void cadenceTransitions() {
    rawrcam::camera::CameraCadencePolicy policy;
    assert(policy.previewFloor() == 15);
    assert(policy.capExposure(1000000000) == 1000000000);
    policy.setPhotoFloor(12);
    policy.setVideoMode(true, 24);
    assert(policy.previewFloor() == 24);
    assert(policy.capExposure(1000000000) == 1000000000 / 24);
    assert(policy.capExposure(1000000) == 1000000);
    policy.setRecordingFps(30);
    assert(policy.recording());
    assert(policy.capExposure(1000000000) == 1000000000 / 30);
    policy.setPhotoFloor(10);
    policy.setVideoMode(true, 60);
    assert(policy.capExposure(1000000000) == 1000000000 / 30);
    policy.setRecordingFps(0);
    assert(!policy.recording());
    assert(policy.previewFloor() == 60);
    assert(policy.capExposure(1000000000) == 1000000000 / 60);
    policy.setVideoMode(false, 60);
    assert(policy.previewFloor() == 10);
    assert(policy.capExposure(1000000000) == 1000000000);
    policy.setVideoMode(true, 0);
    assert(policy.previewFloor() == 30);
}

void explicitRestartRequest() {
    Policy policy = started(0);
    policy.requestRestart(1000);
    assert(policy.advance(1399, 1000) == Action::None);
    assert(policy.advance(1400, 1000) == Action::Restart);
    // A still owns the session: the restart waits for its lease.
    policy.requestRestart(2000);
    policy.beginStill();
    assert(policy.advance(2500, 2400) == Action::None);
    policy.endStill();
    assert(policy.advance(2600, 2500) == Action::Restart);
    // Ignored when the camera is not running.
    Policy idle;
    idle.requestRestart(0);
    assert(idle.advance(1000, 0) == Action::None);
}

int main() {
    explicitRestartRequest();
    startupOrdering();
    settingsTimeout();
    coalescedRestart();
    captureLease();
    frameRecovery();
    cadenceTransitions();
    std::cout << "CAMERA_SESSION_POLICY_PASS\n";
}
