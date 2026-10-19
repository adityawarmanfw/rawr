// SPDX-License-Identifier: GPL-3.0-or-later
// Tile ABI shared with amaze.wgsl (Params) and src/Pipeline.cpp (TileParams).
#ifndef RAWR_AMAZE_TILE_GLSL
#define RAWR_AMAZE_TILE_GLSL
struct TileParams {
    uint srcOriginX, srcOriginY, srcWidth, srcHeight;
    uint outOriginX, outOriginY, outWidth, outHeight;
    uint cfa00, cfa10, cfa01, cfa11;
    float blackR, blackG0, blackG1, blackB;
    float white, wbR, wbG, wbB;
    float color00, color01, color02, color10;
    float color11, color12, color20, color21;
    float color22, sourceIsFloat, outputScale, outputAlpha;
    float inputMode, inputWidth, pattern, reserved;  // amaze.wgsl: highlight_reconstruction, _pad3; reserved = image height
};
// One dispatch covers a batch of tiles; workgroup z selects the tile and its slot.
layout(std430, set = 0, binding = 1) readonly buffer TileParamsBatch { TileParams tiles[]; };
#define tile tiles[gl_WorkGroupID.z]
#define TILE_SLOT gl_WorkGroupID.z
// Per-tile slot sizes, shared with amaze.wgsl (SRC_PIX / OUT_PIX).
const uint kSrcPix = 25600u;
const uint kOutPix = 16384u;

// Packed {R,G1,G2,B} component at raw position p for BayerPattern (rcd_common.glsl).
int cfaComponent(ivec2 p, uint pattern) {
    int x = p.x & 1, y = p.y & 1;
    if (pattern == 0u) return y == 0 ? (x == 0 ? 0 : 1) : (x == 0 ? 2 : 3);  // RGGB
    if (pattern == 1u) return y == 0 ? (x == 0 ? 1 : 0) : (x == 0 ? 3 : 2);  // GRBG
    if (pattern == 2u) return y == 0 ? (x == 0 ? 2 : 3) : (x == 0 ? 0 : 1);  // GBRG
    return y == 0 ? (x == 0 ? 3 : 2) : (x == 0 ? 1 : 0);                        // BGGR
}
#endif
