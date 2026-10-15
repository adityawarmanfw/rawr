#include "color/HdrHeadroomMeter.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace {
using namespace rawrcam::color;
using Bins = std::array<std::uint32_t, kSceneKeyBins>;
constexpr float kBinEv = (kSceneKeyMaxEv - kSceneKeyMinEv) / float(kSceneKeyBins);
std::size_t binFor(float ev) { return static_cast<std::size_t>((ev - kSceneKeyMinEv) / kBinEv); }
bool near(float a, float b, float eps) { return std::abs(a - b) <= eps; }
constexpr std::uint64_t kSec = 1'000'000'000ull;
constexpr float kFloor = -3.0f;

// Midtones at -4 EV with 1% highlights at highEv; clip luma at 0 EV.
Bins scene(float highEv) {
    Bins bins{};
    bins[binFor(-4.0f)] = 9900;
    bins[binFor(highEv)] = 100;
    return bins;
}

// Highlights at sceneHighEv when exposed at Camera2 AE (headroom 0).
Bins exposedAt(float sceneHighEv, float headroomEv) { return scene(std::min(sceneHighEv + headroomEv, 0.0f)); }
}  // namespace

int main() {
    // Highlights well below clip at no headroom: nothing to protect.
    {
        HdrHeadroomMeter meter;
        const auto bins = scene(-2.0f);
        assert(!meter.update(bins.data(), 10000, 0.0f, 0.0f, kFloor, kSec));
    }
    // Clipped (percentile in the clip bin): steps down at least 1 EV, then
    // waits for AE before stepping again, never below the floor.
    {
        HdrHeadroomMeter meter;
        const auto bins = scene(0.0f);
        const auto r = meter.update(bins.data(), 10000, 0.0f, 0.0f, kFloor, kSec);
        assert(r && *r <= -1.0f);
        assert(!meter.update(bins.data(), 10000, 0.0f, *r, kFloor, kSec + kSec / 10));
        const auto r2 = meter.update(bins.data(), 10000, 0.0f, *r, kFloor, 2 * kSec);
        assert(r2 && *r2 < *r && *r2 >= kFloor);
    }
    // Unclipped but inside the channel margin: lowers just enough.
    {
        HdrHeadroomMeter meter;
        const auto bins = scene(-0.3f);  // room = -0.6 - (-0.3) = -0.3 EV
        const auto r = meter.update(bins.data(), 10000, 0.0f, -1.0f, kFloor, kSec);
        assert(r && near(*r, -1.3f, kBinEv + 1e-3f));
    }
    // A scene whose highlights sit 2.4 EV over clip at Camera2 AE settles and
    // stays put: no limit cycle over 30 s of frames.
    {
        HdrHeadroomMeter meter;
        float applied = 0.0f;
        int changes = 0, lateChanges = 0;
        for (int frame = 0; frame < 900; ++frame) {
            const std::uint64_t t = kSec + std::uint64_t(frame) * kSec / 30;
            const auto bins = exposedAt(2.4f, applied);
            if (const auto r = meter.update(bins.data(), 10000, 0.0f, applied, -4.0f, t)) {
                applied = *r;
                ++changes;
                if (frame > 300) ++lateChanges;
            }
        }
        assert(lateChanges == 0);
        assert(changes <= 4);
        // Percentile ends between the channel margin and the cushion above it.
        assert(applied <= -2.4f - 0.6f + kBinEv && applied >= -2.4f - 0.6f - 0.7f - kBinEv);
    }
    // Room to give back raises slowly (1 s hold) and keeps a cushion.
    {
        HdrHeadroomMeter meter;
        const auto bins = scene(-3.0f);  // room 2.4 EV at headroom -3
        const auto r = meter.update(bins.data(), 10000, 0.0f, -3.0f, kFloor, kSec);
        assert(r && near(*r, -3.0f + 2.4f - 0.3f, kBinEv + 1e-3f));
        assert(!meter.update(bins.data(), 10000, 0.0f, *r, kFloor, kSec + kSec / 2));
    }
    // After clipping at -1 EV, it does not climb back above -1.3 EV for a while.
    {
        HdrHeadroomMeter meter;
        const auto clippedBins = scene(0.0f);
        const auto down = meter.update(clippedBins.data(), 10000, 0.0f, -1.0f, kFloor, kSec);
        assert(down);
        const auto roomy = scene(-3.0f);
        const auto up = meter.update(roomy.data(), 10000, 0.0f, *down, kFloor, 3 * kSec);
        assert(up && *up <= -1.3f + 1e-4f);
    }
    // Empty statistics change nothing.
    {
        HdrHeadroomMeter meter;
        Bins bins{};
        assert(!meter.update(bins.data(), 0, 0.0f, 0.0f, kFloor, kSec));
    }
    std::puts("hdr_headroom_meter_test OK");
    return 0;
}
