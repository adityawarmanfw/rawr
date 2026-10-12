// Colour-guide sampling shared by the reconstruction apply passes.
//
// RAWR_HL_SCALE selects the working-image geometry:
//   1  preview: half-resolution RGB, guide cells of 4 pixels.
//   2  still/video: full-resolution RGB, guide cells of 8 pixels.
// Both cover 8 sensor pixels per cell. Requires a `guideImage` (rgba16f) binding.
#if !defined(RAWR_HL_SCALE) || (RAWR_HL_SCALE != 1 && RAWR_HL_SCALE != 2)
#error "RAWR_HL_SCALE must be 1 (preview) or 2 (still)"
#endif

// Bilinear fetch of the low-resolution guide, ignoring empty cells.
vec4 rawrHighlightGuideAt(ivec2 p) {
    ivec2 extent = imageSize(guideImage);
    vec2 q = (vec2(p) + vec2(0.5)) / float(4 * RAWR_HL_SCALE) - vec2(0.5);
    ivec2 q0 = ivec2(floor(q));
    vec2 f = fract(q);
    vec4 sum = vec4(0.0);
    float weight = 0.0;
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            vec4 v = imageLoad(guideImage, clamp(q0 + ivec2(x, y), ivec2(0), extent - ivec2(1)));
            float w = (x == 0 ? 1.0 - f.x : f.x) * (y == 0 ? 1.0 - f.y : f.y) * step(1e-6, dot(v.rgb, vec3(1.0)));
            sum.rgb += v.rgb * w;
            sum.a += max(v.a, 0.0) * w;
            weight += w;
        }
    }
    return weight > 1e-6 ? sum / weight : vec4(0.0);
}

// The mask is classified before lens shading and WB. Corrected RGB magnitude
// cannot classify clipping: a valid sensor sample may exceed its nominal WB
// ceiling after lens shading. Trust every channel that the sensor retained.
vec3 rawrHighlightSensorRepair(ivec2 p) {
    ivec2 q = p / RAWR_HL_SCALE;
    uint state = imageLoad(clipStateImage, clamp(q, ivec2(0), imageSize(clipStateImage) - ivec2(1))).r & 15u;
    return vec3(float((state & 1u) != 0u), float((state & 6u) != 0u), float((state & 8u) != 0u));
}

// Share of fully clipped (all four CFA channels) mask texels in a 9x9 grid at
// 3-texel stride around p (+-12 mask texels). Only evaluated for fully
// clipped pixels, so the loop cost stays confined to light-source cores.
float rawrHighlightCoreFraction(ivec2 p) {
    ivec2 q = p / RAWR_HL_SCALE;
    ivec2 last = imageSize(clipStateImage) - ivec2(1);
    float full = 0.0;
    for (int dy = -12; dy <= 12; dy += 3)
        for (int dx = -12; dx <= 12; dx += 3)
            full += float((imageLoad(clipStateImage, clamp(q + ivec2(dx, dy), ivec2(0), last)).r & 15u) == 15u);
    return full / 81.0;
}
