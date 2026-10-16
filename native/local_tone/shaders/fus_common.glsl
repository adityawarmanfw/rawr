#ifndef RAWR_FUS_COMMON_GLSL
#define RAWR_FUS_COMMON_GLSL
// Exposure fusion passes (local_tone/ExposureFusion.h). One push block.
layout(push_constant) uniform FusionPush {
    uvec4 size;      // full width, full height, base-level width, base-level height
    uvec4 level;     // this pass: source width, source height, destination width, destination height
    vec4 to709_0;    // camera -> linear Rec.709 (x render gain x exposure), column-major columns
    vec4 to709_1;
    vec4 to709_2;
    float shadows;     // linear multiplier of the shadows exposure
    float highlights;  // linear multiplier of the highlights exposure
    float sigmaSq;
    uint block;        // full-res pixels per base-level texel (square)
    uint boost;        // weight Laplacians by magnitude
    uint targetCount, pad1, pad2;
} pc;

// three.js r138 ACESFilmicToneMapping (toneMappingExposure 1), Rec.709 in.
vec3 rawrFusAces(vec3 color) {
    const mat3 inMat = mat3(vec3(0.59719, 0.07600, 0.02840), vec3(0.35458, 0.90834, 0.13383),
                            vec3(0.04823, 0.01566, 0.83777));
    const mat3 outMat = mat3(vec3(1.60475, -0.10208, -0.00327), vec3(-0.53108, 1.10813, -0.07276),
                             vec3(-0.07367, -0.00605, 1.07602));
    color *= 1.0 / 0.6;
    color = inMat * color;
    vec3 a = color * (color + 0.0245786) - 0.000090537;
    vec3 b = color * (0.983729 * color + 0.4329510) + 0.238081;
    return clamp(outMat * (a / b), 0.0, 1.0);
}

const vec3 RAWR_FUS_Y = vec3(0.1, 0.7, 0.2);

vec3 rawrFus709(vec3 camera) {
    return max(mat3(pc.to709_0.xyz, pc.to709_1.xyz, pc.to709_2.xyz) * max(camera, vec3(0.0)), vec3(0.0));
}

// Demo LuminanceShader: (highlights, midtones, shadows) lightness.
vec3 rawrFusLightness(vec3 rgb709) {
    return vec3(sqrt(dot(rawrFusAces(rgb709 * pc.highlights), RAWR_FUS_Y)),
                sqrt(dot(rawrFusAces(rgb709), RAWR_FUS_Y)),
                sqrt(dot(rawrFusAces(rgb709 * pc.shadows), RAWR_FUS_Y)));
}

// Demo ExposureWeightsShader.
vec3 rawrFusWeights(vec3 lightness) {
    vec3 d = lightness - vec3(0.5);
    vec3 w = exp(-0.5 * d * d * pc.sigmaSq);
    return w / (dot(w, vec3(1.0)) + 0.00001);
}
#endif
