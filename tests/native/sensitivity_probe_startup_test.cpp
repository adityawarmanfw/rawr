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
    // The viewfinder shows from the first frame; the third requests the probe.
    for (uint64_t i = 1; i <= 3; ++i) {
        frame.timestampNs = i;
        auto d = gate.observe(frame, true, false, true, start + i * 33ms);
        assert(!d.suppressPreview && d.requestProbe == (i == 3));
    }
    // Repeating frames before the probe returns stay visible.
    frame.timestampNs = 4;
    assert(!gate.observe(frame, true, false, true, start + 133ms).suppressPreview);
    // The probe frame itself is never shown.
    frame.timestampNs = 5;
    frame.optimizedStillRequestId = rawrcam::metadata::kSensitivityProbeRequestId;
    frame.sensitivity = 3840;
    assert(gate.observe(frame, true, true, false, start + 166ms).suppressPreview);
    frame.optimizedStillRequestId.reset();
    // A frame whose exposure jumped while AE recovers is held back...
    frame.timestampNs = 6;
    frame.sensitivity = 3200;
    assert(gate.observe(frame, true, true, false, start + 200ms).suppressPreview);
    // ...and the first frame back at the pre-probe exposure reveals.
    frame.timestampNs = 7;
    frame.sensitivity = 6400;
    auto d = gate.observe(frame, true, true, true, start + 233ms);
    assert(!d.suppressPreview && !d.requestProbe);
    assert(gate.finished() && !gate.timedOut());
    frame.timestampNs = 6;  // An out-of-order transition frame remains hidden.
    assert(gate.observe(frame, true, true, true, start + 1s).suppressPreview);
    frame.timestampNs = 2;  // Frames from before the probe were shown already.
    assert(!gate.observe(frame, true, true, true, start + 1s).suppressPreview);
    frame.timestampNs = 12;
    frame.optimizedStillRequestId = 7;  // Real bracket frames still reach capture.
    assert(!gate.observe(frame, true, true, false, start + 1s).suppressPreview);

    // Recovery never holds the viewfinder longer than 0.5 s.
    gate.reset();
    frame.optimizedStillRequestId.reset();
    frame.sensitivity = 6400;
    for (uint64_t i = 1; i <= 3; ++i) {
        frame.timestampNs = i;
        gate.observe(frame, true, false, true, start);
    }
    frame.timestampNs = 4;
    frame.optimizedStillRequestId = rawrcam::metadata::kSensitivityProbeRequestId;
    gate.observe(frame, true, true, false, start + 100ms);
    frame.optimizedStillRequestId.reset();
    frame.sensitivity = 1600;
    frame.timestampNs = 5;
    assert(gate.observe(frame, true, true, false, start + 300ms).suppressPreview);
    frame.timestampNs = 6;
    assert(!gate.observe(frame, true, true, false, start + 600ms).suppressPreview);
    assert(gate.finished());

    gate.reset();
    assert(!gate.observe(frame, false, false, false, start).suppressPreview);  // Video/non-DCG.
    gate.reset();
    assert(!gate.observe(frame, true, true, false, start).suppressPreview);  // Already calibrated.

    gate.reset();
    frame.timestampNs = 1;
    assert(!gate.observe(frame, true, false, false, start).suppressPreview);
    frame.timestampNs = 2;
    assert(!gate.observe(frame, true, false, false, start + 3s).suppressPreview);
    assert(gate.finished() && gate.timedOut());
    frame.timestampNs = 3;
    frame.optimizedStillRequestId = rawrcam::metadata::kSensitivityProbeRequestId;
    assert(gate.observe(frame, true, true, false, start + 4s).suppressPreview);  // Late probe never displayed.

    gate.reset();
    frame.optimizedStillRequestId.reset();
    assert(!gate.observe(frame, true, false, false, start).suppressPreview);
    gate.submissionFailed();
    assert(!gate.observe(frame, true, false, true, start + 80ms).suppressPreview);
    std::cout << "SENSITIVITY_PROBE_STARTUP_PASS\n";
}
