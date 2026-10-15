#ifndef RAWR_ADAPTIVE_TONE_GLSL
#define RAWR_ADAPTIVE_TONE_GLSL
// Mirrors tonemap/AdaptiveTone.h. p = lift EV, key EV, highlight EV, and
// (vec4 form) the lift per EV of shoulder width; outside [0.1, 0.4] it is the
// default 0.25. Peak lift 1.5 EV below the key, toe 5 EV wide, lift <= limit x
// shoulder width so the mapping stays monotonic and keeps upper-midtone contrast.
float rawrAdaptiveToneGain(float y, vec4 p) {
    float peak = p.y - 1.5;
    float span = p.z - peak;
    if (!(p.x > 0.0) || !(y > 0.0) || !(span > 0.0)) return 1.0;
    float ev = log2(y);
    if (ev >= p.z) return 1.0;
    float perEv = p.w >= 0.1 ? min(p.w, 0.4) : 0.25;
    float lift = min(p.x, perEv * span);
    float toe = smoothstep(peak - 5.0, peak, ev);
    float shoulder = 1.0 - smoothstep(peak, p.z, ev);
    return exp2(lift * toe * shoulder);
}
float rawrAdaptiveToneGain(float y, vec3 p) { return rawrAdaptiveToneGain(y, vec4(p, 0.25)); }
#endif
