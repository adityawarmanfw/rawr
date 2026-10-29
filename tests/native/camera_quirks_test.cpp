#include <cassert>
#include <iostream>
#include <string>

#include "camera/CameraProfileJson.h"
#include "camera/CameraRouting.h"

using namespace rawrcam::camera;

namespace {

// The conditions NativeCameraController used before quirks were named, kept as the oracle. Since then an
// unlisted stream no longer implies noTapAf (Xiaomi full-size lenses focus fine).
struct Legacy {
    bool tapAfHold, noHalPriorityAe, noTapAf, requireFpsRange, noAutoCompanion;
};
Legacy legacy(const std::string& matchedProfileId, const LensRoute& lens) {
    return {matchedProfileId == "vivo_x300_ultra" || !lens.vendorGate.empty(), lens.unlistedStream, false,
            lens.unlistedStream, lens.unlistedStream};
}

void check(const std::string& matchedProfileId, const CameraQuirks& device, const LensRoute& lens) {
    const CameraQuirks q = effectiveQuirks(lens, device);
    const Legacy l = legacy(matchedProfileId, lens);
    assert(q.afTapHold == l.tapAfHold);
    assert(q.noHalPriorityAe == l.noHalPriorityAe);
    assert(q.noTapAf == l.noTapAf);
    assert(q.requireFpsRange == l.requireFpsRange);
    assert(q.noAutoCompanion == l.noAutoCompanion);
}

void testBuiltInDevicesKeepTheirBehaviour() {
    std::vector<LensRoute> lenses;
    for (const auto& p : builtInProfiles()) lenses.insert(lenses.end(), p.profile.lenses.begin(), p.profile.lenses.end());
    LensRoute gated;
    gated.vendorGate = "vivoFullSize";
    LensRoute unlisted;
    unlisted.unlistedStream = true;
    LensRoute both = gated;
    both.unlistedStream = true;
    lenses.insert(lenses.end(), {LensRoute{}, gated, unlisted, both});
    assert(lenses.size() > 8);

    size_t matched = 0;
    for (const auto& device : builtInProfiles()) {
        if (device.match.models.empty()) continue;
        const auto match = matchBuiltInProfile([&](const std::string& name) {
            return name == "ro.product.model" ? device.match.models.front() : std::string();
        });
        assert(match.profileId == device.profile.id);
        assert(match.quirks == device.quirks);
        ++matched;
        // Every lens that can run on this device, the user's included, behaves as it did.
        for (const auto& lens : lenses) check(match.profileId, match.quirks, lens);
    }
    assert(matched >= 2);
    // A device no profile matches has no device quirks.
    const auto none = matchBuiltInProfile([](const std::string&) { return std::string(); });
    assert(none.profileId == "generic" && !none.quirks.any());
    for (const auto& lens : lenses) check(none.profileId, none.quirks, lens);
}

void testVivoUltraDeviceQuirk() {
    const auto& ultra = builtInProfiles();
    bool found = false;
    for (const auto& p : ultra) {
        if (p.profile.id != "vivo_x300_ultra") continue;
        found = true;
        assert(p.quirks.afTapHold && !p.quirks.noTapAf && !p.quirks.noHalPriorityAe);
    }
    assert(found);
    // A plain lens on that device still holds a tap; the same lens elsewhere does not.
    LensRoute plain;
    CameraQuirks device;
    assert(!effectiveQuirks(plain, device).any());
    device.afTapHold = true;
    assert(effectiveQuirks(plain, device).afTapHold);
}

void testPresets() {
    LensRoute gated;
    gated.vendorGate = "someGate";
    const auto g = effectiveQuirks(gated, {});
    assert(g.afTapHold && !g.noTapAf && !g.requireFpsRange);
    LensRoute unlisted;
    unlisted.unlistedStream = true;
    const auto u = effectiveQuirks(unlisted, {});
    assert(!u.afTapHold && u.noHalPriorityAe && !u.noTapAf && u.requireFpsRange && u.noAutoCompanion);
    unlisted.quirks.noTapAf = true;
    assert(effectiveQuirks(unlisted, {}).noTapAf);
}

void testJson() {
    std::string error;
    const auto lens = parseCameraProfile(
        R"({"lenses":[{"id":"a","cameraId":"0","quirks":["noTapAf","requireFpsRange"]}]})", &error);
    assert(lens);
    const auto& q = lens->lenses[0].quirks;
    assert(q.noTapAf && q.requireFpsRange && !q.afTapHold && !q.noHalPriorityAe && !q.noAutoCompanion);
    const auto json = serializeCameraProfile(*lens);
    assert(json.find(R"("quirks":["noTapAf","requireFpsRange"])") != std::string::npos);
    const auto again = parseCameraProfile(json);
    assert(again && again->lenses[0].quirks == q && serializeCameraProfile(*again) == json);
    // No quirks, no field.
    assert(serializeCameraProfile(*parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"0"}]})")).find("quirks") ==
           std::string::npos);
    // Unknown names are rejected, so a typo cannot silently drop a protection.
    assert(!parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"0","quirks":["noTapAF"]}]})", &error));
    assert(error.find("unknown quirk") != std::string::npos);
    assert(!parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"0","quirks":[3]}]})", &error));

    const auto device = parseBuiltInProfile(
        R"({"id":"x","match":{"models":["M"]},"quirks":["afTapHold"],"lenses":[{"id":"a","cameraId":"0"}]})", &error);
    assert(device && device->quirks.afTapHold && !device->quirks.noTapAf);
    assert(!parseBuiltInProfile(
        R"({"id":"x","match":{"models":["M"]},"quirks":["bogus"],"lenses":[{"id":"a","cameraId":"0"}]})", &error));
}

void testKeepRegions() {
    CameraQuirks q;
    assert(!q.keepRegions && !q.any());
    assert(setQuirk(q, "keepRegions") && q.keepRegions && q.any());
    assert(quirkNames(q) == std::vector<std::string>{"keepRegions"});
    CameraQuirks other;
    other.noTapAf = true;
    const auto merged = mergeQuirks(q, other);
    assert(merged.keepRegions && merged.noTapAf && !(merged == other) && !(merged == q));
    std::string error;
    const auto lens = parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"0","quirks":["keepRegions"]}]})", &error);
    assert(lens && lens->lenses[0].quirks.keepRegions);
    const auto json = serializeCameraProfile(*lens);
    assert(json.find(R"("quirks":["keepRegions"])") != std::string::npos);
    assert(parseCameraProfile(json) && parseCameraProfile(json)->lenses[0].quirks == lens->lenses[0].quirks);
    // The request rule: a tap's region is always written; without one the entry is cleared unless the lens keeps the template's.
    assert(writesRegionEntry(true, false) && writesRegionEntry(true, true));
    assert(writesRegionEntry(false, false) && !writesRegionEntry(false, true));
}

void testCarriesRecipe() {
    LensRoute lens;
    assert(!carriesRecipe(lens));
    lens.quirks.noTapAf = true;
    assert(carriesRecipe(lens));
}

}  // namespace

int main() {
    testBuiltInDevicesKeepTheirBehaviour();
    testVivoUltraDeviceQuirk();
    testPresets();
    testJson();
    testKeepRegions();
    testCarriesRecipe();
    std::cout << "camera_quirks_test passed\n";
    return 0;
}
