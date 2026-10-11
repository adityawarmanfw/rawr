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
        // key -5 EV, HAL 2x -> rendered -4 EV; Natural target log2(0.075) ~ -3.74.
        const float lift = autoKeyTargetEv(-5.0f, 2.0f, natural);
        assert(near(lift, std::log2(0.075f) + 4.0f, 1e-4f));
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
    // P95 identifies sky while the key stays on the darker subject.
    {
        Bins bins{};
        bins[binFor(-6.0f)] = 900;
        bins[binFor(-0.5f)] = 100;
        assert(near(*scenePercentileEv(bins.data(), 0.95f), -0.5f, kBinEv));
        assert(!scenePercentileEv(nullptr, 0.95f));
        const auto p = autoKeyCurve(*sceneKeyEv(bins.data()), *scenePercentileEv(bins.data(), .95f), 1.0f,
                                    autoKeyPolicy(AutoKeyMode::Natural));
        assert(p.liftEv == 1.0f);
        assert(tonemap::adaptiveToneGain(std::exp2(p.highlightEv), p) == 1.0f);
    }
    {
        const auto p = autoKeyCurve(-5.0f, -1.0f, 2.0f, autoKeyPolicy(AutoKeyMode::Natural));
        // Full lift lands below the key; the key itself gets most of it.
        const float key = std::exp2(p.keyEv), peak = std::exp2(tonemap::adaptiveTonePeakEv(p));
        assert(near(std::log2(tonemap::adaptiveToneGain(peak, p)), p.liftEv, 1e-5f));
        const float keyOut = key * tonemap::adaptiveToneGain(key, p);
        assert(keyOut > key && keyOut < .075f);
        // Baseline HAL gain is accounted for exactly once.
        assert(p.keyEv == -4.0f && p.highlightEv == 0.0f);
        assert(autoKeyCurve(NAN, 0, 1, autoKeyPolicy(AutoKeyMode::Natural)).liftEv == 0);
        assert(autoKeyCurve(-5, 0, INFINITY, autoKeyPolicy(AutoKeyMode::Natural)).liftEv == 0);
    }
    {
        const auto p = autoKeyCurve(-6.0f, 0.0f, 1.0f, autoKeyPolicy(AutoKeyMode::Natural));
        const auto liftAt = [&](float ev) { return std::log2(tonemap::adaptiveToneGain(std::exp2(ev), p)); };
        // Shadows just below the key are lifted more than the key itself.
        assert(near(liftAt(p.keyEv - 1.5f), 1.0f, 1e-5f));
        assert(liftAt(p.keyEv - 2.0f) > liftAt(p.keyEv) && liftAt(p.keyEv) > .85f && liftAt(p.keyEv) < 1.0f);
        assert(liftAt(p.keyEv - 4.0f) > .45f && liftAt(p.keyEv - 6.5f) == 0.0f);
    }
    for (float key : {-12.0f, -6.0f, -4.0f, -2.0f, 0.0f}) {
        for (float high : {-8.0f, -2.0f, 0.0f, 4.0f}) {
            for (auto mode : {AutoKeyMode::Off, AutoKeyMode::Natural, AutoKeyMode::Strong}) {
                auto p = autoKeyCurve(key, high, 1.0f, autoKeyPolicy(mode));
                assert(p.highlightEv >= std::log2(.36f) && p.highlightEv <= 0.0f);
                // Stress an intermediate smoothed state that exceeds the new span.
                for (float lift : {p.liftEv, 3.0f}) {
                    p.liftEv = lift;
                    float previous = 0.0f, previousEv = -24.0f;
                    for (int i = 0; i <= 32000; ++i) {
                        const float ev = -24.0f + float(i) * .001f;
                        const float y = std::exp2(ev);
                        const float gain = tonemap::adaptiveToneGain(y, p);
                        const float out = y * gain;
                        assert(std::isfinite(out) && out >= y && out > previous);
                        assert(gain <= std::exp2(lift) * 1.000001f);
                        if (ev >= p.highlightEv || ev <= p.keyEv - 6.5f) assert(gain == 1.0f);
                        // Log-domain contrast never drops below 5/8.
                        const float outEv = std::log2(out);
                        if (i > 0) assert((outEv - previousEv) / .001f > .62f);
                        previous = out;
                        previousEv = outEv;
                    }
                    assert(tonemap::adaptiveToneGain(0.0f, p) == 1.0f);
                    assert(tonemap::adaptiveToneGain(-.1f, p) == 1.0f);
                    // Smooth highlight join: derivative tends to one from both sides.
                    const float anchor = std::exp2(p.highlightEv), d = anchor * .0001f;
                    const float slope = (anchor - (anchor - d) * tonemap::adaptiveToneGain(anchor - d, p)) / d;
                    assert(std::abs(slope - 1.0f) < .01f);
                }
            }
        }
    }
    return 0;
}
