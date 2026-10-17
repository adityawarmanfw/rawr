#include "color/ExposureNormalizer.h"

#include <cassert>
#include <cmath>
#include <cstdio>

namespace {
using rawrcam::color::ExposureNormalizer;
constexpr std::uint64_t kFrameNs = 33'333'333ull;
bool near(float a, float b, float eps) { return std::abs(a - b) <= eps; }
}  // namespace

int main() {
    // Steady scene under a -2 EV headroom: lift 2 EV.
    {
        ExposureNormalizer n;
        float lift = 0;
        for (int f = 1; f <= 30; ++f) lift = n.update(f * kFrameNs, 1000.0 / 4.0, -2.0f, true);
        assert(near(lift, 2.0f, 1e-3f));
    }
    // The compensation steps from -2 to -3 EV: the reported headroom changes
    // first, the exposure lags 4 frames, then halves. The rendered
    // brightness (exposure x 2^lift) never moves.
    {
        ExposureNormalizer n;
        const double normal = 1000.0;
        std::uint64_t t = 0;
        for (int f = 0; f < 30; ++f) n.update(t += kFrameNs, normal / 4.0, -2.0f, true);
        double maxDeviation = 0;
        for (int f = 0; f < 60; ++f) {
            const double exposure = f < 4 ? normal / 4.0 : normal / 8.0;
            const bool settled = f < 1 || f > 6;  // AE searching while it moves
            const float lift = n.update(t += kFrameNs, exposure, -3.0f, settled);
            maxDeviation = std::max(maxDeviation, std::abs(std::log2(exposure) + lift - std::log2(normal)));
        }
        std::printf("comp step: max rendered deviation %.3f EV\n", maxDeviation);
        assert(maxDeviation < 0.05);
    }
    // Scene gets 2 EV brighter: AE settles 2 EV shorter. The viewfinder first
    // shows the brighter scene, then eases back to normal within ~1 s.
    {
        ExposureNormalizer n;
        std::uint64_t t = 0;
        for (int f = 0; f < 30; ++f) n.update(t += kFrameNs, 250.0, -2.0f, true);
        float lift = 0;
        for (int f = 0; f < 45; ++f) lift = n.update(t += kFrameNs, f < 6 ? 250.0 : 62.5, -2.0f, f >= 9);
        // Normal fell from 1000 to 250: lift returns to 2 EV over the new exposure.
        std::printf("scene change: lift after 1.5 s %.3f EV\n", lift);
        assert(near(lift, 2.0f, 0.1f));
    }
    // Lifts are kept per frame; a repeated timestamp returns the same answer.
    {
        ExposureNormalizer n;
        const float a = n.update(5 * kFrameNs, 250.0, -2.0f, true);
        assert(n.liftFor(5 * kFrameNs) && *n.liftFor(5 * kFrameNs) == a);
        assert(n.update(5 * kFrameNs, 999.0, -1.0f, true) == a);
        assert(!n.liftFor(6 * kFrameNs));
    }
    // AE that never reports settled (flicker) is still tracked after 1.5 s.
    {
        ExposureNormalizer n;
        std::uint64_t t = 0;
        n.update(t += kFrameNs, 250.0, -2.0f, true);
        float lift = 0;
        for (int f = 0; f < 120; ++f) lift = n.update(t += kFrameNs, 62.5, -2.0f, false);
        assert(near(lift, 2.0f, 0.1f));
    }
    std::puts("exposure_normalizer_test OK");
    return 0;
}
