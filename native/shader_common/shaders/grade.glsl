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

// tone = Contrast, Highlights, Shadows, Midtones.
float rawrGradeToneLuma(float y, float pivot, float black, vec4 tone) {
    // Contrast: power between black and the pivot keeps black fixed, a linear
    // stretch above it; both have slope g at the pivot. g > 0 so tones never
    // invert. Values below black (noise) are left alone.
    if (tone.x != 0.0 && y > black) {
        float g = exp2(0.7 * tone.x);
        float d = y - pivot;
        y = d >= 0.0 ? pivot + d * g : black + (pivot - black) * pow((y - black) / (pivot - black), g);
    }
    // Regional controls (widths keep every stacked combination monotonic).
    float d = y - pivot;
    float yr = y;
    y += tone.w * 0.05 * rawrGradeBump(d, 0.0, 0.10);
    y += tone.z * 0.06 * rawrGradeBump(d, -0.14, 0.10) * smoothstep(black, black + 0.25 * (pivot - black), yr);
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
