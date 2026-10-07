#include <cassert>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "camera/CameraProfileJson.h"
#include "camera/CameraRouting.h"
#include "geometry/RawGeometry.h"

using rawrcam::geometry::isSupportedRawGeometry;
using rawrcam::geometry::negotiateRawStream;
using rawrcam::geometry::RawPixelFormat;
using rawrcam::geometry::RawStreamOption;
using rawrcam::geometry::RawStreamPreference;
using PropertyReaderFn = rawrcam::camera::PropertyReader;

namespace {

void testGeometryGate() {
    assert(isSupportedRawGeometry(4080, 3064));
    assert(isSupportedRawGeometry(4096, 3072));
    assert(isSupportedRawGeometry(4000, 3000));
    assert(isSupportedRawGeometry(8192, 6144));
    assert(isSupportedRawGeometry(12288, 4096));
    assert(!isSupportedRawGeometry(4081, 3064));   // odd width
    assert(!isSupportedRawGeometry(4080, 3065));   // odd height
    assert(!isSupportedRawGeometry(640, 480));     // below multiframe minimum
    assert(!isSupportedRawGeometry(16386, 4096));  // above maxImageDimension2D
}

void testNegotiation() {
    const std::vector<RawStreamOption> x200u = {
        {RawPixelFormat::Raw16, 4096, 3072},
        {RawPixelFormat::Raw10, 4096, 3072},
    };
    // Preferred size missing (dev-phone 4080x3064 route on another phone) -> largest RAW16.
    auto chosen = negotiateRawStream(x200u, {RawPixelFormat::Raw16, 4080, 3064});
    assert(chosen && *chosen == (RawStreamOption{RawPixelFormat::Raw16, 4096, 3072}));

    // Exact preferred size is kept even when a larger one exists.
    const std::vector<RawStreamOption> devPhone = {
        {RawPixelFormat::Raw16, 4080, 3072},
        {RawPixelFormat::Raw16, 4080, 3064},
    };
    chosen = negotiateRawStream(devPhone, {RawPixelFormat::Raw16, 4080, 3064});
    assert(chosen && *chosen == (RawStreamOption{RawPixelFormat::Raw16, 4080, 3064}));

    // "Negotiate" preference picks the largest area.
    chosen = negotiateRawStream(devPhone, {});
    assert(chosen && chosen->height == 3072);

    // RAW10-only device falls back to RAW10.
    const std::vector<RawStreamOption> raw10Only = {{RawPixelFormat::Raw10, 4000, 3000},
                                                    {RawPixelFormat::Raw10, 2000, 1500}};
    chosen = negotiateRawStream(raw10Only, {});
    assert(chosen && *chosen == (RawStreamOption{RawPixelFormat::Raw10, 4000, 3000}));

    // An explicit RAW10 preference wins over available RAW16.
    chosen = negotiateRawStream(x200u, {RawPixelFormat::Raw10, 0, 0});
    assert(chosen && chosen->format == RawPixelFormat::Raw10);

    // Unusable sizes are skipped: odd, too large, RAW10 width not a multiple of 4.
    const std::vector<RawStreamOption> awkward = {{RawPixelFormat::Raw16, 4081, 3064},
                                                  {RawPixelFormat::Raw16, 20000, 15000},
                                                  {RawPixelFormat::Raw10, 4002, 3000},
                                                  {RawPixelFormat::Raw16, 3264, 2448}};
    chosen = negotiateRawStream(awkward, {});
    assert(chosen && *chosen == (RawStreamOption{RawPixelFormat::Raw16, 3264, 2448}));

    assert(!negotiateRawStream({}, {}));
    assert(!negotiateRawStream({{RawPixelFormat::Raw16, 640, 480}}, {}));
}

// Every camera/profiles/*.json must parse strictly (the app silently drops a
// broken one), with a unique id.
void testEmbeddedProfiles() {
    using namespace rawrcam::camera;
    static const char* const sources[] = {
#include "camera/BuiltInCameraProfiles.inc"
    };
    std::vector<std::string> ids;
    for (const char* source : sources) {
        std::string error;
        const auto parsed = parseBuiltInProfile(source, &error);
        if (!parsed) std::cerr << "built-in profile rejected: " << error << '\n';
        assert(parsed);
        for (const auto& id : ids) assert(id != parsed->profile.id);
        ids.push_back(parsed->profile.id);
    }
    assert(builtInProfiles().size() == ids.size());
}

PropertyReaderFn props(std::map<std::string, std::string> values) {
    return [values](const std::string& name) {
        const auto it = values.find(name);
        return it == values.end() ? std::string() : it->second;
    };
}

void testProfileMatching() {
    using namespace rawrcam::camera;
    // Global X300 Ultra: exact, tested model.
    auto m = matchBuiltInProfile(props({{"ro.product.model", "V2562"}, {"ro.vivo.product.model", "PD2547F_EX"}}));
    assert(m.profileId == "vivo_x300_ultra" && m.rule == "model" && m.verified);
    // China variants: listed but untested.
    for (const char* model : {"V2547A", "V2547DA"}) {
        m = matchBuiltInProfile(props({{"ro.product.model", model}}));
        assert(m.profileId == "vivo_x300_ultra" && m.rule == "model" && !m.verified);
    }
    // An unlisted variant is caught by vivo's project code.
    m = matchBuiltInProfile(props({{"ro.product.model", "V2547X"}, {"ro.vivo.product.model", "PD2547"}}));
    assert(m.profileId == "vivo_x300_ultra" && m.rule == "property:ro.vivo.product.model" && !m.verified);
    // X300 Pro (Dimensity 9500 / Mali): exact, tested model.
    m = matchBuiltInProfile(props({{"ro.product.model", "V2514"}}));
    assert(m.profileId == "vivo_x300_pro" && m.rule == "model" && m.verified);
    // Chinese standard and satellite variants are recognized, but not device-tested.
    for (const auto* model : {"V2502A", "V2502DA"}) {
        m = matchBuiltInProfile(props({{"ro.product.model", model}}));
        assert(m.profileId == "vivo_x300_pro" && m.rule == "model" && !m.verified);
    }
    const auto pro = builtInCameraProfile("vivo_x300_pro");
    assert(pro.lenses.size() == 3);
    assert(routeForLens(pro, "UW")->cameraId == "4" && routeForLens(pro, "1x")->cameraId == "2");
    assert(routeForLens(pro, "3.5x")->cameraId == "3");
    // Camera 5 is the same telephoto sensor as camera 3, so it is not offered as a fourth lens.
    assert(!routeForLens(pro, "8x"));
    // Other phones (X200 Ultra) and missing properties get generic.
    m = matchBuiltInProfile(props({{"ro.product.model", "V2454A"}, {"ro.vivo.product.model", "PD2454"}}));
    assert(m.profileId == "generic" && m.rule == "none" && !m.verified);
    assert(matchBuiltInProfile(props({})).profileId == "generic");

    assert(isBuiltInProfileId("vivo_x300_ultra") && isBuiltInProfileId("vivo_x300_pro") && isBuiltInProfileId("generic"));
    assert(!isBuiltInProfileId("") && !isBuiltInProfileId("user") && !isBuiltInProfileId("v2562"));

    // Rejected built-ins: reserved id, no match rules.
    const std::string lens = R"("lenses":[{"id":"1x","cameraId":"0"}])";
    assert(!parseBuiltInProfile(R"({"id":"generic","match":{"models":["X"]},)" + lens + "}"));
    assert(!parseBuiltInProfile(R"({"id":"x","match":{"models":[]},)" + lens + "}"));
    const auto ok = parseBuiltInProfile(R"({"id":"x","version":3,"match":{"propertyPrefixes":{"a":"b"}},)" + lens + "}");
    assert(ok && ok->version == 3 && ok->name == "x" && ok->match.propertyPrefixes.size() == 1);
}

// The X300 Ultra defaults, as exported from the dev phone's release build:
// L* = RAW10 with dynamic levels, D* = RAW16 DCG readout modes with static
// levels, Z* = ISZ crop modes (10-bit) on the tele camera.
void testProfiles() {
    using namespace rawrcam::camera;
    const auto& dev = builtInCameraProfile("vivo_x300_ultra");
    struct Expected {
        const char* lens;
        const char* camera;
        RawPixelFormat format;
        uint32_t width, height;
        int mode;
        bool isStatic;
        float black, white;
    };
    const Expected table[] = {
        {"L14", "4", RawPixelFormat::Raw10, 4096, 3072, 17, false, 0, 0},
        {"D14", "4", RawPixelFormat::Raw16, 4096, 3072, 23, true, 1024, 8712},
        {"L35", "3", RawPixelFormat::Raw10, 4080, 3064, 31, false, 0, 0},
        {"D35", "3", RawPixelFormat::Raw16, 4080, 3064, 17, true, 1024, 8712},
        {"L85", "5", RawPixelFormat::Raw10, 4080, 3072, 23, false, 0, 0},
        {"D85", "5", RawPixelFormat::Raw16, 4080, 3072, 19, true, 1024, 16383},
        {"Z170", "5", RawPixelFormat::Raw10, 4080, 3072, 31, true, 64, 1023},
        {"Z340", "5", RawPixelFormat::Raw10, 4080, 3072, 6, true, 64, 1023},
    };
    assert(dev.lenses.size() == std::size(table));
    for (size_t i = 0; i < std::size(table); ++i) {
        const auto& e = table[i];
        const auto& r = dev.lenses[i];  // Profile order is the capture-screen order.
        assert(r.lensId == e.lens && r.cameraId == e.camera && r.physicalCameraId.empty());
        assert(r.preferredStream.format == e.format);
        assert(r.preferredStream.width == e.width && r.preferredStream.height == e.height);
        assert(r.keys.size() == 1 && r.keys[0].tag == "vivo.control.forceSensorMode");
        assert(r.keys[0].type == CameraKeySetting::Type::Int32 && r.keys[0].scope == CameraKeySetting::Scope::Session);
        assert(r.keys[0].values == std::vector<double>{double(e.mode)} && r.keys[0].enabled);
        assert(r.levels.isStatic == e.isStatic && r.levels.white == e.white);
        for (float b : r.levels.blackRggb) assert(b == e.black);
    }

    const auto& generic = builtInCameraProfile("generic");
    assert(generic.lenses.size() == 1);
    const auto only = routeForLens(generic, "1x");
    assert(only && only->cameraId == "0" && only->keys.empty() && !only->levels.isStatic);
    assert(only->preferredStream.width == 0 && only->preferredStream.height == 0);
    // Unknown ids fall back to the generic profile.
    assert(builtInCameraProfile("nope").id == "generic");

    // Camera-id override: known id keeps its first tuned route, unknown id gets a negotiated one.
    assert(routeForCameraId(dev, "5").lensId == "L85");
    const auto adHoc = routeForCameraId(dev, "7");
    assert(adHoc.cameraId == "7" && adHoc.keys.empty() && adHoc.preferredStream.width == 0);
}

void testProfileJson() {
    using namespace rawrcam::camera;
    // Round trip keeps every field of the built-in profiles.
    for (const char* id : {"vivo_x300_ultra", "generic"}) {
        const auto& original = builtInCameraProfile(id);
        const auto json = serializeCameraProfile(original);
        std::string error;
        const auto parsed = parseCameraProfile(json, &error);
        assert(parsed && error.empty());
        assert(serializeCameraProfile(*parsed) == json);
    }

    // A user profile: escaped name, physical camera, RAW10 stream, mixed key scopes.
    const std::string user = R"({"id":"user","lenses":[{"id":"W\"1","cameraId":"2","physicalCameraId":"5",
        "stream":{"format":"RAW10","width":4000,"height":3000},
        "levels":{"static":true,"black":[64],"white":1023},
        "keys":[{"tag":"0x80020000","type":"int64","scope":"request","values":[1,2],"enabled":false},
                {"tag":"com.vendor.mode","type":"byte","scope":"session","values":[3]}],
        "unknown":{"nested":[null,true]}}]})";
    std::string error;
    const auto parsed = parseCameraProfile(user, &error);
    assert(parsed && parsed->lenses.size() == 1);
    const auto& r = parsed->lenses[0];
    assert(r.lensId == "W\"1" && r.cameraId == "2" && r.physicalCameraId == "5");
    assert(r.preferredStream.format == RawPixelFormat::Raw10 && r.preferredStream.width == 4000);
    assert(r.levels.isStatic && r.levels.white == 1023 && r.levels.blackRggb[3] == 64);
    assert(r.keys.size() == 2 && !r.keys[0].enabled && r.keys[0].scope == CameraKeySetting::Scope::Request);
    assert(r.keys[0].type == CameraKeySetting::Type::Int64 && r.keys[0].values.size() == 2);
    assert(r.keys[1].enabled && r.keys[1].scope == CameraKeySetting::Scope::Session);
    // The escaped name survives a second round trip.
    assert(parseCameraProfile(serializeCameraProfile(*parsed))->lenses[0].lensId == "W\"1");

    // Rejected: malformed JSON, no lenses, duplicate ids, unknown key type, static levels without white.
    assert(!parseCameraProfile("{\"lenses\":[", &error) && !error.empty());
    assert(!parseCameraProfile(R"({"lenses":[]})"));
    assert(!parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"0"},{"id":"a","cameraId":"1"}]})"));
    assert(!parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"0","keys":[{"tag":"x","type":"half"}]}]})"));
    assert(!parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"0","levels":{"static":true,"black":[1]}}]})"));
}

}  // namespace

int main() {
    testGeometryGate();
    testNegotiation();
    testEmbeddedProfiles();
    testProfileMatching();
    testProfiles();
    testProfileJson();
    std::cout << "camera_profile_test passed\n";
    return 0;
}
