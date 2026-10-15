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

// Scene of midtones at -4 EV with 1% highlights at highEv; clip at 0 EV.
Bins scene(float highEv) {
    Bins bins{};
    bins[binFor(-4.0f)] = 9900;
    bins[binFor(highEv)] = 100;
    return bins;
}
}  // namespace

int main() {
    // Highlights 2 EV below clip at no headroom: nothing to protect, stays 0.
    {
        HdrHeadroomMeter meter;
        const auto bins = scene(-2.0f);
        const auto r = meter.update(bins.data(), 10000, 0, 0.0f, 0.0f, -3.0f, kSec);
        assert(r && *r == 0.0f);
    }
    // Clipping (2% clipped) steps down by clipStepEv from the applied headroom.
    {
        HdrHeadroomMeter meter;
        const auto bins = scene(-0.1f);
        const auto r = meter.update(bins.data(), 10000, 200, 0.0f, 0.0f, -3.0f, kSec);
        assert(r && near(*r, -1.0f, 1e-6f));
        // Hold: an immediate second clipped frame is ignored while AE settles.
        assert(!meter.update(bins.data(), 10000, 200, 0.0f, -1.0f, -3.0f, kSec + kSec / 10));
        // After the hold, still clipping at -1: steps to -2.
        const auto r2 = meter.update(bins.data(), 10000, 200, 0.0f, -1.0f, -3.0f, 2 * kSec);
        assert(r2 && near(*r2, -2.0f, 1e-6f));
        // Never below the floor.
        const auto r3 = meter.update(bins.data(), 10000, 200, 0.0f, -2.5f, -3.0f, 3 * kSec);
        assert(r3 && near(*r3, -3.0f, 1e-6f));
    }
    // Unclipped: lands the highlight percentile marginEv under clip.
    {
        HdrHeadroomMeter meter;
        // Exposed at -2 EV, highlights now 1.5 EV under clip: can give back 1.2 EV.
        const auto bins = scene(-1.5f);
        const auto r = meter.update(bins.data(), 10000, 0, 0.0f, -2.0f, -3.0f, kSec);
        assert(r && near(*r, -0.8f, kBinEv + 1e-3f));
        // Small corrections inside the deadband are ignored.
        const auto bins2 = scene(-0.5f);
        assert(!meter.update(bins2.data(), 10000, 0, 0.0f, *r, -3.0f, 2 * kSec));
    }
    // Empty statistics change nothing.
    {
        HdrHeadroomMeter meter;
        Bins bins{};
        assert(!meter.update(bins.data(), 0, 0, 0.0f, 0.0f, -3.0f, kSec));
    }
    std::puts("hdr_headroom_meter_test OK");
    return 0;
}
