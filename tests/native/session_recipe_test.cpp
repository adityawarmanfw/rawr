#include <cassert>
#include <iostream>
#include <string>

#include "camera/CameraProfileJson.h"
#include "camera/CameraRouting.h"
#include "camera/SessionRecipe.h"

using namespace rawrcam::camera;

namespace {

std::vector<LensRoute> allLenses() {
    std::vector<LensRoute> lenses;
    for (const auto& p : builtInProfiles()) lenses.insert(lenses.end(), p.profile.lenses.begin(), p.profile.lenses.end());
    LensRoute plain;
    LensRoute capture;
    capture.photoStream = rawrcam::geometry::RawStreamPreference{};
    LensRoute sized;
    sized.companionSize = std::make_pair(640u, 480u);
    LensRoute off;
    off.disableCompanion = true;
    LensRoute stop;
    stop.stopViewfinderForPhotos = true;
    lenses.insert(lenses.end(), {plain, capture, sized, off, stop});
    return lenses;
}

// The companion decision the controller made before the recipe existed.
void testCompanionMatchesTheOldDecision() {
    for (const auto& lens : allLenses()) {
        for (const bool forced : {false, true}) {
            for (const bool automatic : {false, true}) {
                const auto recipe = resolveSessionRecipe(lens, RecipeInputs{forced, automatic});
                assert(recipe.companion.has_value() == companionStreamEnabled(lens, forced, automatic));
                if (!recipe.companion) continue;
                assert(recipe.companion->size == lens.companionSize);
                assert(recipe.companion->privateFormat == lens.companionPrivate);
                assert(recipe.companion->targeted == lens.companionTargeted);
                // Only a profile-set companion is exempt from recovery and from detaching.
                assert(recipe.companion->required == lens.companionSize.has_value());
            }
        }
    }
}

void testCompanionSource() {
    LensRoute plain;
    assert(!resolveSessionRecipe(plain, {false, false}).companion);
    assert(std::string(resolveSessionRecipe(plain, {false, true}).companion->source) == "rule");
    assert(std::string(resolveSessionRecipe(plain, {true, false}).companion->source) == "override");
    LensRoute capture;
    capture.photoStream = rawrcam::geometry::RawStreamPreference{};
    assert(std::string(resolveSessionRecipe(capture, {false, false}).companion->source) == "capture_stream");
    LensRoute sized;
    sized.photoStream = rawrcam::geometry::RawStreamPreference{};
    sized.companionSize = std::make_pair(640u, 480u);
    assert(std::string(resolveSessionRecipe(sized, {true, true}).companion->source) == "profile");
    capture.disableCompanion = true;
    assert(!resolveSessionRecipe(capture, {true, true}).companion);
}

void testTemplatesAndStructure() {
    for (const auto& lens : allLenses()) {
        const auto recipe = resolveSessionRecipe(lens, {});
        assert(recipe.repeatingStillTemplate == lens.stillTemplate.value_or(false));
        assert(recipe.captureStillTemplate == lens.stillTemplate.value_or(true));
        assert(recipe.auxJpeg == lens.auxJpegSize);
        assert(recipe.startupDelayMs == lens.startupDelayMs);
        assert(recipe.gate == lens.vendorGate);
    }
}

// The gate was released after configuration unless a capture window existed, which it does
// exactly on capture-stream routes.
void testGateHoldKeepsTheOldRule() {
    for (const auto& lens : allLenses()) {
        const auto recipe = resolveSessionRecipe(lens, {});
        assert((recipe.gateHold == GateHold::UntilClosed) == lens.photoStream.has_value());
    }
    LensRoute lens;
    lens.vendorGate = "g";
    lens.photoStream = rawrcam::geometry::RawStreamPreference{};
    lens.gateHold = GateHold::UntilConfigured;
    assert(resolveSessionRecipe(lens, {}).gateHold == GateHold::UntilConfigured);
}

void testVivoStillStrategy() {
    size_t decision = 0, captureLenses = 0;
    for (const auto& p : builtInProfiles()) {
        for (const auto& lens : p.profile.lenses) {
            bool decisionKey = false;
            for (const auto& k : lens.vendorKeys) decisionKey |= k.enabled && k.scope == CameraKeySetting::Scope::Decision;
            const auto recipe = resolveSessionRecipe(lens, {false, false});
            if (lens.photoStream) {
                ++captureLenses;
                // A built-in full-size lens with decision keys and a companion decides, as it did.
                assert((recipe.still == StillStrategy::DecisionFrame) ==
                       (decisionKey && recipe.companion && lens.companionTargeted));
                assert(recipe.gateHold == GateHold::UntilClosed || lens.vendorGate.empty());
            } else {
                assert(recipe.still == (lens.stopViewfinderForPhotos ? StillStrategy::StopRepeatThenStill : StillStrategy::Direct));
            }
            decision += recipe.still == StillStrategy::DecisionFrame;
        }
    }
    assert(captureLenses >= 3);
    assert(decision >= 1);  // the vivo full-size lenses do decide
    LensRoute stop;
    stop.stopViewfinderForPhotos = true;
    assert(resolveSessionRecipe(stop, {}).still == StillStrategy::StopRepeatThenStill);
    assert(resolveSessionRecipe(LensRoute{}, {}).still == StillStrategy::Direct);
    assert(std::string(stillStrategyName(StillStrategy::DecisionFrame)) == "decisionFrame");
    assert(std::string(gateHoldName(GateHold::UntilClosed)) == "closed");
}

void testGateHoldJson() {
    std::string error;
    const auto closed = parseCameraProfile(
        R"({"lenses":[{"id":"a","cameraId":"0","vendorGate":"g","gateHold":"closed"}]})", &error);
    assert(closed && closed->lenses[0].gateHold == GateHold::UntilClosed);
    const auto json = serializeCameraProfile(*closed);
    assert(json.find(R"("gate":{"name":"g","hold":"closed"})") != std::string::npos);
    const auto again = parseCameraProfile(json);
    assert(again && again->lenses[0].gateHold == GateHold::UntilClosed && serializeCameraProfile(*again) == json);
    const auto configured = parseCameraProfile(
        R"({"lenses":[{"id":"a","cameraId":"0","vendorGate":"g","gateHold":"configured"}]})");
    assert(configured && configured->lenses[0].gateHold == GateHold::UntilConfigured);
    // No field, no explicit hold.
    const auto none = parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"0","vendorGate":"g"}]})");
    assert(none && !none->lenses[0].gateHold);
    assert(serializeCameraProfile(*none).find("gateHold") == std::string::npos);
    assert(!parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"0","vendorGate":"g","gateHold":"forever"}]})", &error));
    assert(error.find("unknown gateHold") != std::string::npos);
    assert(!parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"0","gateHold":"closed"}]})", &error));
    assert(error.find("needs a vendorGate") != std::string::npos);
}

void testSessionParamsAndRequestDefaults() {
    std::string error;
    const auto lens = parseCameraProfile(
        R"({"lenses":[{"id":"a","cameraId":"2","session":{"params":"none"},"request":{"defaults":"minimal"},
            "keys":[{"tag":"vendor.mode","type":"int32","scope":"request","values":[3]}]}]})", &error);
    assert(lens);
    assert(lens->lenses[0].sessionParams == SessionParamsPolicy::None);
    assert(lens->lenses[0].requestDefaults == RequestDefaults::Minimal);
    const auto recipe = resolveSessionRecipe(lens->lenses[0], {});
    assert(recipe.sessionParams == SessionParamsPolicy::None && recipe.requestDefaults == RequestDefaults::Minimal);
    const auto json = serializeCameraProfile(*lens);
    assert(json.find(R"("session":{"params":"none"})") != std::string::npos);
    assert(json.find(R"("request":{"defaults":"minimal"})") != std::string::npos);
    const auto again = parseCameraProfile(json);
    assert(again && serializeCameraProfile(*again) == json);

    const auto full = parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"2","session":{"params":"full"}}]})");
    assert(full && full->lenses[0].sessionParams == SessionParamsPolicy::Full && carriesRecipe(full->lenses[0]));
    assert(serializeCameraProfile(*full).find(R"("params":"full")") != std::string::npos);

    // The defaults are the old behaviour and write nothing.
    const auto plain = parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"2"}]})");
    assert(plain && plain->lenses[0].sessionParams == SessionParamsPolicy::Cadence);
    assert(plain->lenses[0].requestDefaults == RequestDefaults::Rawr && !carriesRecipe(plain->lenses[0]));
    assert(serializeCameraProfile(*plain).find("\"session\"") == std::string::npos);
    assert(serializeCameraProfile(*plain).find("\"request\"") == std::string::npos);

    // A session-scope key needs the session parameters; none would drop it silently.
    assert(!parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"2","session":{"params":"none"},
        "keys":[{"tag":"vendor.mode","scope":"session","values":[3]}]}]})", &error));
    assert(error.find("conflicts with session key") != std::string::npos);
    assert(!parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"2","session":{"params":"none"},
        "keys":[{"tag":"vendor.mode","scope":"sessionAndRequest","values":[3]}]}]})", &error));
    // A disabled session key is not sent, so it does not conflict.
    assert(parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"2","session":{"params":"none"},
        "keys":[{"tag":"vendor.mode","scope":"session","values":[3],"enabled":false}]}]})"));
    assert(!parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"2","session":{"params":"sometimes"}}]})", &error));
    assert(error.find("unknown session params") != std::string::npos);
    assert(!parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"2","request":{"defaults":"bare"}}]})", &error));
    assert(error.find("unknown request defaults") != std::string::npos);
    assert(std::string(sessionParamsName(SessionParamsPolicy::Full)) == "full");
    assert(std::string(requestDefaultsName(RequestDefaults::Minimal)) == "minimal");
}

// The old rules: an automatic companion is dropped after the first frame, a profile-sized one stays;
// two images; the route decides the physical camera.
void testCompanionBlockDefaults() {
    LensRoute automatic;
    const auto a = resolveSessionRecipe(automatic, {false, true}).companion;
    assert(a && a->detach && a->maxImages == 2 && !a->usage && !a->physical && a->fallback == CompanionFallback::Base);
    LensRoute sized;
    sized.companionSize = std::make_pair(640u, 480u);
    const auto b = resolveSessionRecipe(sized, {}).companion;
    assert(b && !b->detach && b->required && b->maxImages == 2);
    LensRoute capture;
    capture.photoStream = rawrcam::geometry::RawStreamPreference{};
    assert(resolveSessionRecipe(capture, {}).companion->detach);

    LensRoute tuned = sized;
    tuned.companionDetach = true;
    tuned.companionBufferUsage = 0x100;
    tuned.companionBufferCount = 4;
    tuned.companionPhysical = false;
    tuned.auxJpegSize = std::make_pair(8192u, 6144u);
    tuned.auxJpegBufferUsage = 0x3;
    const auto t = resolveSessionRecipe(tuned, {});
    assert(t.companion->detach && t.companion->usage == 0x100u && t.companion->maxImages == 4u);
    assert(t.companion->physical == false && t.companion->required);
    assert(t.auxJpegBufferUsage == 0x3u);
    LensRoute keep = automatic;
    keep.companionDetach = false;
    assert(!resolveSessionRecipe(keep, {false, true}).companion->detach);
}

}  // namespace

int main() {
    testCompanionMatchesTheOldDecision();
    testCompanionSource();
    testTemplatesAndStructure();
    testGateHoldKeepsTheOldRule();
    testVivoStillStrategy();
    testGateHoldJson();
    testSessionParamsAndRequestDefaults();
    testCompanionBlockDefaults();
    std::cout << "session_recipe_test passed\n";
    return 0;
}
