#ifndef RAWR_LTM_COMMON_GLSL
#define RAWR_LTM_COMMON_GLSL
// Shared by every local tone pass (one push block, 128 bytes max).
// Mirrors local_tone/LocalTone.h.
layout(push_constant) uniform LocalTonePush {
    uvec4 size;          // full width, full height, low width, low height
    vec4 toWorking0;     // camera -> AP1 working, column-major columns
    vec4 toWorking1;
    vec4 toWorking2;
    float logGain;       // log2 of the linear render gain (AE post-gain)
    float strength;      // 0..1: shadow compression toward mid grey
    float detail;        // local contrast gain on (log Y - base)
    float eps;           // guided filter regularization (log2 units^2)
    uint block;          // full-res pixels per low-res texel (square)
    int radius;          // box radius in low-res texels
    uint vertical;       // box direction
    uint pad;
} pc;

const vec3 RAWR_LTM_AP1_Y = vec3(0.2722287168, 0.6740817658, 0.0536895174);
const float RAWR_LTM_FLOOR_EV = -16.0;

// log2 render luminance of one camera-RGB pixel.
float rawrLtmLogLuma(vec3 camera) {
    vec3 work = mat3(pc.toWorking0.xyz, pc.toWorking1.xyz, pc.toWorking2.xyz) * max(camera, vec3(0.0));
    return max(log2(max(dot(RAWR_LTM_AP1_Y, work), 1e-12)) + pc.logGain, RAWR_LTM_FLOOR_EV);
}

// Regional lift (EV) for a base log luminance: shadows compressed toward mid
// grey by (1 - strength), at most 5 x strength EV, fading out near black.
float rawrLtmLift(float base) {
    const float pivot = -2.4739312;  // log2(0.18)
    float target = base < pivot ? pivot + (base - pivot) * (1.0 - pc.strength) : base;
    float lift = clamp(target - base, 0.0, 5.0 * pc.strength);
    return lift * clamp((base + 13.0) / 3.0, 0.0, 1.0);
}
#endif
