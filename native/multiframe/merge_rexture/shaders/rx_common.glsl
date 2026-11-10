// Rexture shared helpers. Reference: tmp-free summary of the algorithm lives in merge_rexture/README.md.
// CFA codes: 0 RGGB, 1 GRBG, 2 GBRG, 3 BGGR (same as RawMergeWronskiGpu CfaPattern).

// Offsets (x, y) of the red site inside a 2x2 quad.
ivec2 rxRedOffset(int cfa) { return ivec2((cfa == 1 || cfa == 3) ? 1 : 0, (cfa == 2 || cfa == 3) ? 1 : 0); }

// Normalised value of a raw sample at (already clamped) site q. Coordinates are clamped to the image first and the
// phase of the clamped site is used: identical to normalising the whole image and edge-padding it.
float rxNormFromRaw(float rawValue, ivec2 qClamped, vec4 black, float white) {
    int phase = (qClamped.y & 1) * 2 + (qClamped.x & 1);
    float b = black[phase];
    float den = max(1.0, white - b);
    return (rawValue - b) / den;
}
