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
    // I: digital gain only stands in for the user's ISO past analog; a dark
    // scene at the longest shutter stays dark (the user's ISO choice shows).
    double digitalCap = std::max(0.0f, l.maxDigitalGainEv);
    if (r.mode == Mode::IsoPriority)
        digitalCap = std::min(digitalCap, std::max(0.0, std::log2(std::max<double>(r.lockedIso, isoMin) / isoAnalog)));
    s.digitalGainEv = float(std::clamp(shortEv, 0.0, digitalCap));
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
    // Highlights: this percentile of the per-channel histogram (brightest
    // channel over its saturation) is kept channelMarginEv under the clip.
    // Without it, the luma percentile keeps lumaMarginEv (luma under-reports
    // single-channel clipping, e.g. blue sky).
    float highlightPercentile = 0.99f;
    float channelMarginEv = 0.25f;
    float lumaMarginEv = 0.6f;
    // The highlight guard may darken the key at most this far. A highlight
    // needing more cannot be saved without sinking the scene (a window from
    // a dark room): the guard eases off to guardReleasedEv by the time it
    // would need guardGiveUpEv, and the highlight clips.
    float maxGuardEv = 1.5f;
    float guardReleasedEv = 0.5f;
    float guardGiveUpEv = 4.0f;
    // A clipped highlight does not show how far over it is, so indoors it
    // darkens at most clippedGuardMaxEv: one just over then shows unclipped
    // and gets the full guard; a window from a dark room stays clipped and
    // the room keeps its exposure. (Going further to find out, then backing
    // off, would pump.) Outdoors (scene EV100 from outdoorEv100Low to
    // outdoorEv100High) a clipped highlight is usually sky worth saving: the
    // cap eases up to the full maxGuardEv.
    float clippedGuardMaxEv = 0.75f;
    // The sensor runs this far under the shown exposure (the viewfinder and
    // photo gain carry it back; the render rolls off what lands above
    // white): highlights may sit this much higher before they clip.
    float sensorHeadroomEv = 0.0f;
    float outdoorEv100Low = 8.0f;
    float outdoorEv100High = 11.0f;
    // Highlight-weighted: the percentile sits at the clip (less the margin)
    // whatever the key, within keyRangeEv below / highlightLiftEv above it.
    bool highlightPriority = false;
    float highlightLiftEv = 1.0f;
    // Classic centre-weighted / average meter: the weighted mean of linear
    // luma (bright areas pull it, as on a camera) at linearMeanTarget, and
    // no highlight guard.
    bool linearMean = false;
    float linearMeanTarget = 0.18f;
    // A clipped percentile does not say by how much; assume this far over
    // (also where the linear mean counts clipped samples). Each frame
    // re-guesses from its own clip, so a larger guess overshoots: a
    // highlight barely over the clip dips the exposure by about this much
    // before it recovers.
    float clippedOverEv = 0.3f;
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

// Per-channel histogram: samples whose brightest channel reaches this
// fraction of its saturation are clipped (image_scopes kSceneChannelClip).
inline constexpr float kChannelClip = 0.95f;

struct FrameObservation {
    const std::uint32_t* weightedBins = nullptr;  // AutoKey layout (kSceneKeyBins)
    float clipLumaEv = 0.0f;                       // log2 of the clip luma
    double logExposure = 0.0;                      // realized log2(shutter x request ISO)
    double shutterNs = 0.0;
    double iso = 0.0;                              // request coordinate
    float aperture = 0.0f;
    std::uint64_t timestampNs = 0;
    // Same layout, log2(max channel / its saturation); null or empty: the
    // highlights are judged on luma.
    const std::uint32_t* channelBins = nullptr;
    // Whole-frame log2 luma mean, when weightedBins only cover a spot: the
    // night look judges the scene, not the tapped area.
    std::optional<float> frameLogMeanEv{};
    // Luma bins of the focus area (same layout); empty or null: none. The
    // key is the frame's and the focus area's, blended in EV at focusShare:
    // a subject 3 EV under the frame moves the exposure 3 x focusShare EV.
    // (A weight share in one histogram would not: the key is a median, which
    // ignores a minority and jumps once the focus area is half the weight.)
    const std::uint32_t* focusBins = nullptr;
    float focusShare = 0.0f;
};

struct Metered {
    double desiredLogExposure = 0.0;
    float sceneEv100 = 0.0f;
    float nightOffsetEv = 0.0f;
    float guardEv = 0.0f;  // <= 0
};

// Exposure the scene wants, independent of the frame's own exposure.
// Weighted mean of linear luma over the scene histogram (log2). Top-bin
// (clipped) samples count at the clip plus overEv: the bin itself spans
// up to +4 EV and says nothing about how far over they are.
inline std::optional<float> sceneLinearMeanEv(const std::uint32_t* bins, float clipEv, float overEv) noexcept {
    if (!bins || !std::isfinite(clipEv)) return std::nullopt;
    const double width = double(kSceneKeyMaxEv - kSceneKeyMinEv) / double(kSceneKeyBins);
    double total = 0.0, sum = 0.0;
    for (std::size_t b = 0; b < kSceneKeyBins; ++b) {
        if (!bins[b]) continue;
        const double ev = b + 1 == kSceneKeyBins ? double(clipEv) + double(overEv)
                                                  : kSceneKeyMinEv + (double(b) + 0.5) * width;
        total += bins[b];
        sum += bins[b] * std::exp2(ev);
    }
    if (!(total > 0.0) || !(sum > 0.0)) return std::nullopt;
    return float(std::log2(sum / total));
}

// The highlight percentile of one frame, scene-referred (exposure removed).
struct HighlightReading {
    bool clipped = false;
    // Unclipped: the percentile. Clipped: the clip plus clippedOverEv.
    double sceneEv = 0.0;
    // Exposure is right when the percentile sits at targetEv (frame units).
    double targetEv = 0.0;
};

inline bool binsPopulated(const std::uint32_t* bins) noexcept {
    if (!bins) return false;
    for (std::size_t b = 0; b < kSceneKeyBins; ++b)
        if (bins[b]) return true;
    return false;
}

inline std::optional<HighlightReading> readHighlight(const FrameObservation& f, const MeterPolicy& p) noexcept {
    const bool channel = binsPopulated(f.channelBins);
    const std::uint32_t* bins = channel ? f.channelBins : f.weightedBins;
    const float clipEv = channel ? std::log2(kChannelClip) : f.clipLumaEv;
    const auto high = scenePercentileEv(bins, p.highlightPercentile);
    if (!high) return std::nullopt;
    HighlightReading h;
    h.clipped = *high >= clipEv - 0.05f;
    h.sceneEv = (h.clipped ? double(clipEv) + double(p.clippedOverEv) : double(*high)) - f.logExposure;
    h.targetEv = double(clipEv) - double(channel ? p.channelMarginEv : p.lumaMarginEv);
    return h;
}

// Metering modes (Settings). The sample weighting itself (matrix, centre,
// average, spot) is the GPU meter's; these set the highlight handling.
enum class Metering : int { Matrix = 0, CenterWeighted = 1, Average = 2, Highlight = 3 };
inline MeterPolicy meterPolicyFor(int metering) noexcept {
    MeterPolicy p;
    switch (Metering(metering)) {
        case Metering::CenterWeighted:
        case Metering::Average:
            p.linearMean = true;
            p.maxGuardEv = 0.0f;
            break;
        case Metering::Highlight:
            p.highlightPriority = true;
            p.highlightPercentile = 0.995f;
            p.maxGuardEv = 5.0f;
            break;
        default:
            break;
    }
    return p;
}

// Darkening the guard applies for a highlight needing `need` EV: all of it
// up to maxGuardEv, then easing (smoothly) to guardReleasedEv at
// guardGiveUpEv and beyond.
inline double guardDarkeningEv(double need, const MeterPolicy& p) noexcept {
    const double cap = p.maxGuardEv;
    if (need <= cap) return std::max(0.0, need);
    const double span = double(p.guardGiveUpEv) - cap;
    const double x = span > 0.0 ? std::clamp((need - cap) / span, 0.0, 1.0) : 1.0;
    const double eased = x * x * (3.0 - 2.0 * x);
    return cap + (std::min(double(p.guardReleasedEv), cap) - cap) * eased;
}

inline double clippedGuardCapEv(float ev100, const MeterPolicy& p) noexcept {
    const float span = p.outdoorEv100High - p.outdoorEv100Low;
    const float x = span > 0.0f && std::isfinite(ev100) ? std::clamp((ev100 - p.outdoorEv100Low) / span, 0.0f, 1.0f)
                                                        : 0.0f;
    const float s = x * x * (3.0f - 2.0f * x);
    const float indoor = std::min(p.clippedGuardMaxEv, p.maxGuardEv);
    return double(indoor + (p.maxGuardEv - indoor) * s);
}

// The user's EV applies after the highlight handling, as on a camera: the
// guard protects the metered exposure, not the user's choice.
inline std::optional<Metered> meter(const FrameObservation& f, NightLook look, float userEv,
                                    const MeterPolicy& p = {}) noexcept {
    if (!f.weightedBins || !std::isfinite(f.logExposure) || !std::isfinite(f.clipLumaEv)) return std::nullopt;
    // Highlights stay whole-frame; only the key leans to the focus area.
    auto key = sceneKeyEv(f.weightedBins);
    if (key && f.focusShare > 0.0f && binsPopulated(f.focusBins)) {
        if (const auto focusKey = sceneKeyEv(f.focusBins))
            key = *key + std::clamp(f.focusShare, 0.0f, 1.0f) * (*focusKey - *key);
    }
    const auto high = readHighlight(f, p);
    if (!key || !high) return std::nullopt;
    Metered m;
    const float nightKey = f.frameLogMeanEv && std::isfinite(*f.frameLogMeanEv) ? *f.frameLogMeanEv : *key;
    m.sceneEv100 = sceneEv100(nightKey, f.shutterNs, f.iso, f.aperture, p.midGray);
    m.nightOffsetEv = nightKeyOffsetEv(m.sceneEv100, look, p);
    // Scene-referred (exposure removed) key and highlight.
    const double keyScene = double(*key) - f.logExposure;
    double byKey = std::log2(double(p.targetKey)) + double(m.nightOffsetEv) - keyScene;
    if (p.linearMean) {
        if (const auto mean = sceneLinearMeanEv(f.weightedBins, f.clipLumaEv, p.clippedOverEv))
            byKey = std::log2(double(p.linearMeanTarget)) + double(m.nightOffsetEv) - (double(*mean) - f.logExposure);
    }
    const double byHighlight = high->targetEv - high->sceneEv + double(p.sensorHeadroomEv);
    double darkening = guardDarkeningEv(byKey - byHighlight, p);
    if (high->clipped) darkening = std::min(darkening, clippedGuardCapEv(m.sceneEv100, p));
    const double guarded = p.highlightPriority
                               ? std::clamp(byHighlight, byKey - double(p.maxGuardEv), byKey + double(p.highlightLiftEv))
                               : byKey - darkening;
    m.guardEv = float(guarded - byKey);
    m.desiredLogExposure = guarded + double(userEv);
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
    // Highlight headroom: the sensor is exposed this far under the shown
    // exposure in good light (as Camera2 AE does with its post-RAW boost),
    // fading to none in dim scenes where noise matters more (see
    // headroomForEv100).
    float headroomEv = 1.0f;
};

// Headroom in effect at a scene EV100: full at 7 and up, none at 4 and down.
inline float headroomForEv100(float headroomEv, float ev100) noexcept {
    if (!std::isfinite(ev100)) return 0.0f;
    const float x = std::clamp((ev100 - 4.0f) / 3.0f, 0.0f, 1.0f);
    return std::max(0.0f, headroomEv) * x * x * (3.0f - 2.0f * x);
}

struct Step {
    Solution request;           // what to put on the repeating request
    double targetLogExposure;   // smoothed target the viewfinder shows
    Metered metered;
    float headroomEv = 0.0f;    // request = target - headroomEv
};

// Frame-lane state: meter -> clamp to reachable -> filter -> solve.
// A frame renders before its own histogram is metered, so its viewfinder
// gain is the latest smoothed target over the frame's realized exposure
// (residualEvFor), fixed on first use so a still gets exactly what was shown.
class Controller {
   public:
    std::optional<Step> update(const FrameObservation& f, const Settings& s, float userEv,
                               const MeterPolicy& mp = {}, const FilterPolicy& fp = {}) noexcept {
        // Headroom follows the light of the last metered frame.
        const float headroom =
            last_ ? headroomForEv100(s.headroomEv, last_->metered.sceneEv100) : 0.0f;
        MeterPolicy policy = mp;
        policy.sensorHeadroomEv = headroom;
        const auto m = meter(f, s.night, userEv, policy);
        if (!m) return std::nullopt;
        // Clamp the desire to what the sensor (+ digital cap) can deliver, so
        // the viewfinder never has to show an exposure the photo cannot get.
        const auto reachable = solve(requestFor(m->desiredLogExposure - headroom, s)).logExposure() + headroom;
        const double target = filter_.update(reachable, f.timestampNs, fp);
        Step step{solve(requestFor(target - headroom, s)), target, *m, headroom};
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
