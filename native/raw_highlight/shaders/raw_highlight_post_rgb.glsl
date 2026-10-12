// Sensor-informed highlight handling in white-balanced camera RGB.
//
// Clipping is classified before white balance. The caller also supplies a
// chromaticity guide propagated from nearby bright, fully valid sensor pixels.
// Safety is unconditional; `recoverDetail` controls only whether surviving
// channels modulate the propagated colour to reconstruct intensity texture.

bool rawrHighlightFinite(float x) { return !isnan(x) && !isinf(x); }
bool rawrHighlightFinite3(vec3 x) {
    return rawrHighlightFinite(x.r) && rawrHighlightFinite(x.g) && rawrHighlightFinite(x.b);
}
vec3 rawrHighlightSanitize(vec3 x) { return rawrHighlightFinite3(x) ? clamp(x, vec3(0.0), vec3(60000.0)) : vec3(0.0); }
float rawrHighlightMaximum(vec3 x) { return max(x.r, max(x.g, x.b)); }
float rawrHighlightSum(vec3 x) { return x.r + x.g + x.b; }

vec3 rawrHighlightRepairWeights(vec3 center, vec3 channelCeilings) {
    vec3 relative = max(center, vec3(0.0)) / max(channelCeilings, vec3(1e-4));
    return smoothstep(vec3(0.85), vec3(1.0), relative);
}

vec3 rawrHighlightNormalizeGuide(vec3 guide, out bool valid) {
    guide = rawrHighlightSanitize(guide);
    float sum = rawrHighlightSum(guide);
    valid = sum > 1e-6 && rawrHighlightMaximum(guide) < 0.9999;
    return valid ? guide / sum : vec3(1.0 / 3.0);
}

// ON: continuously fit the propagated chromaticity to channels which still
// carry sensor detail. Only near-clipped channels are raised; reliable channels
// are never replaced. This preserves texture from the last surviving channel
// and removes the old whole-RGB mask-shaped replacement. When all three
// channels lose reliability there is no recoverable texture, so converge to the
// bright neutral fallback rather than preserving invalid clipped chromaticity.
// coreFraction: share of the neighbourhood whose every channel is clipped
// (1 = deep inside a light source, 0 = at its edge). Fully clipped pixels
// fade from the propagated guide colour at the clip edge to the neutral core,
// so a light keeps a soft coloured rim instead of a one-pixel white contour.
vec3 rawrHighlightRecoverDetailWithRepair(vec3 center, vec3 channelCeilings, vec3 guide, bool guideValid, vec3 repair,
                                          float coreFraction) {
    vec3 ceilings = max(channelCeilings, vec3(1e-4));
    float repairStrength = rawrHighlightMaximum(repair);
    if (repairStrength <= 1e-6) return center;

    float commonCeiling = min(ceilings.r, min(ceilings.g, ceilings.b));
    // With no surviving channel there is no texture to infer. Preserve the
    // measured highlight magnitude but discard its invalid chromaticity so the
    // terminal core renders bright and neutral instead of as a grey patch.
    vec3 neutralFallback = vec3(max(commonCeiling, rawrHighlightMaximum(center)));
    if (!guideValid) return mix(center, neutralFallback, repairStrength);

    vec3 reliability = vec3(1.0) - repair;
    float denominator = dot(guide * guide, reliability);
    float reliableStrength = rawrHighlightMaximum(reliability);
    if (denominator <= 1e-8 || reliableStrength <= 1e-6) {
        // No surviving channel: colour from the guide at the core magnitude,
        // fading to neutral with depth into the fully clipped region.
        vec3 guided = guide / max(rawrHighlightMaximum(guide), 1e-6) * neutralFallback.r;
        return mix(guided, neutralFallback, smoothstep(0.15, 0.85, coreFraction));
    }

    float fitScale = dot(center * guide, reliability) / denominator;
    // Only chromaticity is spatially propagated. The surviving channels at
    // this pixel determine intensity: mixing a neighborhood peak into this
    // scale changes only repaired channels, causing magenta/green rims at
    // brightness edges even when the guide describes a neutral highlight.
    vec3 target = max(center, guide * max(fitScale, 0.0));
    vec3 recovered = mix(center, target, repair);

    // Fade continuously to the non-hallucinated fallback as the final usable
    // channel approaches clipping; no binary clip contour controls this blend.
    float fitConfidence = smoothstep(0.0, 0.25, reliableStrength);
    return mix(neutralFallback, recovered, fitConfidence);
}

// Legacy callers without a sensor clip image retain the numeric ceiling path.
vec3 rawrHighlightRecoverDetail(vec3 center, vec3 channelCeilings, vec3 guide, bool guideValid) {
    return rawrHighlightRecoverDetailWithRepair(center, channelCeilings, guide, guideValid,
                                                rawrHighlightRepairWeights(center, channelCeilings), 1.0);
}

vec3 reconstructPostWbHighlight(vec3 inputRgb, vec3 channelCeilings, vec3 propagatedGuide, bool propagatedGuideValid,
                                bool recoverDetail) {
    vec3 center = rawrHighlightSanitize(inputRgb);
    vec3 ceilings = max(rawrHighlightSanitize(channelCeilings), vec3(1e-4));
    if (!recoverDetail) {
        // HL OFF performs no reconstruction. Collapse the unequal post-WB
        // channel headroom to the lowest channel ceiling, then let the normal
        // colour transform and tonemapper render that clipped camera RGB.
        float commonCeiling = min(ceilings.r, min(ceilings.g, ceilings.b));
        return min(center, vec3(commonCeiling));
    }

    if (rawrHighlightMaximum(ceilings) <= 0.0) return center;

    bool normalizedValid;
    vec3 guide = rawrHighlightNormalizeGuide(propagatedGuide, normalizedValid);
    bool guideValid = propagatedGuideValid && normalizedValid;
    return rawrHighlightSanitize(rawrHighlightRecoverDetail(center, ceilings, guide, guideValid));
}
