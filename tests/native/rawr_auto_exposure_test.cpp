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

void add(Bins& bins, float ev, std::uint32_t weight, float clipEv = kClipEv) {
    // Clipped samples land in the top bin, as in image_scopes.
    const float clamped = ev >= clipEv ? kSceneKeyMaxEv - kBinEv * 0.5f : ev;
    const auto b = std::size_t(std::clamp((clamped - kSceneKeyMinEv) / kBinEv, 0.0f, float(kSceneKeyBins - 1)));
    bins[b] += weight;
}

// Scene: log2 luma of the body (key) and a highlight cluster, in
// scene-referred units (add log exposure to get frame luma).
struct Scene {
    double keyEv;
    double highlightEv;          // 1.5% of the frame
    double highlightSpreadEv{};  // > 0: a gradient up to highlightEv (sky)
    double chromaEv{};           // brightest channel over luma in the highlight
    double area = 0.015;         // highlight share of the frame
};

// Channel bins: log2(max channel / saturation); neutral luma at the clip
// is the channel clip.
constexpr float kChannelClipEv = -0.074f;  // log2(0.95)
constexpr float kLumaToChannelEv = kChannelClipEv - kClipEv;

struct FrameBins {
    Bins luma{}, channel{};
};

FrameBins frameBins(const Scene& s, double logE) {
    FrameBins f;
    const auto both = [&](double ev, std::uint32_t w, double chroma) {
        add(f.luma, float(ev), w);
        add(f.channel, float(ev + kLumaToChannelEv + chroma), w, kChannelClipEv);
    };
    std::uint32_t total = 0;
    for (int i = -8; i <= 8; ++i) {
        both(s.keyEv + logE + i * 0.25, 600 - std::abs(i) * 30, 0.0);
        total += 600 - std::abs(i) * 30;
    }
    const auto high = std::uint32_t(double(total) * s.area / (1.0 - s.area));
    if (s.highlightSpreadEv > 0.0) {
        // The gradient continues down to the body at the same density.
        constexpr int kSteps = 24;
        for (int i = 0; i < kSteps; ++i)
            both(s.highlightEv + logE - s.highlightSpreadEv * (double(i) + 0.5) / kSteps, high / kSteps * 2,
                 s.chromaEv);
    } else {
        both(s.highlightEv + logE, high, s.chromaEv);
    }
    return f;
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
    MeterPolicy policy{};
    bool channel = false;  // feed the per-channel histogram too

    Sim() {
        settings.enabled = true;
        settings.limits = Limits{100, 6400, 1600, 20'000, 66'666'667, 33'333'333, 2.0f};
        settings.headroomEv = 0.0f;  // the headroom tests turn it on
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
            Scene observed = scene;
            observed.keyEv = measuredKey;
            const auto bins = frameBins(observed, logE);
            FrameObservation f{bins.luma.data(), kClipEv, logE, double(sensor.shutterNs), double(sensor.iso), 1.8f, ts};
            if (channel) f.channelBins = bins.channel.data();
            if (const auto step = ae.update(f, settings, userEv, policy)) pending = step->request;
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
    // Metering: the classic average has no highlight guard; matrix darkens
    // for a clipping highlight, highlight-weighted the most.
    {
        std::array<std::uint32_t, rawrcam::color::kSceneKeyBins> bins{};
        const auto binOf = [](float ev) {
            return std::size_t((ev - rawrcam::color::kSceneKeyMinEv) /
                               (rawrcam::color::kSceneKeyMaxEv - rawrcam::color::kSceneKeyMinEv) *
                               float(rawrcam::color::kSceneKeyBins));
        };
        bins[binOf(-3.3f)] = 900;  // mid tones
        bins[rawrcam::color::kSceneKeyBins - 1] = 100;  // 10% clipped highlights
        FrameObservation f{bins.data(), -0.15f, std::log2(33e6 * 100.0), 33e6, 100.0, 1.8f, 1};
        const auto matrix = meter(f, NightLook::Off, 0.0f, meterPolicyFor(0));
        const auto average = meter(f, NightLook::Off, 0.0f, meterPolicyFor(2));
        const auto highlight = meter(f, NightLook::Off, 0.0f, meterPolicyFor(3));
        assert(matrix && average && highlight);
        assert(average->guardEv == 0.0f && matrix->guardEv < -0.5f);
        assert(highlight->desiredLogExposure <= matrix->desiredLogExposure + 1e-6);
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
    // Highlight guard: a highlight that can be saved pulls the exposure down
    // as far as it needs (within maxGuardEv), and it settles (no clip/unclip
    // cycle).
    for (const bool channel : {false, true}) {
        Sim sim;
        sim.channel = channel;
        // Frame highlight 0.5 EV over the clip at the unguarded exposure.
        sim.run({kDayKey, kDayKey + 3.32 + kClipEv + 0.5}, 4.0);
        const auto& last = sim.ae.last();
        assert(last && last->metered.guardEv < -0.6f && last->metered.guardEv >= -1.5f);
        const std::size_t at = sim.preview.size();
        sim.run({kDayKey, kDayKey + 3.32 + kClipEv + 0.5}, 2.0);
        assert(maxAbsDelta(sim.preview, at, sim.preview.size()) < 0.02);
    }
    // A window from a dark room (7 EV over): it stays clipped, so the guard
    // stops at clippedGuardMaxEv and the room keeps nearly its exposure,
    // without first darkening further and backing off. Once the window
    // leaves, a saveable highlight is protected in full again.
    for (const bool channel : {false, true}) {
        Sim sim;
        sim.channel = channel;
        constexpr double kRoom = kDayKey - 5.0;  // EV100 ~5
        sim.settings.night = NightLook::Off;  // the guard alone
        sim.run({kRoom, kRoom + 1.5}, 2.0);
        const double room = sim.preview.back();
        const Scene window{kRoom, kRoom + 3.32 + 7.0, 0.0, 0.0, 0.15};
        const std::size_t enter = sim.preview.size();
        sim.run(window, 4.0);
        assert(near(sim.ae.last()->metered.guardEv, -0.75, 0.01));
        // The cap plus the window lifting the median; the full guard would
        // leave the room ~1.8 EV down.
        assert(sim.preview.back() > room - 1.1);
        for (std::size_t i = enter; i < sim.preview.size(); ++i) assert(sim.preview[i] > sim.preview.back() - 0.1);
        const std::size_t at = sim.preview.size();
        sim.run(window, 2.0);
        assert(maxAbsDelta(sim.preview, at, sim.preview.size()) < 0.02);
        sim.run({kRoom, kRoom + 3.32 + kClipEv + 0.5}, 4.0);
        assert(sim.ae.last()->metered.guardEv < -0.6f);
    }
    // Outdoors (EV100 ~13) a clipped sky needing more than the indoor cap
    // is saved in full, smoothly.
    for (const bool channel : {false, true}) {
        Sim sim;
        sim.channel = channel;
        constexpr double kSun = kDayKey + 3.0;
        sim.run({kSun, kSun + 1.5}, 2.0);
        const std::size_t at = sim.preview.size();
        sim.run({kSun, kSun + 3.32 + kClipEv + 0.8, 1.0}, 5.0);
        const auto& last = sim.ae.last();
        assert(last->metered.sceneEv100 > 11.0f);
        // Luma needs more than the indoor cap (0.75) and gets it.
        assert(last->metered.guardEv < (channel ? -0.6f : -1.0f) && last->metered.guardEv >= -1.5f);
        double low = sim.preview.back();
        for (std::size_t i = at; i < sim.preview.size(); ++i) low = std::min(low, sim.preview[i]);
        assert(sim.preview.back() - low < 0.12);
    }
    // The guard's darkening: all of it up to the cap, easing to the release
    // level by the give-up point, never jumping.
    {
        const MeterPolicy p;
        assert(near(guardDarkeningEv(1.0, p), 1.0, 1e-9) && near(guardDarkeningEv(1.5, p), 1.5, 1e-9));
        assert(near(guardDarkeningEv(4.0, p), 0.5, 1e-9) && near(guardDarkeningEv(9.0, p), 0.5, 1e-9));
        double prev = 0.0;
        for (double need = 0.0; need < 6.0; need += 0.01) {
            const double d = guardDarkeningEv(need, p);
            assert(std::abs(d - prev) < 0.02);
            prev = d;
        }
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
    // A highlight appearing just over the clip (lamp or sky gradient): the
    // viewfinder darkens to protect it without dipping below where it
    // settles by more than a fraction of a stop, even at 6 frames latency.
    {
        for (const bool channel : {false, true})
            for (const int metering : {0, 3})
                for (const double spread : {0.0, 2.0})
                    for (double over = 0.05; over < 1.6; over += 0.15) {
                        Sim sim;
                        sim.channel = channel;
                        sim.policy = meterPolicyFor(metering);
                        sim.latencyFrames = 6;
                        for (int i = 0; i < 2; ++i) sim.inFlight.push_back(sim.sensor);
                        sim.run({kDayKey, kDayKey + 1.5}, 3.0);
                        const std::size_t at = sim.preview.size();
                        sim.run({kDayKey, kDayKey + 3.32 + kClipEv + over, spread}, 5.0);
                        double low = sim.preview.back();
                        for (std::size_t i = at; i < sim.preview.size(); ++i) low = std::min(low, sim.preview[i]);
                        assert(sim.preview.back() - low < 0.12);
                    }
    }
    // Centre-weighted / average: clipped samples count near the clip, not at
    // the top of the histogram (+4 EV), so a lamp does not darken the frame.
    {
        Bins clean{}, lamp{};
        const auto binOf = [](float ev) { return std::size_t((ev - kSceneKeyMinEv) / kBinEv); };
        clean[binOf(-3.3f)] = 1000;
        lamp[binOf(-3.3f)] = 900;
        lamp[kSceneKeyBins - 1] = 100;  // 10% clipped
        const auto at = [](const Bins& b, int metering) {
            FrameObservation f{b.data(), kClipEv, std::log2(33e6 * 100.0), 33e6, 100.0, 1.8f, 1};
            return meter(f, NightLook::Off, 0.0f, meterPolicyFor(metering))->desiredLogExposure;
        };
        for (const int metering : {1, 2}) {
            const double darker = at(clean, metering) - at(lamp, metering);
            assert(darker > 0.5 && darker < 1.4);
        }
    }
    // EV compensation applies on top of the highlight handling: +1 EV is
    // +1 EV in highlight-weighted, and in matrix with the guard active.
    {
        Bins bins{};
        const auto binOf = [](float ev) { return std::size_t((ev - kSceneKeyMinEv) / kBinEv); };
        bins[binOf(-3.3f)] = 900;
        bins[kSceneKeyBins - 1] = 100;
        FrameObservation f{bins.data(), kClipEv, std::log2(33e6 * 100.0), 33e6, 100.0, 1.8f, 1};
        for (const int metering : {0, 3}) {
            const auto base = meter(f, NightLook::Off, 0.0f, meterPolicyFor(metering));
            const auto up = meter(f, NightLook::Off, 1.0f, meterPolicyFor(metering));
            const auto down = meter(f, NightLook::Off, -1.0f, meterPolicyFor(metering));
            assert(base->guardEv < -0.3f);
            assert(near(up->desiredLogExposure - base->desiredLogExposure, 1.0, 1e-6));
            assert(near(base->desiredLogExposure - down->desiredLogExposure, 1.0, 1e-6));
        }
    }
    // Per-channel highlights: a saturated colour clipping one channel is
    // protected though its luma is well under the clip; a neutral highlight
    // just under saturation is left alone (luma needed a wide margin).
    {
        const double logE = std::log2(33e6 * 100.0);
        const auto metered = [&](double highlightFrameEv, double chroma, bool channel) {
            const Scene scene{-3.32 - logE, highlightFrameEv - logE, 0.0, chroma};
            const auto bins = frameBins(scene, logE);
            FrameObservation f{bins.luma.data(), kClipEv, logE, 33e6, 100.0, 1.8f, 1};
            if (channel) f.channelBins = bins.channel.data();
            return *meter(f, NightLook::Off, 0.0f);
        };
        // Red light: luma 0.5 EV under the clip, red channel 0.8 EV over.
        assert(metered(kClipEv - 0.5, 1.3, true).guardEv < -0.4f);
        assert(metered(kClipEv - 0.5, 1.3, false).guardEv > -0.15f);
        // Neutral highlight 0.4 EV under saturation.
        assert(metered(kClipEv - 0.4, 0.0, true).guardEv == 0.0f);
        assert(metered(kClipEv - 0.4, 0.0, false).guardEv < -0.1f);
    }
    // Focus area: its key pulls the frame's by focusShare, in EV (a subject
    // 3 EV under the frame moves the exposure 1.2 EV at 0.4), smoothly in
    // the share; no focus bins or share 0 leave the frame's key.
    {
        Bins frame{}, subject{};
        const auto binOf = [](float ev) { return std::size_t((ev - kSceneKeyMinEv) / kBinEv); };
        for (int i = -4; i <= 4; ++i) frame[binOf(-3.3f + 0.25f * float(i))] = 100;
        subject[binOf(-6.3f)] = 50;
        const auto desired = [&](const std::uint32_t* focus, float share) {
            FrameObservation f{frame.data(), kClipEv, std::log2(33e6 * 100.0), 33e6, 100.0, 1.8f, 1};
            f.focusBins = focus;
            f.focusShare = share;
            return meter(f, NightLook::Off, 0.0f)->desiredLogExposure;
        };
        const double base = desired(nullptr, 0.4f);
        assert(near(desired(subject.data(), 0.0f), base, 1e-9));
        assert(near(desired(subject.data(), 0.4f) - base, 1.2, 0.05));
        assert(near(desired(subject.data(), 0.25f) - base, 0.75, 0.05));
        assert(near(desired(subject.data(), 0.5f) - base, 1.5, 0.05));
        Bins none{};
        assert(near(desired(none.data(), 0.4f), base, 1e-9));
    }
    // Headroom: in daylight the sensor runs 1 EV under the shown exposure and
    // the viewfinder shows the same as without it; a highlight up to ~1 EV
    // over the shown white is kept in the RAW without darkening the scene.
    // In dim light the headroom fades out (noise first).
    {
        const auto settle = [](float headroomEv, double keyEv, double highlightEv) {
            Sim sim;
            sim.settings.headroomEv = headroomEv;
            sim.run({keyEv, highlightEv}, 4.0);
            return sim;
        };
        const auto plain = settle(0.0f, kDayKey, kDayKey + 1.5);
        const auto spare = settle(1.0f, kDayKey, kDayKey + 1.5);
        assert(near(spare.preview.back(), plain.preview.back(), 0.03));
        assert(near(spare.ae.last()->headroomEv, 1.0, 0.01));
        assert(near(plain.applied.back().logExposure() - spare.applied.back().logExposure(), 1.0, 0.05));
        // 0.5 EV over the shown clip: guarded without headroom, not with it.
        const double over = kDayKey + 3.32 + kClipEv + 0.5;
        assert(settle(0.0f, kDayKey, over).ae.last()->metered.guardEv < -0.6f);
        assert(settle(1.0f, kDayKey, over).ae.last()->metered.guardEv > -0.15f);
        // Dim room (EV100 ~3): no headroom.
        const auto dim = settle(1.0f, kDayKey - 7.0, kDayKey - 5.5);
        assert(dim.ae.last()->headroomEv < 0.01f);
        assert(near(headroomForEv100(1.0f, 7.0f), 1.0, 1e-6) && headroomForEv100(1.0f, 4.0f) == 0.0f);
    }
    // Spot: the night look follows the whole frame, not the tapped area.
    // A dark subject in a bright room is exposed as tapped.
    {
        Bins spot{};
        const auto binOf = [](float ev) { return std::size_t((ev - kSceneKeyMinEv) / kBinEv); };
        spot[binOf(-7.0f)] = 1000;
        FrameObservation f{spot.data(), kClipEv, std::log2(10e6 * 100.0), 10e6, 100.0, 1.8f, 1};
        assert(meter(f, NightLook::Natural, 0.0f)->nightOffsetEv < -0.3f);
        f.frameLogMeanEv = -3.3f;
        assert(meter(f, NightLook::Natural, 0.0f)->nightOffsetEv == 0.0f);
    }
    std::puts("rawr_auto_exposure_test ok");
    return 0;
}
