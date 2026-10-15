#pragma once
#include <array>
#include <cmath>
#include <stdexcept>

#include "tonemap/AdaptiveTone.h"
#include "tonemap/Agx.h"
#include "tonemap/color/ColorSpace.h"

namespace tonemap {

// Realtime photographic controls plus externally supplied AE post gain.
// aePostGain is capture-controller output, not an aesthetic default or sensor gain.
// Supported external AE post-gain range is 0 < gain <= 16x.
// 16x linear gain is +4 EV (log2(16) = 4). Applications integrating an
// exposure controller must configure its maximum post gain at or below this
// value; TonemapEngine rejects larger values and never silently clamps them.
// 64 = the 4x HAL post-gain cap times HDR+ Bracketed's 4 EV headroom lift.
inline constexpr float kMaxAePostGain = 64.0f;

// AgX writes outputSpace.transfer (sRGB or Rec.709) from a parametric curve
// instead of the RAWR NTRL LUT; see Agx.h.
enum class RenderTransform : unsigned { Existing = 0, SRgb = 1, Rec709 = 2, Log = 3, AgX = 4 };

struct TonemapParams {
    float exposureEV;
    // Photographic tone controls are conventional -100..+100 values.
    // Historical field names are retained to avoid a broad API/JNI rename.
    float blackPointEV;     // Blacks UI
    float shadowLiftEV;     // Shadows UI
    float midtoneLiftEV;    // Midtones UI
    float contrast;         // Contrast UI
    // Unused; keeps the byte layout CaptureJob persists for pending jobs.
    float persistedLayoutSlot;
    float whitePointEV;     // Whites UI
    // Independent tone-shaping control. It never enables, tunes, or re-runs RAW
    // channel reconstruction; negative values compress scene-linear highlights.
    float highlightBiasEV;  // Highlights UI
    float saturation;       // Saturation UI, conventional -100..+100
    float vibrance;         // Vibrance UI, conventional -100..+100

    // Externally supplied downstream linear gain from the exposure controller.
    // Identity is 1.0. Supported range: 0 < aePostGain <= kMaxAePostGain.
    // This is NOT sensor gain and does not imply SNR improvement. TonemapEngine
    // rejects out-of-range values; it never silently clamps AE decisions.
    // Kept separate from exposureEV even though both shift pre-tone scene exposure.
    float aePostGain = 1.0f;
    RenderTransform renderTransform = RenderTransform::Existing;
    color::ColorSpace outputSpace{};
    AdaptiveToneParams adaptiveTone{};
    // Read only by RenderTransform::AgX.
    AgxParams agx{};
};

// Renderer configuration. These values describe the current validated rendering
// model rather than user slider state. They can be replaced without changing
// the public control semantics or recompiling shaders.
struct TonemapConfig {
    float middleGray;
    float vibranceStrength;
    float vibranceShadowStart;
    float vibranceShadowEnd;
};

struct TonemapPreset {
    TonemapParams params;
    TonemapConfig config;
};

namespace TonemapPresets {
// Synthetic/hardware-validated V1.3 baseline. Deliberately not named Natural:
// real-camera calibration has not happened yet.
constexpr TonemapPreset NeutralBaseline() noexcept {
    return {
        {        /* params */
         0.0f,   // exposureEV
         0.0f,   // Blacks UI
         0.0f,   // Shadows UI
         0.0f,   // Midtones UI
         0.0f,   // Contrast UI
         0.0f,   // persistedLayoutSlot
         0.0f,   // Whites UI
         0.0f,   // Highlights UI
         0.0f,   // saturation UI
         0.0f,   // vibrance UI
         1.0f},  // aePostGain
        {        /* config */
         0.18f,  // middleGray
         0.85f,  // vibranceStrength (+100 weak-color boost, as Saturation+100)
         0.02f,  // vibranceShadowStart
         0.10f}  // vibranceShadowEnd
    };
}
}  // namespace TonemapPresets

}  // namespace tonemap
