#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "color/AutoKeyExposure.h"

namespace rawrcam::color::rawr_ae {

// rawr auto exposure: the app owns sensor exposure (Camera2 AE OFF) and
// solves shutter + ISO itself, on any device with MANUAL_SENSOR.
//
// Exposures are linear products shutter(ns) x ISO in the *request*
// coordinate (a vendor sensor mode may report a different ISO scale; callers
// convert). logE = log2 of that product.
//
// Loop (per delivered frame):
//   scene  = metered level - log2(realized exposure of that frame)
// The scene estimate does not move when our own exposure changes, so
// pipeline latency (3-4 frames until a request lands) cannot cause
// overshoot or oscillation. The desired exposure is a function of the
// scene; a critically damped filter eases toward it, and the viewfinder
// applies target / realized of each frame, so sensor steps (latency,
// integer ISO, flicker snapping) never show.

enum class Mode : int { Auto = 0, ShutterPriority = 1, IsoPriority = 2 };
// Persisted/JNI ids.
enum class NightLook : int { Off = 0, Natural = 1, Bright = 2 };

inline NightLook nightLookFromId(int id) noexcept {
    switch (id) {
        case 0: return NightLook::Off;
        case 2: return NightLook::Bright;
        default: return NightLook::Natural;
    }
}

struct Limits {
    std::int32_t isoMin = 100;
    std::int32_t isoMax = 3200;
    // Above SENSOR_MAX_ANALOG_SENSITIVITY the HAL multiplies integer RAW
    // (clips, coarser steps, device-dependent): never requested; the
    // shortfall becomes digital gain the pipeline applies in float.
    std::int32_t isoAnalogMax = 3200;
    std::int64_t shutterMinNs = 100'000;
    std::int64_t shutterMaxNs = 66'666'667;
    // Auto: shutter takes the light first up to this cap, which then moves
    // geometrically to shutterMaxNs over kCapRampEv of extra demand.
    std::int64_t shutterStartNs = 33'333'333;
    float maxDigitalGainEv = 2.0f;
};

struct SolveRequest {
    double exposure = 0.0;  // desired shutter(ns) x ISO, request coordinate
    Mode mode = Mode::Auto;
    // Auto only: > 0 favours a faster shutter (more ISO); < 0 lower ISO
    // (the shutter runs that much slower before ISO rises, up to the max).
    float balanceEv = 0.0f;
    std::int32_t lockedIso = 0;          // IsoPriority
    std::int64_t lockedShutterNs = 0;    // ShutterPriority
    // Mains flicker period (10 ms at 50 Hz, 8.33 ms at 60 Hz); 0 = none.
    // Auto/I shutters at or above one period snap to whole periods.
    std::int64_t flickerPeriodNs = 0;
    Limits limits{};
};

struct Solution {
    std::int64_t shutterNs = 0;
    std::int32_t iso = 0;
    float digitalGainEv = 0.0f;  // >= 0, applied after RAW in float
    bool underexposed = false;   // demand left after every limit
    bool overexposed = false;
    // log2(shutter x ISO x digital): what the frame will actually get.
    [[nodiscard]] double logExposure() const noexcept {
        return std::log2(double(shutterNs) * double(iso)) + double(digitalGainEv);
    }
};

inline constexpr double kCapRampEv = 4.0;

inline std::int64_t snapToFlicker(double shutterNs, std::int64_t periodNs, std::int64_t minNs,
                                  std::int64_t maxNs) noexcept {
    if (periodNs <= 0 || shutterNs < double(periodNs)) return std::clamp<std::int64_t>(std::llround(shutterNs), minNs, maxNs);
    // Round down: ISO picks up the remainder (never a longer, blurrier frame).
    const auto periods = std::max<std::int64_t>(1, std::int64_t(shutterNs / double(periodNs)));
    std::int64_t snapped = periods * periodNs;
    while (snapped > maxNs && snapped > periodNs) snapped -= periodNs;
    return std::clamp(snapped, minNs, maxNs);
}

// Distributes `exposure` over shutter, analog ISO and digital gain.
inline Solution solve(const SolveRequest& r) noexcept {
    const Limits& l = r.limits;
    const double isoMin = std::max(1, l.isoMin);
    const double isoAnalog = std::clamp<double>(std::min(l.isoAnalogMax, l.isoMax), isoMin, std::max<double>(isoMin, l.isoMax));
    const auto tMin = std::max<std::int64_t>(1, l.shutterMinNs);
    const auto tMax = std::max(tMin, l.shutterMaxNs);
    const double e = std::max(1.0, r.exposure);
    double shutter = 0.0;
    switch (r.mode) {
        case Mode::ShutterPriority:
            shutter = double(std::clamp(r.lockedShutterNs > 0 ? r.lockedShutterNs : l.shutterStartNs, tMin, tMax));
            break;
        case Mode::IsoPriority: {
            // The user's ISO sets the shutter; past analog it is digital gain.
            const double iso = std::clamp<double>(r.lockedIso > 0 ? r.lockedIso : isoMin, isoMin,
                                                  std::max<double>(isoMin, l.isoMax));
            shutter = double(snapToFlicker(e / iso, r.flickerPeriodNs, tMin, tMax));
            break;
        }
        case Mode::Auto: {
            const double lowIsoEv = std::max(0.0, -double(r.balanceEv));
            const auto start = std::clamp<std::int64_t>(std::llround(double(l.shutterStartNs) * std::exp2(lowIsoEv)), tMin, tMax);
            double cap = double(start);
            const double atStart = double(start) * isoMin;
            if (e > atStart && tMax > start) {
                const double ramp = std::clamp(std::log2(e / atStart) / kCapRampEv, 0.0, 1.0);
                cap = double(start) * std::pow(double(tMax) / double(start), ramp);
            }
            shutter = std::clamp(e / isoMin, double(tMin), cap);
            // A faster balance trades within the cap; the cap stays the handheld limit.
            shutter = std::clamp(shutter * std::exp2(-std::max(0.0, double(r.balanceEv))), double(tMin), cap);
            // Never longer than the light needs at minimum ISO.
            shutter = std::min(shutter, std::max(double(tMin), e / isoMin));
            shutter = double(snapToFlicker(shutter, r.flickerPeriodNs, tMin, std::int64_t(cap)));
            break;
        }
    }
    Solution s;
    s.shutterNs = std::clamp<std::int64_t>(std::llround(shutter), tMin, tMax);
    double iso = r.mode == Mode::IsoPriority ? std::min<double>(r.lockedIso > 0 ? r.lockedIso : isoMin, isoAnalog)
                                             : e / double(s.shutterNs);
    if (r.mode != Mode::IsoPriority && iso < isoMin && r.mode == Mode::Auto) {
        // Bright: shorten the shutter before ISO runs out (flicker allows it:
        // shutters this short are below one period anyway).
        s.shutterNs = std::clamp<std::int64_t>(std::llround(e / isoMin), tMin, tMax);
        iso = e / double(s.shutterNs);
    }
    s.iso = std::int32_t(std::lround(std::clamp(iso, isoMin, isoAnalog)));
    const double got = double(s.shutterNs) * double(s.iso);
    const double shortEv = std::log2(e / got);
    s.digitalGainEv = float(std::clamp(shortEv, 0.0, double(std::max(0.0f, l.maxDigitalGainEv))));
    s.underexposed = shortEv - double(s.digitalGainEv) > 1.0 / 16.0;
    s.overexposed = shortEv < -1.0 / 16.0;
    return s;
}

// ---------------------------------------------------------------------------
// Metering

struct MeterPolicy {
    // Trimmed median scene key is placed here (scene-linear luma, white = 1).
    // Similar to where Camera2 AE puts it; AutoKey may lift further.
    float targetKey = 0.10f;
    // Highlights: this percentile is kept channelMarginEv under the clip
    // (luma under-reports single-channel clipping, e.g. blue sky).
    float highlightPercentile = 0.99f;
    float channelMarginEv = 0.6f;
    // The highlight guard may darken the key at most this far.
    float maxGuardEv = 1.5f;
    // A clipped percentile does not say by how much; assume this far over.
    float clippedOverEv = 0.5f;
    // Night: below dayEv100 the key target lowers smoothly, reaching
    // nightKeyEv at nightEv100 (Natural; Bright uses half).
    float dayEv100 = 7.0f;
    float nightEv100 = 0.0f;
    float nightKeyEv = -1.7f;
    // Key (relative to white) a metered EV100 scene puts at mid gray.
    float midGray = 0.18f;
};

// EV100 of the scene from one frame: aperture N, shutter t (s) and ISO,
// corrected by where the frame's key sits relative to mid gray.
inline float sceneEv100(float keyEv, double shutterNs, double iso, float aperture, float midGray) noexcept {
    const double n = aperture > 0.5f ? aperture : 1.8;
    const double t = std::max(1e-9, shutterNs * 1e-9);
    return float(std::log2(n * n / t) - std::log2(std::max(1.0, iso) / 100.0) + (double(keyEv) - std::log2(midGray)));
}

// Smooth key offset (<= 0 EV) for dim scenes.
inline float nightKeyOffsetEv(float ev100, NightLook look, const MeterPolicy& p = {}) noexcept {
    if (look == NightLook::Off || !std::isfinite(ev100) || !(p.dayEv100 > p.nightEv100)) return 0.0f;
    const float x = std::clamp((p.dayEv100 - ev100) / (p.dayEv100 - p.nightEv100), 0.0f, 1.0f);
    const float s = x * x * (3.0f - 2.0f * x);  // smoothstep: no kink at either end
    return s * p.nightKeyEv * (look == NightLook::Bright ? 0.5f : 1.0f);
}

struct FrameObservation {
    const std::uint32_t* weightedBins = nullptr;  // AutoKey layout (kSceneKeyBins)
    float clipLumaEv = 0.0f;                       // log2 of the clip luma
    double logExposure = 0.0;                      // realized log2(shutter x request ISO)
    double shutterNs = 0.0;
    double iso = 0.0;                              // request coordinate
    float aperture = 0.0f;
    std::uint64_t timestampNs = 0;
};

struct Metered {
    double desiredLogExposure = 0.0;
    float sceneEv100 = 0.0f;
    float nightOffsetEv = 0.0f;
    float guardEv = 0.0f;  // <= 0
};

// Exposure the scene wants, independent of the frame's own exposure.
inline std::optional<Metered> meter(const FrameObservation& f, NightLook look, float userEv,
                                    const MeterPolicy& p = {}) noexcept {
    if (!f.weightedBins || !std::isfinite(f.logExposure) || !std::isfinite(f.clipLumaEv)) return std::nullopt;
    const auto key = sceneKeyEv(f.weightedBins);
    const auto high = scenePercentileEv(f.weightedBins, p.highlightPercentile);
    if (!key || !high) return std::nullopt;
    Metered m;
    m.sceneEv100 = sceneEv100(*key, f.shutterNs, f.iso, f.aperture, p.midGray);
    m.nightOffsetEv = nightKeyOffsetEv(m.sceneEv100, look, p);
    // Scene-referred (exposure removed) key and highlight.
    const double keyScene = double(*key) - f.logExposure;
    const bool clipped = *high >= f.clipLumaEv - 0.05f;
    const double highScene = (clipped ? double(f.clipLumaEv + p.clippedOverEv) : double(*high)) - f.logExposure;
    const double keyTarget = std::log2(double(p.targetKey)) + double(m.nightOffsetEv) + double(userEv);
    const double byKey = keyTarget - keyScene;
    const double byHighlight = double(f.clipLumaEv - p.channelMarginEv) - highScene;
    const double guarded = std::max(std::min(byKey, byHighlight), byKey - double(p.maxGuardEv));
    m.guardEv = float(guarded - byKey);
    m.desiredLogExposure = guarded;
    return m;
}

// ---------------------------------------------------------------------------
// Smoothing

struct FilterPolicy {
    // Measurement noise: short EMA before anything moves.
    float measureTauSec = 0.12f;
    // Hysteresis: start moving past startEv, stop inside stopEv. Small
    // pans and noise never move the exposure.
    float startEv = 0.15f;
    float stopEv = 0.03f;
    // Critically damped approach (eases in and out). Darkening runs faster
    // (protects highlights); a large error is a scene change.
    float tauSec = 0.35f;
    float darkenTauSec = 0.25f;
    float sceneChangeEv = 1.5f;
    float sceneChangeTauSec = 0.12f;
};

// Adaptation speed setting (0 slow, 1 normal, 2 fast): scales the time
// constants; hysteresis and the scene-change threshold stay.
inline FilterPolicy filterPolicyForSpeed(int speed) noexcept {
    FilterPolicy p;
    const float k = speed <= 0 ? 1.6f : speed >= 2 ? 0.6f : 1.0f;
    p.measureTauSec *= k;
    p.tauSec *= k;
    p.darkenTauSec *= k;
    p.sceneChangeTauSec *= k;
    return p;
}

class ExposureFilter {
   public:
    // desired: metered log exposure (already limited to what the sensor can
    // reach). Returns the smoothed target for this timestamp.
    double update(double desired, std::uint64_t timestampNs, const FilterPolicy& p = {}) noexcept {
        if (!std::isfinite(desired)) return x_;
        if (!primed_) {
            primed_ = true;
            x_ = measured_ = goal_ = desired;
            v_ = 0.0;
            lastNs_ = timestampNs;
            return x_;
        }
        if (timestampNs <= lastNs_) return x_;
        const double dt = std::min(double(timestampNs - lastNs_) * 1e-9, 0.2);
        lastNs_ = timestampNs;
        measured_ += (desired - measured_) * (1.0 - std::exp(-dt / p.measureTauSec));
        // A big jump bypasses the measurement EMA (scene change, not noise).
        if (std::abs(desired - measured_) > p.sceneChangeEv) measured_ = desired;
        const double err = measured_ - x_;
        if (!moving_ && std::abs(err) > p.startEv) moving_ = true;
        if (moving_) goal_ = measured_;
        if (moving_ && std::abs(err) < p.stopEv && std::abs(v_) < 0.05) moving_ = false;
        const double gap = goal_ - x_;
        const double tau = std::abs(gap) > p.sceneChangeEv ? p.sceneChangeTauSec
                           : gap < 0.0                     ? p.darkenTauSec
                                                           : p.tauSec;
        // Exact critically damped step (stable for any dt).
        const double w = 1.0 / tau;
        const double decay = std::exp(-w * dt);
        const double y = x_ - goal_;
        const double c = v_ + w * y;
        x_ = goal_ + (y + c * dt) * decay;
        v_ = (c - w * (y + c * dt)) * decay;
        return x_;
    }
    void snap(double value) noexcept {
        x_ = measured_ = goal_ = value;
        v_ = 0.0;
        moving_ = false;
    }
    [[nodiscard]] bool primed() const noexcept { return primed_; }
    [[nodiscard]] double value() const noexcept { return x_; }
    [[nodiscard]] bool settled() const noexcept { return primed_ && !moving_; }
    void reset() noexcept { *this = ExposureFilter{}; }

   private:
    double x_ = 0.0, v_ = 0.0, measured_ = 0.0, goal_ = 0.0;
    std::uint64_t lastNs_ = 0;
    bool primed_ = false, moving_ = false;
};

// ---------------------------------------------------------------------------
// Controller

struct Settings {
    bool enabled = false;
    Mode mode = Mode::Auto;
    NightLook night = NightLook::Natural;
    float balanceEv = 0.0f;
    std::int32_t lockedIso = 0;
    std::int64_t lockedShutterNs = 0;
    // 0: no snapping; else the mains period.
    std::int64_t flickerPeriodNs = 0;
    Limits limits{};
};

struct Step {
    Solution request;           // what to put on the repeating request
    double targetLogExposure;   // smoothed target the viewfinder shows
    Metered metered;
};

// Frame-lane state: meter -> clamp to reachable -> filter -> solve.
// A frame renders before its own histogram is metered, so its viewfinder
// gain is the latest smoothed target over the frame's realized exposure
// (residualEvFor), fixed on first use so a still gets exactly what was shown.
class Controller {
   public:
    std::optional<Step> update(const FrameObservation& f, const Settings& s, float userEv,
                               const MeterPolicy& mp = {}, const FilterPolicy& fp = {}) noexcept {
        const auto m = meter(f, s.night, userEv, mp);
        if (!m) return std::nullopt;
        // Clamp the desire to what the sensor (+ digital cap) can deliver, so
        // the viewfinder never has to show an exposure the photo cannot get.
        const auto reachable = solve(requestFor(m->desiredLogExposure, s)).logExposure();
        const double target = filter_.update(reachable, f.timestampNs, fp);
        Step step{solve(requestFor(target, s)), target, *m};
        // Requests only change by a visible-to-the-sensor amount; the
        // residual gain covers the rest exactly (no stream of tiny
        // re-submissions while the filter's tail settles).
        if (last_ && std::abs(step.request.logExposure() - last_->request.logExposure()) < kRequestDeadbandEv &&
            step.request.underexposed == last_->request.underexposed)
            step.request = last_->request;
        last_ = step;
        return step;
    }
    // Viewfinder/still gain (EV) bringing a frame of realized logExposure
    // to the current target. nullopt before the first metered frame.
    std::optional<float> residualEvFor(std::uint64_t timestampNs, double logExposure) noexcept {
        if (const auto cached = cachedResidualEv(timestampNs)) return cached;
        if (!filter_.primed() || !std::isfinite(logExposure)) return std::nullopt;
        const float residual = std::clamp(float(filter_.value() - logExposure), kMinResidualEv, kMaxResidualEv);
        if (timestampNs != 0) {
            ring_[next_] = {timestampNs, residual};
            next_ = (next_ + 1) % ring_.size();
        }
        return residual;
    }
    [[nodiscard]] std::optional<float> cachedResidualEv(std::uint64_t timestampNs) const noexcept {
        if (timestampNs == 0) return std::nullopt;
        for (const auto& e : ring_)
            if (e.timestampNs == timestampNs) return e.residualEv;
        return std::nullopt;
    }
    [[nodiscard]] const std::optional<Step>& last() const noexcept { return last_; }
    [[nodiscard]] bool primed() const noexcept { return filter_.primed(); }
    void reset() noexcept { *this = Controller{}; }

    static constexpr float kMinResidualEv = -2.0f, kMaxResidualEv = 4.0f;
    static constexpr double kRequestDeadbandEv = 1.0 / 48.0;

   private:
    static SolveRequest requestFor(double logExposure, const Settings& s) noexcept {
        SolveRequest r;
        r.exposure = std::exp2(logExposure);
        r.mode = s.mode;
        r.balanceEv = s.balanceEv;
        r.lockedIso = s.lockedIso;
        r.lockedShutterNs = s.lockedShutterNs;
        r.flickerPeriodNs = s.flickerPeriodNs;
        r.limits = s.limits;
        return r;
    }
    struct Entry {
        std::uint64_t timestampNs = 0;
        float residualEv = 0.0f;
    };
    ExposureFilter filter_;
    std::array<Entry, 256> ring_{};  // ~8 s at 30 fps: covers the ZSL reference
    std::size_t next_ = 0;
    std::optional<Step> last_;
};

}  // namespace rawrcam::color::rawr_ae
