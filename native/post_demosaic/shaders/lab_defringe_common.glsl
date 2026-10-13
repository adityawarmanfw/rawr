// Per-frame camera calibration, carried in the guaranteed 128-byte push range.
layout(push_constant) uniform Pc {
    uint width;
    uint height;
    float strength;
    float threshold;
    float edgeMode;
    uint pad0;
    uint pad1;
    uint pad2;
    vec4 forwardRows[3];
    vec4 inverseRows[3];
}
pc;
const vec3 white = vec3(.96422, 1., .82521);
mat3 cameraToXyzMatrix() {
    return transpose(mat3(pc.forwardRows[0].xyz, pc.forwardRows[1].xyz, pc.forwardRows[2].xyz));
}
mat3 xyzToCameraMatrix() {
    return transpose(mat3(pc.inverseRows[0].xyz, pc.inverseRows[1].xyz, pc.inverseRows[2].xyz));
}
#define cameraToXyz cameraToXyzMatrix()
#define xyzToCamera xyzToCameraMatrix()
float labf(float t) { return t > 216. / 24389. ? pow(t, 1. / 3.) : (24389. / 27. * t + 16.) / 116.; }
float labinv(float t) { return t > 6. / 29. ? t * t * t : 3. * (6. / 29.) * (6. / 29.) * (t - 4. / 29.); }
vec3 toLab(vec3 rgb) {
    vec3 v = cameraToXyz * rgb / white;
    vec3 f = vec3(labf(v.x), labf(v.y), labf(v.z));
    return vec3(116. * f.y - 16., 500. * (f.x - f.y), 200. * (f.y - f.z));
}
vec3 fromLab(vec3 v) {
    float fy = (v.x + 16.) / 116.;
    vec3 f = vec3(fy + v.y / 500., fy, fy - v.z / 200.);
    return xyzToCamera * (white * vec3(labinv(f.x), labinv(f.y), labinv(f.z)));
}
float labY(float L) { return labinv((L + 16.) / 116.); }
// RawTherapee 5.13 Color::huelab_to_huehsv2: Lab hue (radians) to its
// approximate HSV hue in 0..1.
float huelabToHuehsv2(float h) {
    float hr = 0.;
    if (h >= 0. && h < .6) hr = .11666 * h + .93;
    else if (h >= .6 && h < 1.4) hr = .1125 * h - .0675;
    else if (h >= 1.4 && h < 2.) hr = .2666 * h - .2833;
    else if (h >= 2.) hr = .1489 * h - .04785;
    else if (h < -2.8) hr = .23419 * h + 1.1557;
    else if (h < -2.3) hr = .16 * h + .948;
    else if (h < -.9) hr = .12143 * h + .85928;
    else if (h < -.1) hr = .2125 * h + .94125;
    else hr = .1 * h + .93;
    return hr < 0. ? hr + 1. : (hr > 1. ? hr - 1. : hr);
}
// One half of a FlatCurve span with both tangents 0.35: a quadratic Bezier
// from the start point (u = 0) to the span midpoint (u = 0.5), returned as
// the fraction of the span's rise reached at u.
float flatCurveHalf(float u) {
    float t = (.7 - sqrt(max(.49 - .8 * u, 0.))) / .4;
    return .5 * t * t;
}
// RawTherapee's default defringe hue curve (PF_correct_RT chCurve): zero
// everywhere except one bump rising from HSV hue 0.669 to 0.978 at 0.829
// (purple-magenta) and back to zero at 0.991. Returns its score factor
// (1 + c)^2 with c = curve - 0.5, doubled below zero: 0 off the bump.
float hueWeight(vec2 ab) {
    const float x0 = .668944571, xPeak = .8287775246, x1 = .9908883827, yPeak = .97835991;
    // atan2(0, 0) is 0 in RawTherapee; GLSL leaves it undefined.
    float hr = huelabToHuehsv2(any(notEqual(ab, vec2(0.))) ? atan(ab.y, ab.x) : 0.);
    float y = 0.;
    if (hr > x0 && hr < xPeak) {
        float u = (hr - x0) / (xPeak - x0);
        y = yPeak * (u <= .5 ? flatCurveHalf(u) : 1. - flatCurveHalf(1. - u));
    } else if (hr >= xPeak && hr < x1) {
        float u = (hr - xPeak) / (x1 - xPeak);
        y = yPeak * (u <= .5 ? 1. - flatCurveHalf(u) : flatCurveHalf(1. - u));
    }
    float c = y - .5;
    c = c < 0. ? 2. * c : c;
    return (1. + c) * (1. + c);
}
// Purple chroma: Lab chroma inside the defringe hue range (curve factor
// capped at 1), zero elsewhere.
float purpleChroma(vec2 ab) { return min(hueWeight(ab), 1.) * length(ab); }
// Share of a 17x17 window at least half as purple as the pixel: fringes
// (1-3 px lines) stay below the low end, edges of purple objects >= ~12 px
// across reach the high end (tuned on twigs vs synthetic flowers on sky).
const float kPurpleObjectLo = .38, kPurpleObjectHi = .48;
