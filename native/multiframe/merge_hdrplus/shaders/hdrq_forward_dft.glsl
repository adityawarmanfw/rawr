// Port of hdr-plus-swift forward_dft (8x8 tiles, each RGBA channel
// independently, raised-cosine window). One 8x8 workgroup per tile: lane
// (dm, dn) computes one frequency bin; the column pass goes through shared
// memory. Output layout matches upstream: bin (dm, dn) of tile (tx, ty) is
// at x = 2*(8*tx + dm) (+0 real, +1 imaginary), y = 8*ty + dn.
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0, rgba16f) readonly uniform image2D src;
layout(set = 0, binding = 1, rgba16f) writeonly uniform image2D ft;
#ifdef RAWR_DFT_SEED
layout(set = 0, binding = 2, rgba16f) writeonly uniform image2D seed;
#endif
layout(push_constant) uniform P {
    ivec2 tiles;
    float frameInv;  // 1 / fp16 storage scale of the frames
    float scale;  // spectra are stored in fp16: scaled so the sum over the burst stays far below 65504
} p;

const float PI = 3.14159265358979323846;
// cos / sin of 2*pi*k/8, k mod 8 (exact eighth-turn values; the loops below index them instead of calling cos/sin).
const float C8[8] = float[8](1.0, 0.70710678118, 0.0, -0.70710678118, -1.0, -0.70710678118, 0.0, 0.70710678118);
const float S8[8] = float[8](0.0, 0.70710678118, 1.0, 0.70710678118, 0.0, -0.70710678118, -1.0, -0.70710678118);
// Raised-cosine window 0.5 - 0.5 cos(2 pi (j + 0.5) / 8).
const float HANN8[8] = float[8](0.03806023374, 0.30865828381, 0.69134171619, 0.96193976626, 0.96193976626, 0.69134171619, 0.30865828381, 0.03806023374);
shared vec4 tmpRe[8][8];  // [dn][dm]
shared vec4 tmpIm[8][8];

void main() {
    ivec2 t = ivec2(gl_WorkGroupID.xy);
    if (any(greaterThanEqual(t, p.tiles))) return;
    int dm = int(gl_LocalInvocationID.x), dn = int(gl_LocalInvocationID.y);
    ivec2 t0 = t * 8;
    // Column-wise (y) transform of the windowed tile.
    vec4 re = vec4(0.0), im = vec4(0.0);
    for (int dy = 0; dy < 8; ++dy) {
        float w = HANN8[dm] * HANN8[dy];
        vec4 v = (w * p.frameInv) * imageLoad(src, t0 + ivec2(dm, dy));
        re += C8[(dn * dy) & 7] * v;
        im += -S8[(dn * dy) & 7] * v;
    }
    tmpRe[dn][dm] = re;
    tmpIm[dn][dm] = im;
    barrier();
    // Row-wise (x) transform.
    re = vec4(0.0);
    im = vec4(0.0);
    for (int dx = 0; dx < 8; ++dx) {
        float c = C8[(dm * dx) & 7], s = -S8[(dm * dx) & 7];
        vec4 dRe = tmpRe[dn][dx], dIm = tmpIm[dn][dx];
        re += c * dRe - s * dIm;
        im += s * dRe + c * dIm;
    }
    ivec2 o = ivec2(2 * (t0.x + dm), t0.y + dn);
    re *= p.scale;
    im *= p.scale;
    imageStore(ft, o, re);
    imageStore(ft, o + ivec2(1, 0), im);
#ifdef RAWR_DFT_SEED
    // Separate storage: the running merge mutates seed, never the reference.
    imageStore(seed, o, re);
    imageStore(seed, o + ivec2(1, 0), im);
#endif
}
