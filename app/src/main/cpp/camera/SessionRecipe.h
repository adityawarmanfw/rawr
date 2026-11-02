#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "camera/CameraQuirks.h"
#include "camera/CameraRouting.h"

namespace rawrcam::camera {

// How a full-size still is taken. Derived from the lens, so a profile states what it needs (a
// capture stream, decision-scope keys, stop-repeating) and the controller reads one answer.
enum class StillStrategy : uint8_t {
    Direct,               // the still goes out beside the running viewfinder
    DecisionFrame,        // a tagged viewfinder-rate frame lets the HAL decide full size, then the viewfinder pauses
    StopRepeatThenStill,  // the viewfinder pauses, the still goes out, the viewfinder is restored
};

[[nodiscard]] constexpr const char* stillStrategyName(StillStrategy s) noexcept {
    switch (s) {
        case StillStrategy::Direct: return "direct";
        case StillStrategy::DecisionFrame: return "decisionFrame";
        case StillStrategy::StopRepeatThenStill: return "stopRepeatThenStill";
    }
    return "direct";
}

[[nodiscard]] constexpr const char* gateHoldName(GateHold hold) noexcept {
    return hold == GateHold::UntilClosed ? "closed" : "configured";
}

// The companion output as the session builds it.
struct CompanionRecipe {
    bool privateFormat = false;
    std::optional<std::pair<uint32_t, uint32_t>> size;  // nullopt: the RAW-aspect pick from the advertised sizes
    bool targeted = true;
    std::optional<uint64_t> usage;    // absent: PRIVATE asks for GPU sampling, YUV for the reader default
    uint32_t maxImages = 2;
    std::optional<bool> physical;     // absent: tied to the route's physical camera, if it has one
    CompanionFallback fallback = CompanionFallback::Base;
    // Dropped from the repeating request after the first accepted frame.
    bool detach = true;
    // Set by the profile: part of the sensor mode, never a recovery option.
    bool required = false;
    // Why the companion exists: "profile", "capture_stream", "override" or "rule".
    const char* source = "rule";

    bool operator==(const CompanionRecipe& o) const {
        return privateFormat == o.privateFormat && size == o.size && targeted == o.targeted && usage == o.usage &&
               maxImages == o.maxImages && physical == o.physical && fallback == o.fallback && detach == o.detach &&
               required == o.required && std::string(source) == o.source;
    }
};

// What the controller decided about the session's structure for one start, in one place.
struct SessionRecipe {
    bool repeatingStillTemplate = false;  // the viewfinder request uses the still-capture template
    bool captureStillTemplate = true;     // full-size stills are built from the still-capture template
    std::optional<CompanionRecipe> companion;
    std::optional<std::pair<uint32_t, uint32_t>> auxJpeg;
    std::optional<uint64_t> auxJpegUsage;
    StillStrategy still = StillStrategy::Direct;
    std::string gate;
    GateHold gateHold = GateHold::UntilConfigured;
    int32_t startupDelayMs = 0;
    SessionParamsPolicy sessionParams = SessionParamsPolicy::Cadence;
    RequestDefaults requestDefaults = RequestDefaults::Rawr;
};

// Per-start facts the lens itself cannot know.
struct RecipeInputs {
    bool forcedCompanion = false;     // debug.rawr.companion_stream=1
    bool automaticCompanion = false;  // the unverified-device rule, after the start ladder and fallbacks
};

[[nodiscard]] inline GateHold defaultGateHold(const LensRoute& lens) noexcept {
    // A capture-stream route keeps the gate until the device closes: the HAL rebuilds its still
    // pipeline per full-size request and crashes if the client is no longer the vendor's camera.
    return lens.captureStream ? GateHold::UntilClosed : GateHold::UntilConfigured;
}

[[nodiscard]] inline StillStrategy stillStrategyFor(const LensRoute& lens, bool companionConfigured) noexcept {
    bool decisionKeys = false;
    for (const auto& k : lens.keys) decisionKeys |= k.enabled && k.scope == CameraKeySetting::Scope::Decision;
    if (lens.captureStream && companionConfigured && lens.companionTargeted && decisionKeys)
        return StillStrategy::DecisionFrame;
    return lens.stopRepeatingForStill ? StillStrategy::StopRepeatThenStill : StillStrategy::Direct;
}

[[nodiscard]] inline SessionRecipe resolveSessionRecipe(const LensRoute& lens, const RecipeInputs& in) {
    SessionRecipe r;
    r.repeatingStillTemplate = lens.stillTemplate.value_or(false);
    r.captureStillTemplate = lens.stillTemplate.value_or(true);
    if (companionStreamEnabled(lens, in.forcedCompanion, in.automaticCompanion)) {
        CompanionRecipe c;
        c.privateFormat = lens.companionPrivate;
        c.size = lens.companionSize;
        c.targeted = lens.companionTargeted;
        c.usage = lens.companionUsage;
        c.maxImages = lens.companionMaxImages ? lens.companionMaxImages : 2;
        c.physical = lens.companionPhysical;
        c.fallback = lens.companionFallback;
        // Only an automatic companion is dropped unless the profile says otherwise.
        c.detach = lens.companionDetach.value_or(!lens.companionSize.has_value());
        c.required = lens.companionSize.has_value();
        c.source = lens.companionSize ? "profile" : lens.captureStream ? "capture_stream" : in.forcedCompanion ? "override" : "rule";
        r.companion = c;
    }
    r.auxJpeg = lens.auxJpegSize;
    r.auxJpegUsage = lens.auxJpegUsage;
    r.still = stillStrategyFor(lens, r.companion.has_value());
    r.gate = lens.vendorGate;
    r.gateHold = lens.gateHold.value_or(defaultGateHold(lens));
    r.startupDelayMs = lens.startupDelayMs;
    r.sessionParams = lens.sessionParams;
    r.requestDefaults = lens.requestDefaults;
    return r;
}

}  // namespace rawrcam::camera
