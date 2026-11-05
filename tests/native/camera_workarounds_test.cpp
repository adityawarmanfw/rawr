#include <cassert>
#include <iostream>
#include <string>

#include "camera/CameraProfileJson.h"
#include "camera/CameraRouting.h"

using namespace rawrcam::camera;

namespace {

// The conditions NativeCameraController used before quirks were named, kept as the oracle for built-in
// lenses. Since then a high-res live stream no longer implies noTapFocus (Xiaomi full-size lenses focus fine),
// and the X300 Ultra's holdTapFocus moved from the device onto each of its lenses.
struct Legacy {
    bool holdTapFocus, appPriorityAe, noTapFocus, alwaysSendFrameRate, noAutoCompanion;
};
Legacy legacy(const std::string& profileId, const LensRoute& lens) {
    return {profileId == "vivo_x300_ultra" || !lens.vendorGate.empty(), lens.highResLive, false,
            lens.highResLive, lens.highResLive};
}

void check(const std::string& profileId, const LensRoute& lens) {
    const CameraWorkarounds q = effectiveWorkarounds(lens);
    const Legacy l = legacy(profileId, lens);
    assert(q.holdTapFocus == l.holdTapFocus);
    assert(q.appPriorityAe == l.appPriorityAe);
    assert(q.noTapFocus == l.noTapFocus);
    assert(q.alwaysSendFrameRate == l.alwaysSendFrameRate);
    assert(q.noAutoCompanion == l.noAutoCompanion);
}

void testBuiltInLensesKeepTheirBehaviour() {
    size_t lenses = 0;
    for (const auto& p : builtInProfiles())
        for (const auto& lens : p.profile.lenses) {
            check(p.profile.id, lens);
            ++lenses;
        }
    assert(lenses > 8);
    // Lenses outside a profile get only what their shape implies.
    LensRoute gated;
    gated.vendorGate = "vivoFullSize";
    LensRoute unlisted;
    unlisted.highResLive = true;
    LensRoute both = gated;
    both.highResLive = true;
    for (const auto& lens : {LensRoute{}, gated, unlisted, both}) check("generic", lens);
}

void testDeviceMatch() {
    size_t matched = 0;
    for (const auto& device : builtInProfiles()) {
        if (device.match.models.empty()) continue;
        const auto match = matchBuiltInProfile([&](const std::string& name) {
            return name == "ro.product.model" ? device.match.models.front() : std::string();
        });
        assert(match.profileId == device.profile.id);
        ++matched;
    }
    assert(matched >= 2);
    const auto none = matchBuiltInProfile([](const std::string&) { return std::string(); });
    assert(none.profileId == "generic");
}

void testVivoUltraLensesHoldTaps() {
    bool found = false;
    for (const auto& p : builtInProfiles()) {
        if (p.profile.id != "vivo_x300_ultra") continue;
        found = true;
        for (const auto& lens : p.profile.lenses)
            assert(lens.workarounds.holdTapFocus && !lens.workarounds.noTapFocus && !lens.workarounds.appPriorityAe);
    }
    assert(found);
    // A lens without the quirk does not hold a tap, whatever device it runs on.
    assert(!effectiveWorkarounds(LensRoute{}).any());
}

void testPresets() {
    LensRoute gated;
    gated.vendorGate = "someGate";
    const auto g = effectiveWorkarounds(gated);
    assert(g.holdTapFocus && !g.noTapFocus && !g.alwaysSendFrameRate);
    LensRoute unlisted;
    unlisted.highResLive = true;
    const auto u = effectiveWorkarounds(unlisted);
    assert(!u.holdTapFocus && u.appPriorityAe && !u.noTapFocus && u.alwaysSendFrameRate && u.noAutoCompanion);
    unlisted.workarounds.noTapFocus = true;
    assert(effectiveWorkarounds(unlisted).noTapFocus);
}

void testJson() {
    std::string error;
    const auto lens = parseCameraProfile(
        R"({"lenses":[{"id":"a","cameraId":"0","quirks":["noTapAf","requireFpsRange"]}]})", &error);
    assert(lens);
    const auto& q = lens->lenses[0].workarounds;
    assert(q.noTapFocus && q.alwaysSendFrameRate && !q.holdTapFocus && !q.appPriorityAe && !q.noAutoCompanion);
    const auto json = serializeCameraProfile(*lens);
    assert(json.find(R"("workarounds":["noTapFocus","alwaysSendFrameRate"])") != std::string::npos);
    const auto again = parseCameraProfile(json);
    assert(again && again->lenses[0].workarounds == q && serializeCameraProfile(*again) == json);
    // No quirks, no field.
    assert(serializeCameraProfile(*parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"0"}]})")).find("workarounds") ==
           std::string::npos);
    // Unknown names are rejected, so a typo cannot silently drop a protection.
    assert(!parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"0","quirks":["noTapAF"]}]})", &error));
    assert(error.find("unknown workaround") != std::string::npos);
    assert(!parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"0","quirks":[3]}]})", &error));

    // Quirks belong to lenses: a device-wide list is refused instead of silently applied or dropped.
    assert(!parseBuiltInProfile(
        R"({"id":"x","match":{"models":["M"]},"quirks":["afTapHold"],"lenses":[{"id":"a","cameraId":"0"}]})", &error));
    assert(error.find("workarounds belong to lenses") != std::string::npos);
}

void testKeepRegions() {
    CameraWorkarounds q;
    assert(!q.keepTemplateRegions && !q.any());
    assert(setWorkaround(q, "keepTemplateRegions") && q.keepTemplateRegions && q.any());
    assert(!setWorkaround(q, "keepRegions"));  // the schema-1 name is only read by the upgrade
    assert(workaroundNames(q) == std::vector<std::string>{"keepTemplateRegions"});
    CameraWorkarounds other;
    other.noTapFocus = true;
    const auto merged = mergeWorkarounds(q, other);
    assert(merged.keepTemplateRegions && merged.noTapFocus && !(merged == other) && !(merged == q));
    std::string error;
    const auto lens = parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"0","quirks":["keepRegions"]}]})", &error);
    assert(lens && lens->lenses[0].workarounds.keepTemplateRegions);
    const auto json = serializeCameraProfile(*lens);
    assert(json.find(R"("workarounds":["keepTemplateRegions"])") != std::string::npos);
    assert(parseCameraProfile(json) && parseCameraProfile(json)->lenses[0].workarounds == lens->lenses[0].workarounds);
    // The request rule: a tap's region is always written; without one the entry is cleared unless the lens keeps the template's.
    assert(writesRegionEntry(true, false) && writesRegionEntry(true, true));
    assert(writesRegionEntry(false, false) && !writesRegionEntry(false, true));
}

void testCarriesRecipe() {
    LensRoute lens;
    assert(!carriesRecipe(lens));
    lens.workarounds.noTapFocus = true;
    assert(carriesRecipe(lens));
}

}  // namespace

int main() {
    testBuiltInLensesKeepTheirBehaviour();
    testDeviceMatch();
    testVivoUltraLensesHoldTaps();
    testPresets();
    testJson();
    testKeepRegions();
    testCarriesRecipe();
    std::cout << "camera_workarounds_test passed\n";
    return 0;
}
