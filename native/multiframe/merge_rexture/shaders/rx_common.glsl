// Rexture shared helpers. Reference: tmp-free summary of the algorithm lives in merge_rexture/README.md.
// CFA codes: 0 RGGB, 1 GRBG, 2 GBRG, 3 BGGR (same as RawMergeCommon CfaPattern).

// Offsets (x, y) of the red site inside a 2x2 quad.
ivec2 rxRedOffset(int cfa) { return ivec2((cfa == 1 || cfa == 3) ? 1 : 0, (cfa == 2 || cfa == 3) ? 1 : 0); }

// A normalised sample at or above this is treated as clipped. Below the white level on purpose: some sensors clip a
// colour a few percent under it (V2562 35 mm red at about 0.978).
const float RX_CLIP_NORM = 0.95;

// Normalised value of a raw sample at (already clamped) site q. Coordinates are clamped to the image first and the
// phase of the clamped site is used: identical to normalising the whole image and edge-padding it.
float rxNormFromRaw(float rawValue, ivec2 qClamped, vec4 black, float white) {
    int phase = (qClamped.y & 1) * 2 + (qClamped.x & 1);
    float b = black[phase];
    float den = max(1.0, white - b);
    return (rawValue - b) / den;
}

// ---- covariance (kernel) helpers -------------------------------------------------------------------------------------
// Packed (cxx, cxy, cyy). The 4 neighbouring half-resolution covariances of output pixel o are fetched by the caller
// (image formats cannot be passed to functions); RX_COV_NEIGHBOURS does it for an image named `covI`.
// cov_inverse_at: bilinear (edge clamp) at ((oy - .5) / 2, (ox - .5) / 2), then the 2x2 inverse used by the kernel weight.
#define RX_COV_NEIGHBOURS(COVI, O, A, B, C, D, F)                                                                    \
    {                                                                                                                \
        vec2 k_ = (vec2(O) - 0.5) / 2.0;                                                                             \
        vec2 b_ = floor(k_);                                                                                         \
        F = k_ - b_;                                                                                                 \
        ivec2 sz_ = imageSize(COVI);                                                                                 \
        ivec2 lo_ = clamp(ivec2(b_), ivec2(0), sz_ - 1), hi_ = clamp(ivec2(b_) + 1, ivec2(0), sz_ - 1);              \
        A = imageLoad(COVI, ivec2(lo_.x, lo_.y)).xyz;                                                                \
        B = imageLoad(COVI, ivec2(hi_.x, lo_.y)).xyz;                                                                \
        C = imageLoad(COVI, ivec2(lo_.x, hi_.y)).xyz;                                                                \
        D = imageLoad(COVI, ivec2(hi_.x, hi_.y)).xyz;                                                                \
    }

// f = (fx, fy). Returns the inverse (qxx, qxy, qyy).
vec3 rxCovInverse(vec3 a, vec3 b, vec3 c, vec3 d, vec2 f) {
    vec3 t = a + f.x * (b - a);
    vec3 u = c + f.x * (d - c);
    vec3 cv = t + f.y * (u - t);
    float det = cv.x * cv.z - cv.y * cv.y;
    bool good = !isnan(det) && !isinf(det) && abs(det) > 1e-10;
    float idet = good ? 1.0 / det : 0.0;
    return vec3(good ? idet * cv.z : 1.0, good ? -idet * cv.y : 0.0, good ? idet * cv.x : 1.0);
}

// Colour of the CFA site q: 0 R, 1 G, 2 B.
int rxChannel(ivec2 q, int cfa) {
    int px = (q.x ^ ((cfa == 1 || cfa == 3) ? 1 : 0)) & 1;
    int py = (q.y ^ ((cfa == 2 || cfa == 3) ? 1 : 0)) & 1;
    return (py == 0 && px == 0) ? 0 : ((py == 1 && px == 1) ? 2 : 1);
}

// ---- float-float (double-single) arithmetic ---------------------------------------------------------------------------
// The reference sums tile costs in float64; block-match and candidate winners are decided by strict '<' on those sums, so
// ties and near-ties must be resolved from (almost) exactly rounded sums. Values are (hi, lo) with hi = fl(hi + lo).
void rxFfAdd(inout float hi, inout float lo, float xh, float xl) {
    precise float s = hi + xh;
    precise float bb = s - hi;
    precise float err = (hi - (s - bb)) + (xh - bb);
    precise float t = lo + err + xl;
    precise float z = s + t;
    lo = t - (z - s);
    hi = z;
}
// a - b exactly as (hi, lo)
void rxFfDiff(float a, float b, out float hi, out float lo) {
    precise float d = a - b;
    precise float bb = d - a;
    precise float e = (a - (d - bb)) + (-b - bb);
    hi = d;
    lo = e;
}
// |e| and e^2 of an exact difference (hi, lo), as (hi, lo)
void rxFfAbs(float dh, float dl, out float th, out float tl) {
    th = abs(dh);
    tl = dh < 0.0 ? -dl : dl;
}
void rxFfSquare(float dh, float dl, out float th, out float tl) {
    precise float p = dh * dh;
    th = p;
    tl = fma(dh, dh, -p) + 2.0 * dh * dl;
}
bool rxFfLess(float ah, float al, float bh, float bl) { return ah < bh || (ah == bh && al < bl); }
