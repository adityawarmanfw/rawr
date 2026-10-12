#ifndef RAWR_GRADE_GLSL
#define RAWR_GRADE_GLSL
#include "../../tonemap_engine/shaders/color/cst.glsl"
// Grade operators shared by tonemap.comp (SDR render) and gainmap.comp (HDR
// tap), so the gain map encodes headroom only, never an inverse of a slider.
// Sliders arrive as -1..1. Tone works on luma in an encoded space: distances
// are encoded units from the 18% gray pivot, anchored at the space's black
// code. Chroma gains are thresholds in linear light, the same in every space.

float rawrGradeBump(float d, float center, float width) {
    float t = (d - center) / width;
    return exp(-t * t);
}

// Highlights: a ramp above the pivot plus a slope change past it, so negative
// values compress the top (pulling headroom under the shoulder) instead of
// only shifting it. The quadratic ease keeps the slope continuous.
float rawrGradeHighlightsShape(float d) {
    const float w = 0.10;
    float x = max(d - 0.30, 0.0);
    return 0.10 * smoothstep(0.02, 0.30, d) + 0.25 * (x < w ? x * x / (2.0 * w) : x - 0.5 * w);
}

// Grade parameter sets: NTRL, imported LUTs (log or DI grade space), direct
// sRGB/Rec.709. Shadows is fitted to the pre-0.4.0 gray-ramp response
// (sliders after the render, as in 0.3.1). Contrast is scaled so +-100 moves
// -2 and +1 EV by ~0.07 output, a visible S-curve (pre-0.4.0 barely moved).
const uint RAWR_GRADE_SET_NTRL = 0u;
const uint RAWR_GRADE_SET_LUT = 1u;
const uint RAWR_GRADE_SET_DIRECT = 2u;

struct RawrGradeParams {
    vec2 contrast;     // log2 slope gain per unit slider: +, -
    vec2 shadowAmp;    // +, -
    vec2 shadowShape;  // center, width (encoded units from the pivot)
};

RawrGradeParams rawrGradeParams(uint set) {
    if (set == RAWR_GRADE_SET_NTRL)
        return RawrGradeParams(vec2(0.4961, 0.6851), vec2(0.0464, 0.0094), vec2(-0.1448, 0.0925));
    if (set == RAWR_GRADE_SET_LUT)
        return RawrGradeParams(vec2(0.5192, 0.7394), vec2(0.0517, 0.0103), vec2(-0.2211, 0.1595));
    return RawrGradeParams(vec2(0.5389, 0.7143), vec2(0.0627, 0.0106), vec2(-0.3414, 0.2961));
}

// Midtones is an exposure lift centered on 18% gray, the same in every space:
// up to 0.6 stop at +-100, Gaussian over ~1.8 stops, so black and the top stay
// put. Its slope stays above 0.7, so tones never invert.
const float RAWR_GRADE_MID_STOPS = 0.6;
const float RAWR_GRADE_MID_WIDTH = 1.8;

// tone = Contrast, Highlights, Shadows, Midtones; tf: the grade space encoding.
float rawrGradeToneLuma(float y, float pivot, float black, vec4 tone, uint set, uint tf) {
    RawrGradeParams G = rawrGradeParams(set);
    // Contrast: power between black and the pivot keeps black fixed, a linear
    // stretch above it; both have slope g at the pivot. g > 0 so tones never
    // invert. Values below black (noise) are left alone.
    if (tone.x != 0.0 && y > black) {
        float g = exp2((tone.x > 0.0 ? G.contrast.x : G.contrast.y) * tone.x);
        float d = y - pivot;
        y = d >= 0.0 ? pivot + d * g : black + (pivot - black) * pow((y - black) / (pivot - black), g);
    }
    float d = y - pivot;
    float yr = y;
    if (tone.w != 0.0) {
        float lin = rawrTfDecode1(y, tf);
        float gray = rawrTfDecode1(pivot, tf);
        if (lin > 0.0 && gray > 0.0) {
            float ev = log2(lin / gray) / RAWR_GRADE_MID_WIDTH;
            y = rawrTfEncode1(lin * exp2(tone.w * RAWR_GRADE_MID_STOPS * exp(-ev * ev)), tf);
        }
    }
    y += tone.z * (tone.z > 0.0 ? G.shadowAmp.x : G.shadowAmp.y) * rawrGradeBump(d, G.shadowShape.x, G.shadowShape.y) *
         smoothstep(black, black + 0.25 * (pivot - black), yr);
    y += tone.y * rawrGradeHighlightsShape(d);
    return y;
}

// Saturation x Vibrance chroma gain. lin: scene-linear RGB, linY: its luma;
// guard: vibrance shadow guard start/end in linear luma.
float rawrGradeChromaGain(vec3 lin, float linY, float sat, float vib, float vibStrength, vec2 guard) {
    if (sat == 0.0 && vib == 0.0) return 1.0;
    // Saturation: -100 monochrome, +100 nominal 1.85x opponent chroma, for every color.
    float satGain = sat >= 0.0 ? 1.0 + 0.85 * sat : max(0.0, 1.0 + sat);
    // Occupancy: 0 for neutral, ~1 near the gamut edge.
    lin = max(lin, vec3(0.0));
    float hi = max(lin.r, max(lin.g, lin.b));
    float lo = min(lin.r, min(lin.g, lin.b));
    float occupancy = clamp((hi - lo) / max(hi, 0.06), 0.0, 1.0);
    float vibGain;
    if (vib >= 0.0) {
        // Vibrance+ boosts muted color and leaves strong color alone. Skin
        // (r > g > b, orange hue) gets a third of the boost. Faded out in deep
        // shadows, where it mostly lifts chroma noise.
        float weak = 1.0 - smoothstep(0.30, 0.85, occupancy);
        float hue = (lin.g - lin.b) / max(lin.r - lin.b, 1e-6);
        float skin = lin.r >= lin.g && lin.g >= lin.b ? rawrGradeBump(hue, 0.45, 0.22) : 0.0;
        vibGain = 1.0 + vibStrength * vib * weak * (1.0 - 0.67 * skin) * smoothstep(guard.x, guard.y, linY);
    } else {
        // Vibrance- removes muted color first: -100 grays weak color, strong color stays.
        vibGain = 1.0 + vib * (1.0 - smoothstep(0.25, 0.75, occupancy));
    }
    return satGain * vibGain;
}
#endif
