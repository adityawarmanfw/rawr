#include "color/AutoKeyExposure.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>

namespace {
using namespace rawrcam::color;
using Bins = std::array<std::uint32_t, kSceneKeyBins>;
constexpr float kBinEv = (kSceneKeyMaxEv - kSceneKeyMinEv) / float(kSceneKeyBins);
std::size_t binFor(float ev) { return static_cast<std::size_t>((ev - kSceneKeyMinEv) / kBinEv); }
bool near(float a, float b, float eps) { return std::abs(a - b) <= eps; }
}  // namespace

int main() {
    // Empty histogram has no key.
    {
        Bins bins{};
        assert(!sceneKeyEv(bins.data()).has_value());
    }
    // Single populated bin: key lies inside it.
    {
        Bins bins{};
        bins[binFor(-4.0f)] = 1000;
        const auto key = sceneKeyEv(bins.data());
        assert(key && *key >= -4.0f - kBinEv && *key <= -4.0f + kBinEv);
    }
    // Backlit scene: 60% dark subject at -6 EV, 40% clipped sky in the top
    // bin. The median stays on the subject; a mean would be dragged up.
    {
        Bins bins{};
        bins[binFor(-6.0f)] = 600;
        bins[kSceneKeyBins - 1] = 400;
        const auto key = sceneKeyEv(bins.data());
        assert(key && near(*key, -6.0f, 2.0f * kBinEv));
    }
    // Trim: a 1% shadow tail cannot move the key below the body of the scene.
    {
        Bins bins{};
        bins[0] = 10;
        bins[binFor(-3.0f)] = 990;
        const auto key = sceneKeyEv(bins.data());
        assert(key && *key > -3.0f - 2.0f * kBinEv);
    }
    // Policy: lift = log2(target) - (key + log2(halGain)), clamped.
    {
        const auto natural = autoKeyPolicy(AutoKeyMode::Natural);
        const auto strong = autoKeyPolicy(AutoKeyMode::Strong);
        // key -5 EV, HAL 2x -> rendered -4 EV; Natural target log2(0.12) ~ -3.06.
        const float lift = autoKeyTargetEv(-5.0f, 2.0f, natural);
        assert(near(lift, std::log2(0.12f) + 4.0f, 1e-4f));
        // Clamped to maxLift.
        assert(near(autoKeyTargetEv(-12.0f, 1.0f, natural), natural.maxLiftEv, 1e-6f));
        assert(near(autoKeyTargetEv(-12.0f, 1.0f, strong), strong.maxLiftEv, 1e-6f));
        // Never darkens a bright scene.
        assert(autoKeyTargetEv(0.0f, 1.0f, strong) == 0.0f);
        // Strong lifts at least as much as Natural.
        assert(autoKeyTargetEv(-6.0f, 1.0f, strong) >= autoKeyTargetEv(-6.0f, 1.0f, natural));
        // Bad inputs give no lift.
        assert(autoKeyTargetEv(NAN, 1.0f, strong) == 0.0f);
        assert(autoKeyTargetEv(-6.0f, 0.0f, strong) == 0.0f);
    }
    // Off is exactly zero, whatever the scene.
    {
        const auto off = autoKeyPolicy(AutoKeyMode::Off);
        assert(autoKeyTargetEv(-15.0f, 1.0f, off) == 0.0f);
        assert(autoKeyModeFromId(0) == AutoKeyMode::Off);
        assert(autoKeyModeFromId(1) == AutoKeyMode::Natural);
        assert(autoKeyModeFromId(2) == AutoKeyMode::Strong);
        assert(autoKeyModeFromId(99) == AutoKeyMode::Off);
    }
    // Smoother: first sample lands, then exponential approach with tau.
    {
        AutoKeySmoother s;
        assert(s.update(1.0f, 1'000'000'000ull, false) == 1.0f);
        // A 100 ms step toward 2.0 covers 1 - exp(-0.1 / tau) of the gap.
        constexpr std::uint64_t stepNs = 100'000'000ull;
        const float v = s.update(2.0f, 1'000'000'000ull + stepNs, false);
        assert(near(v, 1.0f + (1.0f - std::exp(-0.1f / AutoKeySmoother::kTimeConstantSec)), 1e-3f));
        // Recording is slower over the same interval.
        AutoKeySmoother r;
        r.update(1.0f, 1'000'000'000ull, true);
        assert(r.update(2.0f, 1'000'000'000ull + stepNs, true) < v);
    }
    // Deadband: small target changes are ignored.
    {
        AutoKeySmoother s;
        s.update(1.0f, 0, false);
        assert(s.update(1.05f, 100'000'000ull, false) == 1.0f);
        assert(s.update(0.92f, 200'000'000ull, false) == 1.0f);
        assert(s.update(1.5f, 300'000'000ull, false) > 1.0f);
    }
    // A zero target is approached all the way, not stranded at the deadband.
    {
        AutoKeySmoother s;
        s.update(0.15f, 0, false);
        float v = 0.15f;
        for (std::uint64_t t = 1; t <= 60; ++t) v = s.update(0.0f, t * 33'000'000ull, false);
        assert(v < 0.01f);
    }
    // Long gaps are capped and time going backwards does not move the value.
    {
        AutoKeySmoother s;
        s.update(0.0f, 5'000'000'000ull, false);
        const float capped = s.update(3.0f, 50'000'000'000ull, false);
        assert(capped < 3.0f * (1.0f - std::exp(-AutoKeySmoother::kMaxStepSec / AutoKeySmoother::kTimeConstantSec)) +
                            1e-4f);
        assert(s.update(3.0f, 1'000'000'000ull, false) == capped);
        s.reset();
        assert(s.value() == 0.0f);
    }
    return 0;
}
