// Shared single-frame RAW develop for standalone demosaic and fused tone.
// The caller defines the three descriptor bindings before including this file.
layout(binding = VIDEO_RAW_IMAGE_BINDING, r16ui) uniform readonly uimage2D rawImage;
layout(std430, binding = VIDEO_RAW_BUFFER_BINDING) readonly buffer RawWords { uint words[]; } rawBuffer;
layout(push_constant) uniform VideoRawParams {
    vec4 black;
    vec4 invRange;
    vec4 wb;
    uint width;
    uint height;
    uint outWidth;
    uint outHeight;
    uint cropX;
    uint cropY;
    uint pattern;
    uint stridePixels;
    uint bufferEnabled;
    uint reduceCfa;
    uint lscEnabled;
    uint lscWidth;
    uint lscHeight;
    uint monitorEnabled;
} pc;
#define LSC_BINDING VIDEO_LSC_BINDING
#include "lens_shading.glsl"
#include "cfa_common.glsl"

#ifndef VIDEO_RAW_CUSTOM_CACHE
// Square output workgroup side. An N x N tile needs (N+4)^2 RAW samples at
// full resolution, or (2N+4)^2 for fused 2x reduction, including the 5x5 MHC
// halo. Larger tiles load each sample fewer times; the fused RAW-input
// tonemap keeps 8. Lens-shading gain is smooth, so it is applied once per
// output pixel in videoSensorAt instead of per halo tap in the load loop.
#ifndef VIDEO_TILE
#define VIDEO_TILE 8
#endif
const int kVideoTileMax = 2 * VIDEO_TILE + 4;
shared float cachedRaw[kVideoTileMax * kVideoTileMax];
#ifdef VIDEO_CLIP_STATE
// Only the standalone video stage needs the sensor codes. The fused fast
// path keeps its original shared-memory footprint.
shared uint cachedCode[kVideoTileMax * kVideoTileMax];
#endif

int videoTileSide() { return pc.reduceCfa != 0u ? 2 * VIDEO_TILE + 4 : VIDEO_TILE + 4; }
ivec2 videoTileBase() {
    return ivec2(pc.cropX, pc.cropY) + ivec2(gl_WorkGroupID.xy) *
           (pc.reduceCfa != 0u ? 2 * VIDEO_TILE : VIDEO_TILE);
}
float videoCachedAt(ivec2 q) {
    ivec2 local = q - videoTileBase() + ivec2(2);
    return cachedRaw[local.y * videoTileSide() + local.x];
}
#endif

int videoChannelAt(ivec2 q) { return rawrCfaChannelAt(pc.pattern, q); }

// Mirror about the edge pixel; unlike clamping this keeps the CFA phase, so
// the outer rows and columns (visible in Open Gate) demosaic correctly.
ivec2 videoMirrored(ivec2 q) {
    ivec2 last = ivec2(pc.width, pc.height) - 1;
    q = abs(q);
    return min(q, 2 * last - q);
}

// Callers pass already-mirrored coordinates; the loader mirrors once so the
// per-tap path stays cheap. The cache holds black/invRange-normalized values
// without the lens-shading gain; videoSensorAt applies the gain per output.
uint videoCodeAt(ivec2 q) {
    uint v;
    if (pc.bufferEnabled != 0u) {
        uint address = uint(q.y) * pc.stridePixels + uint(q.x);
        uint word = rawBuffer.words[address >> 1u];
        v = (address & 1u) == 0u ? (word & 65535u) : (word >> 16u);
    } else {
        v = imageLoad(rawImage, q).r;
    }
    return v;
}

float videoRawAt(ivec2 q, uint code) {
    int c = videoChannelAt(q);
    return clamp((float(code) - pc.black[c]) * pc.invRange[c], 0.0, 1.0);
}

#ifndef VIDEO_RAW_CUSTOM_CACHE
void videoLoadTile() {
    int side = videoTileSide();
    ivec2 base = videoTileBase();
    for (uint index = gl_LocalInvocationIndex; index < uint(side * side); index += uint(VIDEO_TILE * VIDEO_TILE)) {
        ivec2 q = videoMirrored(base + ivec2(int(index % uint(side)) - 2,
                                             int(index / uint(side)) - 2));
        uint code = videoCodeAt(q);
        cachedRaw[index] = videoRawAt(q, code);
#ifdef VIDEO_CLIP_STATE
        cachedCode[index] = code;
#endif
    }
    // Every invocation reaches this barrier, including output-edge threads.
    barrier();
}
#endif

#ifndef VIDEO_RAW_CUSTOM_CACHE
#define VIDEO_MHC_AT(q) videoCachedAt(q)
#endif
#include "video_mhc.glsl"

vec3 videoGainAt(ivec2 q) {
    // Row parity selects the Camera2 G_even/G_odd map channel, matching the
    // per-tap lookup this replaces.
    return vec3(lensShadingGain(q, 0), lensShadingGain(q, 1), lensShadingGain(q, 3));
}

#ifndef VIDEO_RAW_CUSTOM_CACHE
vec3 videoSensorAt(ivec2 outPixel) {
    ivec2 crop = ivec2(pc.cropX, pc.cropY);
    vec3 sensor;
    if (pc.reduceCfa != 0u) {
        ivec2 p = crop + outPixel * 2;
        sensor = 0.25 * (videoFullRgb(p) * videoGainAt(p) +
                         videoFullRgb(p + ivec2(1, 0)) * videoGainAt(p + ivec2(1, 0)) +
                         videoFullRgb(p + ivec2(0, 1)) * videoGainAt(p + ivec2(0, 1)) +
                         videoFullRgb(p + ivec2(1, 1)) * videoGainAt(p + ivec2(1, 1)));
    } else {
        ivec2 q = crop + outPixel;
        sensor = videoFullRgb(q) * videoGainAt(q);
    }
    return sensor;
}

vec3 videoCameraAt(ivec2 outPixel) {
    float greenWb = 0.5 * (pc.wb.y + pc.wb.z);
    return videoSensorAt(outPixel) * vec3(pc.wb.x, greenWb, pc.wb.w);
}
#endif
