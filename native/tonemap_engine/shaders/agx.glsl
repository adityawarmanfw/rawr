// AgX render transform, ported from darktable src/iop/agx.c (GPL-3.0-or-later).
// Coefficients come from tonemap::compileAgxCurve()/compileAgxMatrices();
// agxRender() in Agx.h is the CPU reference. Requires the TonemapGpuParams
// block P. No loops: per-channel work is spelled out (Adreno, see repo notes).

const float AGX_EPSILON = 1e-6;

float agxSigmoid(float x, float power) { return x / pow(1.0 + pow(x, power), 1.0 / power); }

float agxCurve(float x) {
    float r;
    if (x < P.agxToe.y) {
        if (P.agxFlags.x != 0u)
            r = P.agxLine.z + max(0.0, P.agxFallback.x * pow(max(x, 0.0), P.agxFallback.y));
        else
            r = P.agxToe.w * agxSigmoid(P.agxLine.x * (x - P.agxToe.y) / P.agxToe.w, P.agxToe.x) + P.agxToe.z;
    } else if (x <= P.agxShoulder.y) {
        r = P.agxLine.x * x + P.agxLine.y;
    } else {
        if (P.agxFlags.y != 0u)
            r = x >= 1.0 ? P.agxLine.w : P.agxLine.w - max(0.0, P.agxFallback.z * pow(1.0 - x, P.agxFallback.w));
        else
            r = P.agxShoulder.w * agxSigmoid(P.agxLine.x * (x - P.agxShoulder.y) / P.agxShoulder.w, P.agxShoulder.x) +
                P.agxShoulder.z;
    }
    return clamp(r, P.agxLine.z, P.agxLine.w);
}

float agxToneChannel(float v) {
    float relative = max(AGX_EPSILON, v / 0.18);
    return agxCurve(clamp((log2(relative) - P.agxLog.x) / P.agxLog.y, 0.0, 1.0));
}

// Blender luminance_compenstation_bt2020.py, via darktable.
const vec3 AGX_REC2020_LUMA = vec3(0.2658180370250449, 0.59846986045365, 0.1357121025213052);
float agxOpponentLuma(vec3 c) {
    vec3 o = vec3(max(c.r, max(c.g, c.b))) - c;
    return max(o.r, max(o.g, o.b)) - dot(o, AGX_REC2020_LUMA);
}
vec3 agxCompressIntoGamut(vec3 c) {
    float target = agxOpponentLuma(c) + dot(c, AGX_REC2020_LUMA);
    c += vec3(max(-min(c.r, min(c.g, c.b)), 0.0));
    float compensated = agxOpponentLuma(c) + dot(c, AGX_REC2020_LUMA);
    return c * (compensated > target && compensated > AGX_EPSILON ? target / compensated : 1.0);
}

vec3 agxRgbToHsv(vec3 c) {
    float mn = min(c.r, min(c.g, c.b)), mx = max(c.r, max(c.g, c.b)), d = mx - mn;
    if (!(abs(mx) > 1e-6 && abs(d) > 1e-6)) return vec3(0.0, 0.0, mx);
    float h = c.r == mx ? (c.g - c.b) / d : c.g == mx ? 2.0 + (c.b - c.r) / d : 4.0 + (c.r - c.g) / d;
    return vec3(fract(h / 6.0), d / mx, mx);
}
vec3 agxHsvToRgb(vec3 hsv) {
    float C = hsv.y * hsv.z, m = hsv.z - C, h = hsv.x * 6.0, i = floor(h), fc = (h - i) * C;
    float top = C + m, inc = fc + m, dec = top - fc;
    if (i < 1.0) return vec3(top, inc, m);
    if (i < 2.0) return vec3(dec, top, m);
    if (i < 3.0) return vec3(m, top, inc);
    if (i < 4.0) return vec3(m, dec, top);
    if (i < 5.0) return vec3(inc, m, top);
    return vec3(top, m, dec);
}

// Linear AP1 after exposure -> display-linear sRGB/Rec.709, unclamped.
vec3 agxRender(vec3 ap1) {
    ap1 = clamp(mix(ap1, vec3(0.0), isnan(ap1)), vec3(-1e6), vec3(1e6));
    vec3 rgb = mat3(P.agxBaseToRendering) * agxCompressIntoGamut(mat3(P.agxAp1ToBase) * ap1);
    bool restoreHue = P.agxFlags.w != 0u;
    float hueBefore = restoreHue ? agxRgbToHsv(rgb).x : 0.0;
    vec3 c = vec3(agxToneChannel(rgb.r), agxToneChannel(rgb.g), agxToneChannel(rgb.b));
    if (P.agxFlags.z != 0u) {
        float m = P.agxLook.y / (1.0 + P.agxLook.x), b = P.agxLook.x * m;
        vec3 s = m * c + vec3(b);
        c = mix(s, pow(max(s, vec3(AGX_EPSILON)), vec3(P.agxLook.z)), greaterThan(s, vec3(0.0)));
        float y = dot(c, P.agxLookLuma.xyz);
        c = vec3(y) + P.agxLook.w * (c - vec3(y));
    }
    c = pow(max(c, vec3(0.0)), vec3(P.agxLog.z));
    if (restoreHue) {
        vec3 hsv = agxRgbToHsv(c);
        // remainder(x, 1): signed shortest distance on the hue circle.
        float d = hsv.x - hueBefore;
        d -= floor(d + 0.5);
        hsv.x = fract(hueBefore + (1.0 - P.agxLog.w) * d);
        c = agxHsvToRgb(hsv);
    }
    return mat3(P.agxRenderingToSrgb) * c;
}
