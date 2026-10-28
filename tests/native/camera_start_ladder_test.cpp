#include <cassert>
#include <iostream>

#include "camera/CameraStartLadder.h"

using Ladder = rawrcam::camera::CameraStartLadder;
using Plan = Ladder::Plan;
using Outcome = Ladder::Outcome;

namespace {

void deviceErrorsStepToRawOnlyThenGiveUp() {
    Ladder l;
    assert(l.begin("16", 1, 0, 0, true) == Plan::AsConfigured);
    assert(l.deviceFailed(1) == Outcome::Retry);
    assert(l.deviceFailed(1) == Outcome::None);  // One failure per start.
    assert(l.begin("16", 2, 0, 0, true) == Plan::AsConfigured);
    assert(l.deviceFailed(2) == Outcome::RetryRawOnly);
    assert(l.begin("16", 3, 0, 0, true) == Plan::RawOnly);
    assert(l.deviceFailed(3) == Outcome::GaveUp);
    assert(l.begin("16", 4, 0, 0, true) == Plan::GaveUp);
    // A user action reverts to the configured session, not the demoted one.
    l.resetGivenUp();
    assert(l.begin("16", 5, 0, 0, true) == Plan::AsConfigured);
}

void firstFrameProvesTheLens() {
    Ladder l;
    l.begin("1x", 1, 0, 0, true);
    l.firstFrame(1);
    assert(l.deviceFailed(1) == Outcome::None);
    assert(l.begin("1x", 2, 0, 0, true) == Plan::AsConfigured);
    assert(l.deviceFailed(2) == Outcome::None);
    assert(l.tick(2, 1'000'000) == Outcome::None);
}

void rawOnlyWorksAndSticks() {
    Ladder l;
    l.begin("2x", 1, 0, 0, true);
    l.deviceFailed(1);
    l.begin("2x", 2, 0, 0, true);
    assert(l.tick(2, Ladder::kMinFirstFrameMs) == Outcome::RetryRawOnly);
    assert(l.begin("2x", 3, 0, 0, true) == Plan::RawOnly);
    l.firstFrame(3);
    assert(l.begin("2x", 4, 0, 0, true) == Plan::RawOnly);
    assert(l.rawOnly("2x"));
}

void lensesAreIndependent() {
    Ladder l;
    l.begin("16", 1, 0, 0, true);
    l.deviceFailed(1);
    l.begin("16", 2, 0, 0, true);
    l.deviceFailed(2);
    assert(l.rawOnly("16"));
    // Same camera, other lens: untouched.
    assert(!l.rawOnly("1x"));
    assert(l.begin("1x", 3, 0, 0, true) == Plan::AsConfigured);
}

void ineligibleStartsAreLeftAlone() {
    Ladder l;
    assert(l.begin("F35", 1, 0, 0, false) == Plan::AsConfigured);
    assert(!l.watching(1));
    assert(l.deviceFailed(1) == Outcome::None);
    assert(l.tick(1, 1'000'000) == Outcome::None);
}

void deadlineScalesWithExposure() {
    assert(Ladder::firstFrameDeadlineMs(0) == Ladder::kMinFirstFrameMs);
    assert(Ladder::firstFrameDeadlineMs(10'000'000) == Ladder::kMinFirstFrameMs);
    assert(Ladder::firstFrameDeadlineMs(8'000'000'000LL) == 9000);
    Ladder l;
    l.begin("night", 1, 100, 8'000'000'000LL, true);
    assert(l.tick(1, 100 + 8999) == Outcome::None);
    assert(l.tick(1, 100 + 9000) == Outcome::Retry);
}

void failedCapturesCount() {
    Ladder l;
    l.begin("2x", 1, 0, 0, true);
    for (uint32_t i = 1; i < Ladder::kFailedCapturesToCount; ++i) assert(l.captureFailed(1) == Outcome::None);
    assert(l.captureFailed(1) == Outcome::Retry);
}

void staleGenerationsAreIgnored() {
    Ladder l;
    l.begin("16", 1, 0, 0, true);
    l.begin("16", 2, 0, 0, true);
    assert(l.deviceFailed(1) == Outcome::None);
    assert(l.tick(1, 1'000'000) == Outcome::None);
    l.firstFrame(1);
    assert(l.watching(2));
}

void configureRejectionGoesStraightToRawOnly() {
    Ladder l;
    l.configureRejected("16");
    assert(l.begin("16", 1, 0, 0, false) == Plan::RawOnly);
    assert(l.watching(1));
    assert(l.deviceFailed(1) == Outcome::GaveUp);
}

}  // namespace

int main() {
    deviceErrorsStepToRawOnlyThenGiveUp();
    firstFrameProvesTheLens();
    rawOnlyWorksAndSticks();
    lensesAreIndependent();
    ineligibleStartsAreLeftAlone();
    deadlineScalesWithExposure();
    failedCapturesCount();
    staleGenerationsAreIgnored();
    configureRejectionGoesStraightToRawOnly();
    std::cout << "camera_start_ladder_test passed\n";
    return 0;
}
