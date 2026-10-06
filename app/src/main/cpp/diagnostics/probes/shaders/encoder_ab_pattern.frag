#version 450
// Encoder A/B test content: a slowly panning scene with soft gradients,
// fine edges and per-frame grain, so the encoder sees camera-like work.
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(push_constant) uniform Pattern {
    uint frame;
    float width;
    float height;
} pc;

uint hash(uvec3 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return v.x;
}

void main() {
    float t = float(pc.frame) / 30.0;
    vec2 q = uv * vec2(pc.width / pc.height, 1.0) + vec2(0.08 * t, 0.03 * t);
    vec3 base = 0.5 + 0.35 * vec3(sin(q.x * 3.1 + 0.5), sin(q.y * 2.3 + 1.7 + 0.4 * t), sin((q.x + q.y) * 1.7 + 2.9));
    float stripes = step(0.5, fract(q.x * 24.0 + 0.3 * sin(q.y * 6.0)));
    float checker = step(0.5, fract(q.y * 40.0));
    float region = smoothstep(0.45, 0.55, fract(q.x * 1.3 + q.y * 0.7));
    vec3 c = mix(base, base * mix(0.6, 1.0, stripes * checker), region);
    float grain = float(hash(uvec3(uvec2(gl_FragCoord.xy), pc.frame)) & 0xffffu) / 65535.0 - 0.5;
    color = vec4(clamp(c + grain * 0.06, 0.0, 1.0), 1.0);
}
