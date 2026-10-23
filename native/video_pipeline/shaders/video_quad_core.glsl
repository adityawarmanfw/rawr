// Quad-per-thread RAW develop for 1:1 recordings (4K, Open Gate; no 2x reduction).
//
// A VIDEO_QT x VIDEO_QT workgroup covers (2 * VIDEO_QT)^2 output pixels and each
// thread owns one 2x2 Bayer cell. Recording crops are even-aligned (VideoCrop.h),
// so a cell starts on a CFA cell boundary and pixel (x, y) of the cell has the
// same channel in every thread of a wave: no per-pixel channel branching, and
// with the demosaic loops unrolled the roles fold to constants.
//
// Green at red/blue sites is the Hamilton-Adams estimator of video_mhc.glsl
// (keep the two in sync). Red and blue are not MHC kernels: each missing value is
// sqrt(G) + d, where d = sqrt(sample) - sqrt(G) is interpolated from the nearest
// samples of that colour, each weighted by 1 / (VIDEO_GUIDE_EPS + |sqrt(G) -
// sqrt(G_neighbour)|). The weights come from the dense green plane, so chroma
// follows green edges instead of straddling them, and the sqrt domain keeps
// saturated colour edges from over- or undershooting. The sample's own channel
// is kept exactly.
//
// Phase A fills a shared plane of packed half pairs (sqrt(G), d) for the tile
// plus a 2-pixel ring; phase B reads it back per cell.
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

// Chroma weight floor, in sqrt(linear) units: differences below it count as flat,
// which keeps sensor noise in the green plane from steering the weights.
const float kGuideEps = 0.03;

// Raw cache: the tile plus a 4-pixel ring (the green estimator reaches 2 pixels
// past the plane's 2-pixel ring). Planes: the tile plus a 2-pixel ring.
const int kRawSide = 2 * VIDEO_QT + 8;
const int kPlaneSide = 2 * VIDEO_QT + 4;
const int kPlaneCells = VIDEO_QT + 2;
shared float cachedRaw[kRawSide * kRawSide];
// One packed half pair per plane position: (sqrt(green), R/B difference).
shared uint greenChroma[kPlaneSide * kPlaneSide];
ivec2 gTileBase;  // absolute sensor position of the tile's first output pixel

float videoCachedAt(ivec2 q) {
    ivec2 local = q - gTileBase + ivec2(4);
    return cachedRaw[local.y * kRawSide + local.x];
}

int videoChannelAbs(ivec2 q) { return rawrCfaChannelAt(pc.pattern, q); }
// A cell origin is even, so a site's channel depends only on its offset from it.
int videoChannelRel(ivec2 offset) { return rawrCfaChannelAt(pc.pattern, offset); }

// Mirror about the edge pixel; unlike clamping this keeps the CFA phase.
ivec2 videoMirrored(ivec2 q) {
    ivec2 last = ivec2(pc.width, pc.height) - 1;
    q = abs(q);
    return min(q, 2 * last - q);
}

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
    int c = videoChannelAbs(q);
    return clamp((float(code) - pc.black[c]) * pc.invRange[c], 0.0, 1.0);
}

void videoLoadTile() {
    gTileBase = ivec2(pc.cropX, pc.cropY) + ivec2(gl_WorkGroupID.xy) * (2 * VIDEO_QT);
    for (uint index = gl_LocalInvocationIndex; index < uint(kRawSide * kRawSide);
         index += uint(VIDEO_QT * VIDEO_QT)) {
        ivec2 q = videoMirrored(gTileBase + ivec2(int(index % uint(kRawSide)) - 4,
                                                  int(index / uint(kRawSide)) - 4));
        cachedRaw[index] = videoRawAt(q, videoCodeAt(q));
    }
    // Every invocation reaches this barrier, including output-edge threads.
    barrier();
}

// Lens-shading gain per output pixel, applied to the demosaiced RGB.
vec3 videoGainAt(ivec2 q) {
    return vec3(lensShadingGain(q, 0), lensShadingGain(q, 1), lensShadingGain(q, 3));
}

// ---- phase A: green at red/blue sites ----
float gRaw[6][6];  // normalized RAW around a cell, origin gCell - 2
ivec2 gCell;
#define VIDEO_RAW_AT(q) gRaw[(q).y - gCell.y + 2][(q).x - gCell.x + 2]

// Same arithmetic as the green branch of videoFullRgb in video_mhc.glsl.
float videoGreenAtRedBlue(ivec2 p) {
    float center = VIDEO_RAW_AT(p);
    float left = VIDEO_RAW_AT(p + ivec2(-1, 0)), right = VIDEO_RAW_AT(p + ivec2(1, 0));
    float up = VIDEO_RAW_AT(p + ivec2(0, -1)), down = VIDEO_RAW_AT(p + ivec2(0, 1));
    float h1 = left + right;
    float v1 = up + down;
    float h2 = VIDEO_RAW_AT(p + ivec2(-2, 0)) + VIDEO_RAW_AT(p + ivec2(2, 0));
    float v2 = VIDEO_RAW_AT(p + ivec2(0, -2)) + VIDEO_RAW_AT(p + ivec2(0, 2));
    float gh = 0.5 * (h1) + 0.25 * (2.0 * center - h2);
    float gv = 0.5 * (v1) + 0.25 * (2.0 * center - v2);
    float dh = abs(left - right) + abs(2.0 * center - h2);
    float dv = abs(up - down) + abs(2.0 * center - v2);
    float wsum = dh + dv;
    float gIso = wsum > 1e-6 ? (dv * gh + dh * gv) / wsum : 0.5 * (gh + gv);
    float gMean = 0.25 * (h1 + v1);
    float gAllow = 0.5 * (max(max(left, right), max(up, down)) -
                          min(min(left, right), min(up, down))) + 1e-4;
    return max(gMean + clamp(gIso - gMean, -gAllow, gAllow), 0.0);
}

// Fills the plane for every 2x2 cell of the tile plus a one-cell ring.
void videoBuildPlane() {
    for (uint index = gl_LocalInvocationIndex; index < uint(kPlaneCells * kPlaneCells);
         index += uint(VIDEO_QT * VIDEO_QT)) {
        ivec2 cell = ivec2(int(index % uint(kPlaneCells)), int(index / uint(kPlaneCells)));
        gCell = gTileBase + 2 * (cell - 1);
        [[unroll]] for (int y = 0; y < 6; ++y)
            [[unroll]] for (int x = 0; x < 6; ++x) gRaw[y][x] = videoCachedAt(gCell + ivec2(x - 2, y - 2));
        [[unroll]] for (int dy = 0; dy < 2; ++dy) {
            [[unroll]] for (int dx = 0; dx < 2; ++dx) {
                float value = gRaw[dy + 2][dx + 2];
                int c = videoChannelRel(ivec2(dx, dy));
                float sqrtGreen;
                float diff = 0.0;
                if (c == 1 || c == 2) {
                    sqrtGreen = sqrt(value);
                } else {
                    sqrtGreen = sqrt(videoGreenAtRedBlue(gCell + ivec2(dx, dy)));
                    diff = sqrt(value) - sqrtGreen;
                }
                greenChroma[(2 * cell.y + dy) * kPlaneSide + 2 * cell.x + dx] = packHalf2x16(vec2(sqrtGreen, diff));
            }
        }
    }
    barrier();
}

// ---- phase B: chroma from the plane ----
float gSqrtGreen[4][4];  // origin = cell - 1
float gDiff[4][4];       // R/B differences, same layout

float videoGuide(float a, float b) { return 1.0 / (kGuideEps + abs(a - b)); }

// Weighted mean of the differences at offsets a and b around pixel (x, y) of the cell.
float videoPair(int x, int y, ivec2 a, ivec2 b) {
    float g = gSqrtGreen[y + 1][x + 1];
    float wa = videoGuide(g, gSqrtGreen[y + 1 + a.y][x + 1 + a.x]);
    float wb = videoGuide(g, gSqrtGreen[y + 1 + b.y][x + 1 + b.x]);
    return (wa * gDiff[y + 1 + a.y][x + 1 + a.x] + wb * gDiff[y + 1 + b.y][x + 1 + b.x]) / (wa + wb);
}

float videoDiagonals(int x, int y) {
    float g = gSqrtGreen[y + 1][x + 1];
    float w0 = videoGuide(g, gSqrtGreen[y][x]), w1 = videoGuide(g, gSqrtGreen[y][x + 2]);
    float w2 = videoGuide(g, gSqrtGreen[y + 2][x]), w3 = videoGuide(g, gSqrtGreen[y + 2][x + 2]);
    return (w0 * gDiff[y][x] + w1 * gDiff[y][x + 2] + w2 * gDiff[y + 2][x] + w3 * gDiff[y + 2][x + 2]) /
           (w0 + w1 + w2 + w3);
}

float videoSquare(float v) { v = max(v, 0.0); return v * v; }

// Loads the cell's 4x4 neighbourhood of the plane into registers.
void videoLoadCellPlane(ivec2 cellOrigin) {
    ivec2 local = cellOrigin - gTileBase;  // inside the tile; the plane ring is 2, the cell ring 1
    [[unroll]] for (int y = 0; y < 4; ++y) {
        [[unroll]] for (int x = 0; x < 4; ++x) {
            vec2 plane = unpackHalf2x16(greenChroma[(local.y + 1 + y) * kPlaneSide + local.x + 1 + x]);
            gSqrtGreen[y][x] = plane.x;
            gDiff[y][x] = plane.y;
        }
    }
}

// Demosaiced RGB of pixel (x, y) of the cell; `value` is its own RAW sample.
vec3 videoPixelRgb(int x, int y, float value) {
    int c = videoChannelRel(ivec2(x, y));
    float sg = gSqrtGreen[y + 1][x + 1];
    const ivec2 left = ivec2(-1, 0), right = ivec2(1, 0), up = ivec2(0, -1), down = ivec2(0, 1);
    if (c == 0) return vec3(value, videoSquare(sg), videoSquare(sg + videoDiagonals(x, y)));
    if (c == 3) return vec3(videoSquare(sg + videoDiagonals(x, y)), videoSquare(sg), value);
    // Green in the red row (1): red left/right, blue above/below; in the blue row (2) the reverse.
    float horizontal = videoSquare(sg + videoPair(x, y, left, right));
    float vertical = videoSquare(sg + videoPair(x, y, up, down));
    return c == 1 ? vec3(horizontal, value, vertical) : vec3(vertical, value, horizontal);
}
