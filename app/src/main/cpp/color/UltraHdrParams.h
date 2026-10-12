#pragma once
#include <gainmap/GainmapCompute.h>
namespace rawrcam::color {
struct UltraHdrParams {
    bool enabled = false;
    // Map JPEG quality. Fixed at 92 (was 95): full-res RGB maps at q92
    // hold smooth gradients without banding; q90 showed 8x8 blocking risk
    // in sky, q95+ buys nothing visible. Base quality is separate.
    int gainmapQuality = 92;
    // Identity floor (1x): the file must never darken the base rendition.
    float gainMapMinLog2 = 0.0f;
    float gainMapMaxLog2 = 4.7090998f;
    float gamma = 1.0f;
    float offsetSdr = 0.015625f;
    float offsetHdr = 0.015625f;
    float hdrCapacityMinLog2 = 0.0f;
    float hdrCapacityMaxLog2 = 4.7090998f;
    // Linear gain for sensor-clipped texels (clip mask nonzero): the HDR tap
    // pins them at sensor white so the measured ratio can only reach the
    // capture exposure there. Positive values add specular dazzle beyond the
    // measured headroom, but flatten large clipped washes into hard-edged
    // discs. Default 0 (pure measured ratio: smooth natural rolloff; the
    // ratio still carries genuine recovered headroom above the exposure).
    float clipBoost = 0.0f;
    // Chroma protection (see gainmap::GainmapParams::satProtect): keeps
    // saturated brights (warm lamps, neon) from clipping white under the
    // multiplicative single-channel gain. Default 0.6.
    float satProtect = 0.6f;
    // Multi-channel (RGB) gain map (see gainmap::GainmapParams): per-channel
    // gains preserve hue through a white SDR base (warm lamp stays yellow).
    // Metadata stays channel-identical so single-channel readers keep
    // parsing; verified on Photos with the lamp/neon scenes.
    bool multiChannelMap = true;
    // Post-encode map blur sigma in map texels (see gainmap::GainmapParams).
    // Default 0 (off: blur passes skipped, sharper maps, no edge halo).
    float mapBlurSigma = 0.0f;
    // Full-resolution RGB map grid (see gainmap::GainmapParams::fullResMap).
    // Default true (validated A/B: sharper, no extra halo).
    bool fullResMap = true;
    // Single-pass adaptive ceiling (applied in the JPEG writer on the GPU
    // wide-range map, no second render): p99.9 of per-pixel mean log-gain +
    // fitMargin, rounded up to fitStep, clamped to [fitFloor, displayCap].
    // displayCap 1.45 (2.8x) is the on-device ladder verdict: higher caps
    // push mids/shadows dark next to the brighter highlights on this panel.
    float gainMapFitMargin = 0.25f;
    float gainMapFitStep = 0.25f;
    float gainMapFitFloor = 1.0f;
    float gainMapDisplayCap = 1.45f;
    // Soft shoulder above the fitted max (same tanh family as the chroma
    // knee below): over-max content compresses into the top shoulderWidth EV
    // starting shoulderKneeBelowMax under max instead of clipping to flat
    // white. Encode-only like satProtect.
    float shoulderWidth = 0.5f;
    float shoulderKneeBelowMax = 0.5f;
    // Luma baseline floor (applied before the shoulder): every texel gets at
    // least fitted * baseShare * Y^baseExponent EV, Y = linear Rec.709 luma
    // of the SDR base. The measured ratio is <= 1x below the highlights (the
    // tonemap lifts mids above the HDR tap), so without a floor mids and
    // shadows stay at identity and read underexposed once the display dims
    // SDR white for HDR. Relative to the fitted max so the floor scales with
    // display headroom like the highlights. 0.75 / 0.4 from the on-device
    // A/B across profiles; the lower exponent keeps deep-shadow profiles lifted.
    // baseShare 0 disables (measured ratio only).
    float baseShare = 0.75f;
    float baseExponent = 0.4f;
    // Soft-knee chroma compression around the per-pixel channel mean
    // (d' = T*tanh(d/T)): slope 1 at neutral, asymptote +/-T. Kills hue
    // fringing (magenta windows) without the contour a hard clip prints.
    float chromaKnee = 0.05f;
    // Film glow factor for film+UltraHDR (see gainmap::GainmapParams): scales
    // the post/pre scatter quotient toward the HDR numerator so halation /
    // camera-diffusion glow keeps headroom with hue. 0 = pure scene tap.
    // Default 0.5; upper clamp 2.0 bounds dark-neighbor bleed. Encode-only.
    float glowStrength = 0.5f;
    float glowMax = 2.0f;

    [[nodiscard]] gainmap::GainmapParams toGainmapParams() const {
        gainmap::GainmapParams p;
        p.minLog2 = gainMapMinLog2;
        p.maxLog2 = gainMapMaxLog2;
        p.gamma = gamma;
        p.offsetSdr = offsetSdr;
        p.offsetHdr = offsetHdr;
        p.hdrCapacityMin = hdrCapacityMinLog2;
        p.hdrCapacityMax = hdrCapacityMaxLog2;
        p.clipBoost = clipBoost;
        p.satProtect = satProtect;
        p.multiChannelMap = multiChannelMap;
        p.mapBlurSigma = mapBlurSigma;
        p.fullResMap = fullResMap;
        p.glowStrength = glowStrength;
        p.glowMax = glowMax;
        return p;
    }
};
}  // namespace rawrcam::color
