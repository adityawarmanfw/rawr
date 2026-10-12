#ifndef RAWR_GRADE_GLSL
#define RAWR_GRADE_GLSL
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

// Grade parameter sets, fitted so each slider's response on the gray ramp
// matches pre-0.4.0 (sliders after the render, as in 0.3.1) within ~0.01-0.026 output at
// +-50/+-100: NTRL, imported LUTs (log or DI grade space), direct sRGB/Rec.709.
const uint RAWR_GRADE_SET_NTRL = 0u;
const uint RAWR_GRADE_SET_LUT = 1u;
const uint RAWR_GRADE_SET_DIRECT = 2u;

struct RawrGradeParams {
    vec2 contrast;   // log2 slope gain per unit slider: +, -
    vec2 midAmp;     // +, -
    vec2 midShape;   // center, width (encoded units from the pivot)
    vec2 shadowAmp;  // +, -
    vec2 shadowShape;
};

RawrGradeParams rawrGradeParams(uint set) {
    if (set == RAWR_GRADE_SET_NTRL)
        return RawrGradeParams(vec2(0.0900, 0.1245), vec2(0.0370, 0.0421), vec2(-0.1491, 0.1061),
                               vec2(0.0464, 0.0094), vec2(-0.1448, 0.0925));
    if (set == RAWR_GRADE_SET_LUT)
        return RawrGradeParams(vec2(0.0925, 0.1256), vec2(0.0412, 0.0457), vec2(-0.1648, 0.1031),
                               vec2(0.0517, 0.0103), vec2(-0.2211, 0.1595));
    return RawrGradeParams(vec2(0.1068, 0.1352), vec2(0.0509, 0.0522), vec2(-0.2330, 0.1612),
                           vec2(0.0627, 0.0106), vec2(-0.3414, 0.2961));
}

// tone = Contrast, Highlights, Shadows, Midtones.
float rawrGradeToneLuma(float y, float pivot, float black, vec4 tone, uint set) {
    RawrGradeParams G = rawrGradeParams(set);
    // Contrast: power between black and the pivot keeps black fixed, a linear
    // stretch above it; both have slope g at the pivot. g > 0 so tones never
    // invert. Values below black (noise) are left alone.
    if (tone.x != 0.0 && y > black) {
        float g = exp2((tone.x > 0.0 ? G.contrast.x : G.contrast.y) * tone.x);
        float d = y - pivot;
        y = d >= 0.0 ? pivot + d * g : black + (pivot - black) * pow((y - black) / (pivot - black), g);
    }
    // Regional controls.
    float d = y - pivot;
    float yr = y;
    y += tone.w * (tone.w > 0.0 ? G.midAmp.x : G.midAmp.y) * rawrGradeBump(d, G.midShape.x, G.midShape.y);
    y += tone.z * (tone.z > 0.0 ? G.shadowAmp.x : G.shadowAmp.y) * rawrGradeBump(d, G.shadowShape.x, G.shadowShape.y) *
         smoothstep(black, black + 0.25 * (pivot - black), yr);
    y += tone.y * rawrGradeHighlightsShape(d);
    return y;
}

// Saturation x Vibrance chroma gain. lin: scene-linear RGB, linY: its luma;
// guard: vibrance shadow guard start/end in linear luma.
float rawrGradeChromaGain(vec3 lin, float linY, float sat, float vib, float vibStrength, vec2 guard) {
    if (sat == 0.0 && vib == 0.0) return 1.0;
    // Saturation: -100 monochrome, +100 nominal 1.85x opponent chroma.
    float satGain = sat >= 0.0 ? 1.0 + 0.85 * sat : max(0.0, 1.0 + sat);
    // Vibrance prioritizes weak color (+) or removes it first (-).
    // Occupancy: 0 for neutral, ~1 near the gamut edge.
    lin = max(lin, vec3(0.0));
    float hi = max(lin.r, max(lin.g, lin.b));
    float occupancy = clamp((hi - min(lin.r, min(lin.g, lin.b))) / max(hi, 0.06), 0.0, 1.0);
    float vibGain;
    if (vib >= 0.0) {
        // Fade the boost out in deep shadows, where it mostly lifts chroma noise.
        vibGain = 1.0 + vibStrength * vib * pow(max(1.0 - occupancy, 0.0), 0.30) * smoothstep(guard.x, guard.y, linY);
    } else {
        vibGain = mix(1.0, pow(occupancy, 4.0), -vib);
    }
    return satGain * vibGain;
}
#endif
