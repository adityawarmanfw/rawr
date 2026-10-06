// Single-pixel MHC-style demosaic shared by the tiled recording stages.
// The includer defines VIDEO_MHC_AT(q), returning the normalized RAW sample at
// absolute sensor position q from its shared-memory tile, and videoChannelAt.
#ifndef VIDEO_MHC_AT
#error "define VIDEO_MHC_AT(q) before including video_mhc.glsl"
#endif

vec3 videoFullRgb(ivec2 p) {
    int c = videoChannelAt(p);
    float center = VIDEO_MHC_AT(p);
    float h1 = VIDEO_MHC_AT(p + ivec2(-1, 0)) + VIDEO_MHC_AT(p + ivec2(1, 0));
    float v1 = VIDEO_MHC_AT(p + ivec2(0, -1)) + VIDEO_MHC_AT(p + ivec2(0, 1));
    float h2 = VIDEO_MHC_AT(p + ivec2(-2, 0)) + VIDEO_MHC_AT(p + ivec2(2, 0));
    float v2 = VIDEO_MHC_AT(p + ivec2(0, -2)) + VIDEO_MHC_AT(p + ivec2(0, 2));
    float diagonals = VIDEO_MHC_AT(p + ivec2(-1, -1)) + VIDEO_MHC_AT(p + ivec2(1, -1)) +
                      VIDEO_MHC_AT(p + ivec2(-1, 1)) + VIDEO_MHC_AT(p + ivec2(1, 1));
    // Green at R/B sites is gradient-directed (Hamilton-Adams style): MHC's
    // isotropic green is exactly the mean of gh and gv below, and the
    // along-edge estimate wins. The Laplacian deviation from the local mean
    // is limited to half the local range instead of hard-clamped to the
    // neighbour min/max, which clipped asymmetrically per Bayer phase and
    // left a dotted zipper on high-contrast edges.
    if (c == 0 || c == 3) {
        float left = VIDEO_MHC_AT(p + ivec2(-1, 0)), right = VIDEO_MHC_AT(p + ivec2(1, 0));
        float up = VIDEO_MHC_AT(p + ivec2(0, -1)), down = VIDEO_MHC_AT(p + ivec2(0, 1));
        float gh = 0.5 * (h1) + 0.25 * (2.0 * center - h2);
        float gv = 0.5 * (v1) + 0.25 * (2.0 * center - v2);
        float dh = abs(left - right) + abs(2.0 * center - h2);
        float dv = abs(up - down) + abs(2.0 * center - v2);
        float wsum = dh + dv;
        float gIso = wsum > 1e-6 ? (dv * gh + dh * gv) / wsum : 0.5 * (gh + gv);
        float gMean = 0.25 * (h1 + v1);
        float gAllow = 0.5 * (max(max(left, right), max(up, down)) -
                              min(min(left, right), min(up, down))) + 1e-4;
        float g = gMean + clamp(gIso - gMean, -gAllow, gAllow);
        float oppIso = (6.0 * center + 2.0 * diagonals - 1.5 * (h2 + v2)) * 0.125;
        float oppMean = 0.25 * diagonals;
        float d00 = VIDEO_MHC_AT(p + ivec2(-1, -1)), d10 = VIDEO_MHC_AT(p + ivec2(1, -1));
        float d01 = VIDEO_MHC_AT(p + ivec2(-1, 1)), d11 = VIDEO_MHC_AT(p + ivec2(1, 1));
        float oppAllow = 0.5 * (max(max(d00, d10), max(d01, d11)) -
                                min(min(d00, d10), min(d01, d11))) + 1e-4;
        float opposite = oppMean + clamp(oppIso - oppMean, -oppAllow, oppAllow);
        return max(c == 0 ? vec3(center, g, opposite) : vec3(opposite, g, center), vec3(0.0));
    }
    float horIso = (5.0 * center + 4.0 * h1 - diagonals - h2 + 0.5 * v2) * 0.125;
    float verIso = (5.0 * center + 4.0 * v1 - diagonals - v2 + 0.5 * h2) * 0.125;
    float left = VIDEO_MHC_AT(p + ivec2(-1, 0)), right = VIDEO_MHC_AT(p + ivec2(1, 0));
    float up = VIDEO_MHC_AT(p + ivec2(0, -1)), down = VIDEO_MHC_AT(p + ivec2(0, 1));
    float horMean = 0.5 * (left + right);
    float verMean = 0.5 * (up + down);
    float horizontal = horMean + clamp(horIso - horMean, -0.5 * abs(left - right) - 1e-4,
                                       0.5 * abs(left - right) + 1e-4);
    float vertical = verMean + clamp(verIso - verMean, -0.5 * abs(up - down) - 1e-4,
                                     0.5 * abs(up - down) + 1e-4);
    bool redHorizontal = videoChannelAt(p + ivec2(1, 0)) == 0;
    return max(redHorizontal ? vec3(horizontal, center, vertical)
                             : vec3(vertical, center, horizontal), vec3(0.0));
}
