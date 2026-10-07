#include <cassert>
#include <chrono>
#include <iostream>

#include "camera/SensitivityProbeStartup.h"

using rawrcam::camera::SensitivityProbeStartup;
using rawrcam::metadata::FrameMetadataSnapshot;
using namespace std::chrono_literals;

int main() {
    const auto start = SensitivityProbeStartup::Clock::time_point{};
    FrameMetadataSnapshot frame;
    frame.exposureTimeNs = 80000000;
    frame.sensitivity = 6400;
    SensitivityProbeStartup gate;
    for (uint64_t i = 1; i <= 3; ++i) {
        frame.timestampNs = i;
        auto d = gate.observe(frame, true, false, true, start + i * 80ms);
        assert(d.suppressPreview && d.requestProbe == (i == 3));
    }
    // In-flight repeating frames cannot open preview before the probe returns.
    frame.timestampNs = 4;
    assert(gate.observe(frame, true, false, true, start + 320ms).suppressPreview);
    frame.timestampNs = 5;
    frame.optimizedStillRequestId = rawrcam::metadata::kSensitivityProbeRequestId;
    frame.sensitivity = 3840;
    assert(gate.observe(frame, true, true, false, start + 400ms).suppressPreview);
    frame.optimizedStillRequestId.reset();
    frame.sensitivity = 6400;
    // Black/transition results with AE still searching do not count.
    frame.timestampNs = 6;
    assert(gate.observe(frame, true, true, false, start + 480ms).suppressPreview);
    frame.timestampNs = 7;
    assert(gate.observe(frame, true, true, true, start + 560ms).suppressPreview);
    // Exposure changes reset the consecutive settled-frame count.
    frame.timestampNs = 8;
    frame.sensitivity = 3200;
    assert(gate.observe(frame, true, true, true, start + 640ms).suppressPreview);
    for (uint64_t i = 9; i <= 11; ++i) {
        frame.timestampNs = i;
        auto d = gate.observe(frame, true, true, true, start + i * 80ms);
        assert(d.suppressPreview == (i < 11));
        assert(!d.requestProbe);
    }
    assert(gate.finished() && !gate.timedOut());
    frame.timestampNs = 9; // An out-of-order transition frame remains hidden.
    assert(gate.observe(frame, true, true, true, start + 1s).suppressPreview);
    frame.timestampNs = 12;
    frame.optimizedStillRequestId = 7; // Real dark bracket frames still reach capture.
    assert(!gate.observe(frame, true, true, false, start + 1s).suppressPreview);

    gate.reset();
    frame.optimizedStillRequestId.reset();
    assert(!gate.observe(frame, false, false, false, start).suppressPreview); // Video/non-DCG.
    gate.reset();
    assert(!gate.observe(frame, true, true, false, start).suppressPreview); // Manual already calibrated.

    gate.reset();
    frame.timestampNs = 1;
    assert(gate.observe(frame, true, false, false, start).suppressPreview);
    frame.timestampNs = 2;
    assert(!gate.observe(frame, true, false, false, start + 3s).suppressPreview);
    assert(gate.finished() && gate.timedOut());
    frame.timestampNs = 3;
    frame.optimizedStillRequestId = rawrcam::metadata::kSensitivityProbeRequestId;
    assert(gate.observe(frame, true, true, false, start + 4s).suppressPreview); // Late probe never displayed.

    gate.reset();
    frame.optimizedStillRequestId.reset();
    assert(gate.observe(frame, true, false, false, start).suppressPreview);
    gate.submissionFailed();
    assert(!gate.observe(frame, true, false, true, start + 80ms).suppressPreview);
    std::cout << "SENSITIVITY_PROBE_STARTUP_PASS\n";
}
