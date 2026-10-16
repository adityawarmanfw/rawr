#include "color/RawrAutoExposure.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <random>
#include <vector>

namespace {
using namespace rawrcam::color;
using namespace rawrcam::color::rawr_ae;
using Bins = std::array<std::uint32_t, kSceneKeyBins>;
constexpr float kBinEv = (kSceneKeyMaxEv - kSceneKeyMinEv) / float(kSceneKeyBins);
constexpr float kClipEv = -0.152f;  // log2(0.9)

void add(Bins& bins, float ev, std::uint32_t weight) {
    // Clipped samples land in the top bin, as in image_scopes.
    const float clamped = ev >= kClipEv ? kSceneKeyMaxEv - kBinEv * 0.5f : ev;
    const auto b = std::size_t(std::clamp((clamped - kSceneKeyMinEv) / kBinEv, 0.0f, float(kSceneKeyBins - 1)));
    bins[b] += weight;
}

// Scene: log2 luma of the body (key) and a highlight cluster, in
// scene-referred units (add log exposure to get frame luma).
struct Scene {
    double keyEv;
    double highlightEv;  // 1.5% of the frame
};

Bins frameBins(const Scene& s, double logE) {
    Bins bins{};
    for (int i = -8; i <= 8; ++i) add(bins, float(s.keyEv + logE + i * 0.25), 600 - std::abs(i) * 30);
    std::uint32_t total = 0;
    for (auto w : bins) total += w;
    add(bins, float(s.highlightEv + logE), total * 15 / 985);
    return bins;
}

struct Sim {
    Settings settings;
    Controller ae;
    double fps = 30.0;
    int latencyFrames = 4;    // request -> sensor
    int tickFrames = 3;       // camera lane pushes the repeating request at ~100 ms
    double noiseEv = 0.0;
    std::mt19937 rng{7};
    std::deque<Solution> inFlight;
    Solution sensor{};
    Solution pending{};
    std::uint64_t frame = 0;
    std::vector<double> preview;  // log2 viewfinder brightness of the key
    std::vector<Solution> applied;

    Sim() {
        settings.enabled = true;
        settings.limits = Limits{100, 6400, 1600, 20'000, 66'666'667, 33'333'333, 2.0f};
        sensor = {10'000'000, 100, 0.0f};
        pending = sensor;
        for (int i = 0; i < latencyFrames; ++i) inFlight.push_back(sensor);
    }
    void run(const Scene& scene, double seconds, float userEv = 0.0f) {
        std::normal_distribution<double> noise(0.0, noiseEv);
        const int frames = int(seconds * fps);
        for (int n = 0; n < frames; ++n, ++frame) {
            // Sensor takes the request submitted latencyFrames ago.
            if (frame % std::uint64_t(tickFrames) == 0) inFlight.push_back(pending);
            else inFlight.push_back(inFlight.back());
            sensor = inFlight.front();
            inFlight.pop_front();
            const double logE = std::log2(double(sensor.shutterNs) * double(sensor.iso));
            const std::uint64_t ts = 1'000'000'000ull + frame * std::uint64_t(1e9 / fps);
            // Render first (residual from the current target), then meter.
            const float residual = ae.residualEvFor(ts, logE).value_or(0.0f);
            const double measuredKey = scene.keyEv + (noiseEv > 0 ? noise(rng) : 0.0);
            preview.push_back(scene.keyEv + logE + residual);
            const Scene observed{measuredKey, scene.highlightEv};
            const auto bins = frameBins(observed, logE);
            FrameObservation f{bins.data(), kClipEv, logE, double(sensor.shutterNs), double(sensor.iso), 1.8f, ts};
            if (const auto step = ae.update(f, settings, userEv)) pending = step->request;
            applied.push_back(sensor);
        }
    }
};

double maxAbsDelta(const std::vector<double>& v, std::size_t from, std::size_t to) {
    double m = 0.0;
    for (std::size_t i = from + 1; i < to; ++i) m = std::max(m, std::abs(v[i] - v[i - 1]));
    return m;
}

bool near(double a, double b, double eps) { return std::abs(a - b) <= eps; }

// Scene key in scene units (frame luma = key + log2(shutter ns x ISO)):
// ~0.1 at 5 ms ISO 100, EV100 ~10, so night look stays out of the way.
constexpr double kDayKey = -3.32 - 28.9;
}  // namespace

int main() {
    // Solver: Auto spends shutter first up to the start cap, then ISO.
    {
        SolveRequest r;
        r.limits = Limits{100, 6400, 1600, 20'000, 66'666'667, 33'333'333, 2.0f};
        r.exposure = 10'000'000.0 * 100.0;
        auto s = solve(r);
        assert(s.iso == 100 && near(double(s.shutterNs), 10e6, 1e3) && s.digitalGainEv == 0.0f);
        r.exposure = 33'333'333.0 * 400.0;  // 2 EV past the start cap: cap ramps half way
        s = solve(r);
        assert(s.shutterNs > 33'333'333 && s.shutterNs < 66'666'667);
        assert(near(s.logExposure(), std::log2(r.exposure), 0.01));
    }
    // Solver: analog cap. Gain past isoAnalogMax is digital, capped.
    {
        SolveRequest r;
        r.limits = Limits{100, 6400, 1600, 20'000, 66'666'667, 33'333'333, 2.0f};
        r.exposure = 66'666'667.0 * 1600.0 * 2.0;  // 1 EV past analog at max shutter
        auto s = solve(r);
        assert(s.iso == 1600 && s.shutterNs == 66'666'667);
        assert(near(s.digitalGainEv, 1.0, 0.01) && !s.underexposed);
        r.exposure *= 8.0;  // 4 EV past: 2 EV digital, then underexposed
        s = solve(r);
        assert(near(s.digitalGainEv, 2.0, 1e-4) && s.underexposed);
    }
    // Solver: flicker. Auto/I shutters >= one period are whole periods.
    {
        SolveRequest r;
        r.limits = Limits{100, 6400, 1600, 20'000, 66'666'667, 33'333'333, 2.0f};
        r.flickerPeriodNs = 10'000'000;
        for (double e : {15e6 * 100, 27e6 * 300, 33e6 * 800, 50e6 * 1500}) {
            r.exposure = e;
            const auto s = solve(r);
            assert(s.shutterNs % 10'000'000 == 0);
            assert(near(s.logExposure(), std::log2(e), 0.02));
        }
        r.exposure = 2e6 * 100;  // bright: below one period, unsnapped
        assert(solve(r).shutterNs == 2'000'000);
    }
    // Solver: S holds the shutter, I holds the ISO.
    {
        SolveRequest r;
        r.limits = Limits{100, 6400, 1600, 20'000, 66'666'667, 33'333'333, 2.0f};
        r.mode = Mode::ShutterPriority;
        r.lockedShutterNs = 4'000'000;
        r.exposure = 33e6 * 200;
        auto s = solve(r);
        assert(s.shutterNs == 4'000'000 && s.iso == 1600 && s.digitalGainEv > 0.0f);
        r.mode = Mode::IsoPriority;
        r.lockedIso = 400;
        s = solve(r);
        assert(s.iso == 400 && near(double(s.shutterNs), 33e6 * 200 / 400, 1e3));
    }
    // Balance: faster shortens the shutter, lower ISO lets it run longer first.
    {
        SolveRequest r;
        r.limits = Limits{100, 6400, 1600, 20'000, 66'666'667, 16'666'667, 2.0f};
        r.exposure = 33e6 * 100;
        const auto balanced = solve(r);
        r.balanceEv = 1.0f;
        const auto faster = solve(r);
        r.balanceEv = -1.0f;
        const auto lowIso = solve(r);
        assert(faster.shutterNs < balanced.shutterNs && faster.iso > balanced.iso);
        assert(lowIso.iso < balanced.iso && lowIso.shutterNs > balanced.shutterNs);
        assert(near(lowIso.logExposure(), faster.logExposure(), 0.02));
    }
    // Solver: I above analog keeps analog ISO, the rest is digital, and the
    // shutter is the one the user's ISO implies.
    {
        SolveRequest r;
        r.limits = Limits{100, 6400, 1600, 20'000, 66'666'667, 33'333'333, 2.0f};
        r.mode = Mode::IsoPriority;
        r.lockedIso = 6400;
        r.exposure = 10e6 * 6400;
        const auto s = solve(r);
        assert(s.iso == 1600 && near(double(s.shutterNs), 10e6, 1e3) && near(s.digitalGainEv, 2.0, 1e-3));
        assert(!s.underexposed);
    }
    // Solver: I in a dark scene at the longest shutter adds no digital gain
    // within analog, so the user's ISO choice changes the exposure.
    {
        SolveRequest r;
        r.limits = Limits{50, 1600, 1600, 20'000, 66'666'667, 33'333'333, 2.0f};
        r.mode = Mode::IsoPriority;
        r.exposure = 66'666'667.0 * 800;  // needs ISO 800 at the longest shutter
        r.lockedIso = 50;
        const auto low = solve(r);
        r.lockedIso = 126;
        const auto high = solve(r);
        assert(low.digitalGainEv == 0.0f && high.digitalGainEv == 0.0f && low.underexposed);
        assert(near(high.logExposure() - low.logExposure(), std::log2(126.0 / 50.0), 0.02));
    }
    // Speed presets scale the time constants only.
    {
        const auto slow = filterPolicyForSpeed(0), normal = filterPolicyForSpeed(1), fast = filterPolicyForSpeed(2);
        assert(slow.tauSec > normal.tauSec && normal.tauSec > fast.tauSec);
        assert(slow.startEv == normal.startEv && fast.sceneChangeEv == normal.sceneChangeEv);
    }
    // Night look: darker scenes are placed lower, smoothly; day untouched.
    {
        assert(nightKeyOffsetEv(10.0f, NightLook::Natural) == 0.0f);
        assert(near(nightKeyOffsetEv(-2.0f, NightLook::Natural), -1.7, 1e-4));
        assert(near(nightKeyOffsetEv(-2.0f, NightLook::Bright), -0.85, 1e-4));
        assert(nightKeyOffsetEv(-2.0f, NightLook::Off) == 0.0f);
        float prev = 0.0f;
        for (float ev = 8.0f; ev >= -1.0f; ev -= 0.05f) {
            const float o = nightKeyOffsetEv(ev, NightLook::Natural);
            assert(o <= prev + 1e-6f && prev - o < 0.03f);
            prev = o;
        }
    }
    // Steady scene with metering noise: no hunting once settled, and the
    // viewfinder does not shimmer.
    {
        Sim sim;
        sim.noiseEv = 0.04;
        sim.run({kDayKey, kDayKey + 3.0}, 3.0);
        const std::size_t settled = sim.applied.size();
        sim.run({kDayKey, kDayKey + 3.0}, 5.0);
        int changes = 0;
        for (std::size_t i = settled + 1; i < sim.applied.size(); ++i)
            changes += sim.applied[i].shutterNs != sim.applied[i - 1].shutterNs ||
                       sim.applied[i].iso != sim.applied[i - 1].iso;
        assert(changes <= 2);
        assert(maxAbsDelta(sim.preview, settled, sim.preview.size()) < 0.02);
    }
    // Scene change (light switched up 3 EV): the viewfinder jumps with the
    // scene once, then eases back without overshoot or pumping.
    {
        Sim sim;
        sim.run({kDayKey, kDayKey + 3.0}, 2.0);
        const double before = sim.preview.back();
        const std::size_t at = sim.preview.size();
        sim.run({kDayKey + 3.0, kDayKey + 6.0}, 3.0);
        assert(sim.preview[at] > before + 2.5);  // the change itself shows
        // Converged near the original placement, within a second.
        assert(near(sim.preview[at + 30], before, 0.15));
        assert(near(sim.preview.back(), before, 0.05));
        // Monotonic recovery (no pump): never rises again once falling,
        // never dips below the final level by more than noise.
        for (std::size_t i = at + 2; i < sim.preview.size(); ++i) {
            assert(sim.preview[i] <= sim.preview[i - 1] + 0.01);
            assert(sim.preview[i] >= sim.preview.back() - 0.05);
        }
    }
    // Same down: darker scene recovers upward, monotonic.
    {
        Sim sim;
        sim.run({kDayKey, kDayKey + 3.0}, 2.0);
        const double before = sim.preview.back();
        const std::size_t at = sim.preview.size();
        sim.run({kDayKey - 2.0, kDayKey + 1.0}, 3.0);
        assert(near(sim.preview.back(), before, 0.05));
        for (std::size_t i = at + 2; i < sim.preview.size(); ++i) assert(sim.preview[i] >= sim.preview[i - 1] - 0.01);
    }
    // Slow light change (clouds, sunset): continuous, no stepping.
    {
        Sim sim;
        sim.run({kDayKey, kDayKey + 3.0}, 2.0);
        const std::size_t at = sim.preview.size();
        for (int i = 0; i < 120; ++i) sim.run({kDayKey - i * 0.02, kDayKey + 3.0 - i * 0.02}, 1.0 / 30.0);
        assert(maxAbsDelta(sim.preview, at, sim.preview.size()) < 0.03);
    }
    // Night: a dim scene lands darker than a day scene, ISO stays analog,
    // the remainder is digital, and the viewfinder shows the target.
    {
        Sim day;
        day.run({kDayKey, kDayKey + 3.0}, 3.0);
        Sim night;
        night.run({kDayKey - 8.0, kDayKey - 3.0}, 4.0);
        assert(night.preview.back() < day.preview.back() - 1.0);
        assert(night.applied.back().iso <= 1600);
        const auto& last = night.ae.last();
        assert(last && last->metered.nightOffsetEv < -1.0f);
    }
    // Highlight guard: clipped highlights pull the exposure down, at most
    // maxGuardEv, and it settles (no clip/unclip cycle).
    {
        Sim sim;
        sim.run({kDayKey, kDayKey + 4.5}, 4.0);
        const auto& last = sim.ae.last();
        assert(last && last->metered.guardEv < -0.5f && last->metered.guardEv >= -1.5f);
        const std::size_t at = sim.preview.size();
        sim.run({kDayKey, kDayKey + 4.5}, 2.0);
        assert(maxAbsDelta(sim.preview, at, sim.preview.size()) < 0.02);
    }
    // Flicker snapping in the loop still gives a smooth viewfinder.
    {
        Sim sim;
        sim.settings.flickerPeriodNs = 10'000'000;
        sim.run({kDayKey - 4.0, kDayKey - 1.0}, 2.0);
        const std::size_t at = sim.preview.size();
        for (int i = 0; i < 90; ++i) sim.run({kDayKey - 4.0 - i * 0.02, kDayKey - 1.0 - i * 0.02}, 1.0 / 30.0);
        for (std::size_t i = at; i < sim.applied.size(); ++i)
            if (sim.applied[i].shutterNs >= 10'000'000) assert(sim.applied[i].shutterNs % 10'000'000 == 0);
        assert(maxAbsDelta(sim.preview, at, sim.preview.size()) < 0.03);
    }
    // S in the loop: the shutter never moves, ISO follows the scene smoothly.
    {
        Sim sim;
        sim.settings.mode = Mode::ShutterPriority;
        sim.settings.lockedShutterNs = 8'000'000;
        sim.run({kDayKey - 2.0, kDayKey + 1.0}, 2.0);
        const double before = sim.preview.back();
        const std::size_t at = sim.preview.size();
        sim.run({kDayKey - 3.0, kDayKey}, 3.0);
        for (std::size_t i = at + 4; i < sim.applied.size(); ++i) assert(sim.applied[i].shutterNs == 8'000'000);
        assert(near(sim.preview.back(), before, 0.06));
    }
    // I in the loop: the ISO never moves.
    {
        Sim sim;
        sim.settings.mode = Mode::IsoPriority;
        sim.settings.lockedIso = 400;
        sim.run({kDayKey - 3.0, kDayKey}, 3.0);
        for (std::size_t i = 10; i < sim.applied.size(); ++i) assert(sim.applied[i].iso == 400);
    }
    std::puts("rawr_auto_exposure_test ok");
    return 0;
}
