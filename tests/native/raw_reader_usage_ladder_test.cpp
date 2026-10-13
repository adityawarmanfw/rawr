#include <cassert>
#include <iostream>

#include "imaging/RawReaderUsageLadder.h"

using Ladder = rawrcam::imaging::RawReaderUsageLadder;
constexpr int64_t kDeadline = Ladder::kFirstFrameDeadlineMs;

void silentGpuReaderDemotesOnce() {
    Ladder ladder;
    ladder.readerCreated(1, true, 100);
    assert(!ladder.advance(100 + kDeadline - 1));
    assert(ladder.advance(100 + kDeadline));
    assert(ladder.cpuOnly());
    assert(!ladder.advance(100 + 10 * kDeadline));
    // The CPU-only reader is never armed, even if it is silent too.
    ladder.readerCreated(2, true, 20000);
    assert(!ladder.advance(20000 + 10 * kDeadline));
    assert(ladder.cpuOnly());
}

void deliveringReaderNeverDemotes() {
    Ladder ladder;
    ladder.readerCreated(1, true, 0);
    ladder.rawArrived(1);
    assert(!ladder.advance(10 * kDeadline));
    // A later session that stalls (e.g. HAL hiccup) keeps the proven usage.
    ladder.readerCreated(2, true, 50000);
    assert(!ladder.advance(50000 + 10 * kDeadline));
    assert(!ladder.cpuOnly());
}

void cpuReaderAndRetiredReaderAreIgnored() {
    Ladder ladder;
    ladder.readerCreated(1, false, 0);
    assert(!ladder.advance(10 * kDeadline));
    ladder.readerCreated(2, true, 0);
    ladder.readerDestroyed(2);
    assert(!ladder.advance(10 * kDeadline));
    // A stale generation's frame neither satisfies nor disarms the new reader.
    ladder.readerCreated(3, true, 0);
    ladder.rawArrived(2);
    ladder.readerDestroyed(2);
    assert(ladder.advance(kDeadline));
}

void failedCapturesDemoteEarly() {
    Ladder ladder;
    ladder.readerCreated(1, true, 0);
    for (uint32_t i = 1; i < Ladder::kFailedCapturesToDemote; ++i) ladder.captureFailed(1);
    ladder.captureFailed(7);  // another generation's failures don't count
    assert(!ladder.advance(1));
    ladder.captureFailed(1);
    assert(ladder.advance(1));
    assert(ladder.cpuOnly());
}

void demoteSkipsTheGpuReader() {
    Ladder ladder;
    ladder.demote();
    ladder.readerCreated(1, true, 0);
    assert(ladder.cpuOnly());
    assert(!ladder.advance(10 * kDeadline));
}

int main() {
    failedCapturesDemoteEarly();
    demoteSkipsTheGpuReader();
    silentGpuReaderDemotesOnce();
    deliveringReaderNeverDemotes();
    cpuReaderAndRetiredReaderAreIgnored();
    std::cout << "raw_reader_usage_ladder_test passed\n";
}
