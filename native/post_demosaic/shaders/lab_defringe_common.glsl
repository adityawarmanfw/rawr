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
// Broad blue-purple selection, tapering into cyan and red. Independent prototype hue policy.
float hueWeight(vec2 ab) {
    float c = length(ab);
    float align = dot(ab / max(c, 1e-5), normalize(vec2(1., -1.)));
    return smoothstep(.25, .85, align) * smoothstep(1., 4., c);
}
