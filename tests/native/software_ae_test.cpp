#include <cassert>
#include <cmath>
#include <deque>
#include <iostream>

#include "camera/CameraControlTypes.h"
#include "camera/SoftwareAe.h"

using rawrcam::camera::CameraControlState;
using rawrcam::camera::ExposureControlMode;
using rawrcam::camera::SoftwareAe;

namespace {

// A scene whose rendered median follows exposure^0.45 (display encoding), clipped to 0..1, seen through a
// camera that applies a command three frames late.
struct Plant {
    double sceneGain;  // scene brightness: product (ns*ISO) that renders the median at 1.0
    int64_t t;
    int32_t s;
    std::deque<SoftwareAe::Command> inFlight;
    float clip = 0.0f;

    Plant(double gain, int64_t t0, int32_t s0) : sceneGain(gain), t(t0), s(s0) {
        for (int i = 0; i < 3; ++i) inFlight.push_back({t0, s0});
    }
    SoftwareAe::Sample frame() {
        const auto applied = inFlight.front();
        inFlight.pop_front();
        t = applied.exposureTimeNs;
        s = applied.sensitivity;
        const double x = static_cast<double>(t) * static_cast<double>(s) / sceneGain;
        SoftwareAe::Sample out;
        out.exposureTimeNs = t;
        out.sensitivity = s;
        out.lumaP50 = static_cast<float>(std::clamp(std::pow(x, 0.45), 0.0, 1.0));
        out.lumaP95 = std::min(1.0f, out.lumaP50 * 1.6f);
        out.clippedFraction = clip;
        return out;
    }
    void command(const SoftwareAe::Command& c) {
        inFlight.back() = c;  // replaces the newest queued frame; older ones are already committed
    }
    void hold() { inFlight.push_back(inFlight.empty() ? SoftwareAe::Command{t, s} : inFlight.back()); }
};

SoftwareAe::Limits limits() {
    SoftwareAe::Limits l;
    l.shutterMinNs = 100'000;
    l.autoShutterMaxNs = 66'000'000;
    l.shutterMaxNs = 32'000'000'000LL;
    l.sensitivityMin = 72;
    l.sensitivityMax = 19200;
    return l;
}

struct RunResult {
    float finalLuma;
    int64_t t;
    int32_t s;
    int changes;
    int lateChanges;  // commands issued in the last 30 frames (hunting)
};

RunResult run(SoftwareAe::FreeAxis axis, double sceneGain, int64_t t0, int32_t s0, int64_t heldT, int32_t heldS,
              float ev, float clip = 0.0f, bool calibrate = false, int frames = 200) {
    SoftwareAe ae;
    ae.reset(t0, s0, calibrate);
    Plant plant(sceneGain, t0, s0);
    plant.clip = clip;
    RunResult r{};
    for (int i = 0; i < frames; ++i) {
        plant.hold();
        const auto sample = plant.frame();
        r.finalLuma = sample.lumaP50;
        const auto cmd = ae.update(axis, heldT, heldS, ev, limits(), sample);
        if (cmd) {
            plant.command(*cmd);
            ++r.changes;
            if (i >= frames - 30) ++r.lateChanges;
        }
    }
    r.t = plant.t;
    r.s = plant.s;
    return r;
}

void converges(SoftwareAe::FreeAxis axis, double gain, int64_t t0, int32_t s0, int64_t heldT, int32_t heldS) {
    const auto r = run(axis, gain, t0, s0, heldT, heldS, 0.0f);
    std::cout << "axis=" << int(axis) << " gain=" << gain << " -> luma=" << r.finalLuma << " t=" << r.t
              << " s=" << r.s << " changes=" << r.changes << " late=" << r.lateChanges << std::endl;
    assert(std::fabs(r.finalLuma - 0.40f) < 0.06f);
    assert(r.lateChanges == 0);  // settled, not hunting
}

void testConverges() {
    // ISO priority (ISO held at 400): shutter follows the scene, starting both too dark and too bright.
    for (double gain : {2e9, 2e10, 2e11}) {
        converges(SoftwareAe::FreeAxis::Shutter, gain, 4'000'000, 400, 4'000'000, 400);
        converges(SoftwareAe::FreeAxis::Shutter, gain, 40'000'000, 400, 40'000'000, 400);
    }
    // Shutter priority (1/250 held): ISO follows the scene.
    for (double gain : {2e10, 1e11, 2e11}) {
        converges(SoftwareAe::FreeAxis::Sensitivity, gain, 4'000'000, 100, 4'000'000, 100);
        converges(SoftwareAe::FreeAxis::Sensitivity, gain, 4'000'000, 3200, 4'000'000, 3200);
    }
}

void testHeldAxisNeverChanges() {
    SoftwareAe ae;
    ae.reset(4'000'000, 400, false);
    Plant plant(2e11, 4'000'000, 400);
    for (int i = 0; i < 80; ++i) {
        plant.hold();
        const auto cmd = ae.update(SoftwareAe::FreeAxis::Shutter, 4'000'000, 400, 0.0f, limits(), plant.frame());
        if (cmd) {
            assert(cmd->sensitivity == 400);
            plant.command(*cmd);
        }
    }
    SoftwareAe ae2;
    ae2.reset(4'000'000, 400, false);
    Plant plant2(2e11, 4'000'000, 400);
    for (int i = 0; i < 80; ++i) {
        plant2.hold();
        const auto cmd = ae2.update(SoftwareAe::FreeAxis::Sensitivity, 4'000'000, 400, 0.0f, limits(), plant2.frame());
        if (cmd) {
            assert(cmd->exposureTimeNs == 4'000'000);
            plant2.command(*cmd);
        }
    }
}

void testEvCompensation() {
    const auto zero = run(SoftwareAe::FreeAxis::Shutter, 2e10, 4'000'000, 400, 4'000'000, 400, 0.0f);
    const auto plus = run(SoftwareAe::FreeAxis::Shutter, 2e10, 4'000'000, 400, 4'000'000, 400, 1.0f);
    const auto minus = run(SoftwareAe::FreeAxis::Shutter, 2e10, 4'000'000, 400, 4'000'000, 400, -1.0f);
    std::cout << "ev: -1 " << minus.finalLuma << " 0 " << zero.finalLuma << " +1 " << plus.finalLuma << "\n";
    assert(plus.finalLuma > zero.finalLuma + 0.05f);
    assert(minus.finalLuma < zero.finalLuma - 0.05f);
}

void testPinnedAxisDoesNotWindUp() {
    // Scene far too dark for ISO 100 within the auto shutter cap: the shutter pins at its limit and stays there.
    const auto r = run(SoftwareAe::FreeAxis::Shutter, 5e12, 4'000'000, 100, 4'000'000, 100, 0.0f);
    std::cout << "pinned: t=" << r.t << " luma=" << r.finalLuma << " late=" << r.lateChanges << std::endl;
    assert(r.t == limits().autoShutterMaxNs);
    assert(r.finalLuma < 0.40f);
    assert(r.lateChanges == 0);
    // And ISO hits its ceiling instead of overrunning it.
    const auto s = run(SoftwareAe::FreeAxis::Sensitivity, 5e15, 4'000'000, 100, 4'000'000, 100, 0.0f);
    assert(s.s == limits().sensitivityMax);
}

void testHighlightGuard() {
    const auto clean = run(SoftwareAe::FreeAxis::Shutter, 2e11, 4'000'000, 400, 4'000'000, 400, 0.0f, 0.0f);
    const auto clipped = run(SoftwareAe::FreeAxis::Shutter, 2e11, 4'000'000, 400, 4'000'000, 400, 0.0f, 0.2f);
    assert(clipped.t < clean.t);
}

void testCalibratesFromAutoEntry() {
    // Entering from Auto on a scene the HAL exposed to a median of ~0.33: that becomes the target, so the loop
    // holds it rather than drifting toward the fixed default.
    SoftwareAe ae;
    const double gain = 2e11;
    const int64_t t0 = static_cast<int64_t>(std::pow(0.33, 1.0 / 0.45) * gain / 400.0);
    ae.reset(t0, 400, true);
    Plant plant(gain, t0, 400);
    plant.hold();
    const auto first = plant.frame();
    (void)ae.update(SoftwareAe::FreeAxis::Shutter, t0, 400, 0.0f, limits(), first);
    assert(std::fabs(ae.target() - first.lumaP50) < 1e-4f);
    // A wildly dark first frame must not be adopted as the goal.
    SoftwareAe dark;
    dark.reset(1'000'000, 100, true);
    SoftwareAe::Sample d;
    d.exposureTimeNs = 1'000'000;
    d.sensitivity = 100;
    d.lumaP50 = 0.02f;
    (void)dark.update(SoftwareAe::FreeAxis::Shutter, 1'000'000, 100, 0.0f, limits(), d);
    assert(dark.target() >= 0.28f);
}

void testWaitsForSettledFrames() {
    SoftwareAe ae;
    ae.reset(4'000'000, 400, false);
    SoftwareAe::Sample stale;
    stale.exposureTimeNs = 8'000'000;  // a frame from before the last command landed
    stale.sensitivity = 400;
    stale.lumaP50 = 0.1f;
    // Unsettled frames are skipped...
    for (int i = 0; i < 12; ++i) assert(!ae.update(SoftwareAe::FreeAxis::Shutter, 0, 400, 0.0f, limits(), stale));
    // ...but not forever: the camera's real exposure is trusted afterwards.
    assert(ae.update(SoftwareAe::FreeAxis::Shutter, 0, 400, 0.0f, limits(), stale).has_value());
}

void testRequestStateTranslation() {
    CameraControlState s;
    s.capabilities.manualExposureSupported = true;
    s.capabilities.softwarePrioritySupported = true;
    s.requestedExposureTimeNs = 4'000'000;
    s.requestedSensitivity = 100;
    s.softSensitivity = 800;
    s.softExposureTimeNs = 9'000'000;

    // Shutter priority: the held shutter stays, the loop's sensitivity is substituted, sent as plain Manual.
    s.exposureMode = ExposureControlMode::ShutterPriority;
    assert(rawrcam::camera::usesSoftwarePriority(s));
    auto r = rawrcam::camera::requestStateFor(s);
    assert(r.exposureMode == ExposureControlMode::Manual);
    assert(r.requestedExposureTimeNs == 4'000'000 && r.requestedSensitivity == 800);

    // ISO priority: the held sensitivity stays, the loop's shutter is substituted.
    s.exposureMode = ExposureControlMode::IsoPriority;
    r = rawrcam::camera::requestStateFor(s);
    assert(r.exposureMode == ExposureControlMode::Manual);
    assert(r.requestedSensitivity == 100 && r.requestedExposureTimeNs == 9'000'000);

    // Not seeded yet: fall back to the requested values rather than writing zeros.
    s.softExposureTimeNs = 0;
    assert(rawrcam::camera::requestStateFor(s).requestedExposureTimeNs == 4'000'000);

    // Hardware priority wins, and Auto/Manual/video/recording are never rewritten.
    auto hw = s;
    hw.capabilities.isoPrioritySupported = true;
    assert(!rawrcam::camera::usesSoftwarePriority(hw));
    assert(rawrcam::camera::requestStateFor(hw).exposureMode == ExposureControlMode::IsoPriority);
    for (auto mode : {ExposureControlMode::Auto, ExposureControlMode::Manual}) {
        auto m = s;
        m.exposureMode = mode;
        assert(!rawrcam::camera::usesSoftwarePriority(m));
        assert(rawrcam::camera::requestStateFor(m).exposureMode == mode);
    }
    auto video = s;
    video.videoMode = true;
    assert(!rawrcam::camera::usesSoftwarePriority(video));
    auto recording = s;
    recording.recordingFps = 30;
    assert(!rawrcam::camera::usesSoftwarePriority(recording));
    // No manual sensor control means no emulation at all.
    auto noManual = s;
    noManual.capabilities.softwarePrioritySupported = false;
    assert(!rawrcam::camera::usesSoftwarePriority(noManual));
}

void testRebaseKeepsTarget() {
    SoftwareAe ae;
    ae.reset(4'000'000, 400, true);
    SoftwareAe::Sample first;
    first.exposureTimeNs = 4'000'000;
    first.sensitivity = 400;
    first.lumaP50 = 0.33f;
    (void)ae.update(SoftwareAe::FreeAxis::Shutter, 4'000'000, 400, 0.0f, limits(), first);
    const float learned = ae.target();
    // The user drags ISO: the loop adopts the new product at once instead of waiting for frames that never match.
    ae.rebase(4'000'000, 1600);
    SoftwareAe::Sample next;
    next.exposureTimeNs = 4'000'000;
    next.sensitivity = 1600;
    next.lumaP50 = 0.9f;
    const auto cmd = ae.update(SoftwareAe::FreeAxis::Shutter, 4'000'000, 1600, 0.0f, limits(), next);
    assert(cmd.has_value() && cmd->exposureTimeNs < 4'000'000);
    assert(std::fabs(ae.target() - learned) < 1e-6f);
}

}  // namespace

int main() {
    testConverges();
    testHeldAxisNeverChanges();
    testEvCompensation();
    testPinnedAxisDoesNotWindUp();
    testHighlightGuard();
    testCalibratesFromAutoEntry();
    testWaitsForSettledFrames();
    testRequestStateTranslation();
    testRebaseKeepsTarget();
    std::cout << "software_ae_test passed\n";
    return 0;
}
