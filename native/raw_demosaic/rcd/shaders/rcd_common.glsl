#ifndef RAWR_RCD_COMMON_GLSL
#define RAWR_RCD_COMMON_GLSL

layout(std430, set = 0, binding = 0) readonly buffer NormalizedInput { float bayer255[]; }
normalizedInput;
layout(set = 0, binding = 1, r16ui) uniform readonly uimage2D raw16Input;
layout(set = 0, binding = 2, r32f) uniform readonly image2D rawFloatInput;
layout(set = 0, binding = 3) uniform sampler2D packedInput;
layout(set = 0, binding = 4, rg32f) uniform image2D directionImage;
layout(set = 0, binding = 5, rgba16f) uniform image2D outputImage;

layout(push_constant) uniform Push {
    uint width;
    uint height;
    uint pattern;
    uint inputMode;
    vec4 black;
    vec4 invRange;
    float outputFactor;
    float outputAlpha;
    uint autoBalance;
    vec4 fixedBalance;
}
pc;

layout(std430, set=0, binding=6) buffer Balance { vec4 gain; vec4 blocks[]; } balance;

const float EPS = 1e-5;
const float EPS2 = 1e-10;
const int RCD_BORDER = 9;
const int RCD_WORK_MARGIN = 4;

int cfaComponent(ivec2 p) {
    int x = p.x & 1, y = p.y & 1;
    if (pc.pattern == 0u) return y == 0 ? (x == 0 ? 0 : 1) : (x == 0 ? 2 : 3);  // RGGB
    if (pc.pattern == 1u) return y == 0 ? (x == 0 ? 1 : 0) : (x == 0 ? 3 : 2);  // GRBG
    if (pc.pattern == 2u) return y == 0 ? (x == 0 ? 2 : 3) : (x == 0 ? 0 : 1);  // GBRG
    return y == 0 ? (x == 0 ? 3 : 2) : (x == 0 ? 1 : 0);                        // BGGR
}
int rgbColorAt(ivec2 p) {
    int c = cfaComponent(p);
    return c == 0 ? 0 : (c == 3 ? 2 : 1);
}
float component(vec4 v, int c) { return c == 0 ? v.x : (c == 1 ? v.y : (c == 2 ? v.z : v.w)); }
ivec2 clampRaw(ivec2 p) { return clamp(p, ivec2(0), ivec2(int(pc.width) - 1, int(pc.height) - 1)); }

float unbalancedCfa(ivec2 p) {
    p = clampRaw(p);
    int c = cfaComponent(p);
    int parity = ((p.y & 1) << 1) | (p.x & 1);
    if (pc.inputMode == 0u) return normalizedInput.bayer255[p.y * int(pc.width) + p.x] * (1.0 / 255.0);
    if (pc.inputMode == 1u) {
        float v = float(imageLoad(raw16Input, p).r);
        return (v - component(pc.black, parity)) * component(pc.invRange, parity);
    }
    if (pc.inputMode == 2u) {
        float v = imageLoad(rawFloatInput, p).r;
        return (v - component(pc.black, parity)) * component(pc.invRange, parity);
    }
    vec4 v = texelFetch(packedInput, p >> 1, 0);
    return component(v, c);
}

float cfa(ivec2 p) {
    float v = unbalancedCfa(p);
    return v * (pc.autoBalance != 0u ? balance.gain[rgbColorAt(clampRaw(p))] : pc.fixedBalance[rgbColorAt(clampRaw(p))]);
}

float hp1d(ivec2 p, ivec2 d) {
    // RCD high-pass color-difference response at p along direction d.
    float h = (cfa(p - 3 * d) - cfa(p - d) - cfa(p + d) + cfa(p + 3 * d)) - 3.0 * (cfa(p - 2 * d) + cfa(p + 2 * d)) +
              6.0 * cfa(p);
    return h * h;
}

float vhDirection(ivec2 p) {
    float vs =
        max(EPS2, hp1d(p + ivec2(0, -1), ivec2(0, 1)) + hp1d(p, ivec2(0, 1)) + hp1d(p + ivec2(0, 1), ivec2(0, 1)));
    float hs =
        max(EPS2, hp1d(p + ivec2(-1, 0), ivec2(1, 0)) + hp1d(p, ivec2(1, 0)) + hp1d(p + ivec2(1, 0), ivec2(1, 0)));
    return vs / (vs + hs);
}

float pqDirection(ivec2 p) {
    ivec2 P = ivec2(1, 1), Q = ivec2(1, -1);
    float ps = max(EPS2, hp1d(p - P, P) + hp1d(p, P) + hp1d(p + P, P));
    float qs = max(EPS2, hp1d(p - Q, Q) + hp1d(p, Q) + hp1d(p + Q, Q));
    return ps / (ps + qs);
}

float directionAt(ivec2 p, int channel) { return imageLoad(directionImage, p)[channel]; }
float refinedDirection(ivec2 p, int channel) {
    float center = directionAt(p, channel);
    float neigh = 0.25 * (directionAt(p + ivec2(-1, -1), channel) + directionAt(p + ivec2(1, -1), channel) +
                          directionAt(p + ivec2(-1, 1), channel) + directionAt(p + ivec2(1, 1), channel));
    return abs(0.5 - center) < abs(0.5 - neigh) ? neigh : center;
}

float ratioDenom(float x) { return EPS + x; }

float lpfRb(ivec2 p) {
    return cfa(p) +
           0.5 * (cfa(p + ivec2(-1, 0)) + cfa(p + ivec2(1, 0)) + cfa(p + ivec2(0, -1)) + cfa(p + ivec2(0, 1))) +
           0.25 * (cfa(p + ivec2(-1, -1)) + cfa(p + ivec2(1, -1)) + cfa(p + ivec2(-1, 1)) + cfa(p + ivec2(1, 1)));
}

bool inRcdWorkingRegion(ivec2 p) {
    return p.x >= RCD_WORK_MARGIN && p.y >= RCD_WORK_MARGIN && p.x < int(pc.width) - RCD_WORK_MARGIN &&
           p.y < int(pc.height) - RCD_WORK_MARGIN;
}
bool inRcdInterior(ivec2 p) {
    return p.x >= RCD_BORDER && p.y >= RCD_BORDER && p.x < int(pc.width) - RCD_BORDER &&
           p.y < int(pc.height) - RCD_BORDER;
}
vec3 nativeCfaRgb(ivec2 p) {
    vec3 v = vec3(0.0);
    int own = rgbColorAt(p);
    v[own] = cfa(p);
    return v;
}

// Exact semantic port of librtprocess::bayerborder_demosaic() for the
// 3-color Bayer CFA used by librtprocess RCD: R=0, G=1, B=2.
// Source oracle: CarVac/librtprocess src/demosaic/border.cc.
// For a border pixel, preserve its native CFA sample and reconstruct each
// missing channel as the unweighted mean of same-color samples in the clipped
// 3x3 neighborhood. With a valid Bayer CFA every channel has >=1 contributor.
vec3 librtprocessBayerBorderRgb(ivec2 p) {
    vec3 sum = vec3(0.0);
    ivec3 count = ivec3(0);
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            ivec2 q = p + ivec2(dx, dy);
            if (q.x < 0 || q.y < 0 || q.x >= int(pc.width) || q.y >= int(pc.height)) continue;
            int c = rgbColorAt(q);
            float v = cfa(q);
            if (c == 0) {
                sum.r += v;
                count.r++;
            } else if (c == 1) {
                sum.g += v;
                count.g++;
            } else {
                sum.b += v;
                count.b++;
            }
        }
    }
    int own = rgbColorAt(p);
    float ownv = cfa(p);
    vec3 outRgb = sum / vec3(count);
    if (own == 0)
        outRgb.r = ownv;
    else if (own == 1)
        outRgb.g = ownv;
    else
        outRgb.b = ownv;
    return outRgb;
}

vec3 rgbAt(ivec2 p) { return imageLoad(outputImage, p).rgb; }
void storeRgb(ivec2 p, vec3 rgb) { imageStore(outputImage, p, vec4(rgb * pc.outputFactor, pc.outputAlpha)); }
// Intermediate passes store normalized RCD-domain RGB. Only the final green-site
// pass applies outputFactor globally; helpers below therefore use raw imageStore.
void storeIntermediate(ivec2 p, vec3 rgb) { imageStore(outputImage, p, vec4(rgb, pc.outputAlpha)); }

#endif
