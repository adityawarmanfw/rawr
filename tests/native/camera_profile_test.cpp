#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <tuple>
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
    // The binned lenses, then the 50 MP H14 / H35 / H85 and 200 MP F35 / F85 (checked in testProfileJson).
    assert(dev.lenses.size() == std::size(table) + 5);
    assert(dev.lenses[std::size(table)].lensId == "H14" && dev.lenses[std::size(table) + 1].lensId == "H35" &&
           dev.lenses[std::size(table) + 2].lensId == "H85");
    assert(dev.lenses[std::size(table) + 3].lensId == "F35" && dev.lenses[std::size(table) + 4].lensId == "F85");
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

    const auto trimmed = parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"0","keys":[{"tag":" \tvendor.mode\n","values":[7]}]}]})");
    assert(trimmed && trimmed->lenses[0].keys[0].tag == "vendor.mode");
    assert(!parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"0","keys":[{"tag":"vendor.mode\nInt32\n7","values":[7]}]}]})", &error));
    assert(error == "Key name cannot contain whitespace; enter type and value in their separate fields");

    // A lens camera profile (.dcp) loads from its path; a missing or unusable
    // file keeps the lens (camera colour) and says why.
    {
        const auto path = (std::filesystem::temp_directory_path() / "camera_profile_test.dcp").string();
        std::vector<uint8_t> dcp = {'I', 'I', 0x52, 0x43, 8, 0, 0, 0, 2, 0};
        const auto put16 = [&](uint16_t v) { dcp.insert(dcp.end(), {uint8_t(v), uint8_t(v >> 8)}); };
        const auto put32 = [&](uint32_t v) {
            dcp.insert(dcp.end(), {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)});
        };
        put16(50721), put16(10), put32(9), put32(8 + 2 + 2 * 12 + 4);
        put16(50778), put16(3), put32(1), put16(21), put16(0);
        put32(0);
        for (int i = 0; i < 9; ++i) put32(i % 4 == 0 ? 1 : 0), put32(1);
        std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(dcp.data()), long(dcp.size()));
        const auto json = R"({"lenses":[{"id":"a","cameraId":"0","dcp":{"path":")" + path +
                          R"("}},{"id":"b","cameraId":"0","dcp":{"path":"/nonexistent/x.dcp"}}]})";
        const auto withDcp = parseCameraProfile(json, &error);
        assert(withDcp && withDcp->lenses.size() == 2);
        const auto& a = withDcp->lenses[0];
        assert(a.dcp && a.dcp->illuminant1 == 21 && a.dcpPath == path && a.dcpError.empty());
        const auto& b = withDcp->lenses[1];
        assert(!b.dcp && !b.dcpError.empty());
        assert(serializeCameraProfile(*withDcp).find(R"("dcp":{"path":")" + path + "\"}") != std::string::npos);
        std::filesystem::remove(path);
    }

    // vivo ultrawide lenses carry the stock app's distortion calibration (OpenCV rational,
    // 4096x3072); the DNG warp fit reproduces it on the binned and the 50 MP frame.
    {
        const auto& x300 = builtInCameraProfile("vivo_x300_ultra");
        for (const char* lensId : {"L14", "D14", "H14"}) {
            const auto r = routeForLens(x300, lensId);
            assert(r && r->distortion && r->distortion->width == 4096 && r->distortion->height == 3072);
            assert(std::abs(r->distortion->k[0] + 0.051307) < 1e-9 && std::abs(r->distortion->k[7] - 0.014256) < 1e-9);
            assert(std::abs(r->distortion->outputScale - 0.975811) < 1e-9);
        }
        for (const char* lensId : {"L35", "D35", "H35", "H85", "F85"}) assert(!routeForLens(x300, lensId)->distortion);
        const auto h14 = routeForLens(x300, "H14");
        const auto& d = *h14->distortion;
        // Fit verified against the stock JPEG (Python reference: kr0 1.0243256, kr1 -0.1207707).
        const auto binned = rawrcam::geometry::fitWarpRectilinear(d, 4096, 3072);
        const auto full = rawrcam::geometry::fitWarpRectilinear(d, 8192, 6144);
        assert(binned && full);
        assert(std::abs(binned->kr[0] - 1.0243256) < 1e-3 && std::abs(binned->kr[1] + 0.1207707) < 5e-3);
        assert(binned->meanErrorPx < 0.2 && binned->maxErrorPx < 3.0);
        for (size_t i = 0; i < 4; ++i) assert(std::abs(full->kr[i] - binned->kr[i]) < 1e-6);
        assert(full->maxErrorPx < 6.0);
        // Rejects another aspect; scaling keeps pixel-index centres consistent.
        assert(!rawrcam::geometry::scaledTo(d, 8160, 6144));
        const auto big = rawrcam::geometry::scaledTo(d, 8192, 6144);
        assert(big && std::abs(big->cx - ((d.cx + 0.5) * 2 - 0.5)) < 1e-9 && std::abs(big->fx - 2 * d.fx) < 1e-9);
        // Viewfinder UV mapping: resolution independent, matches the pixel mapping, inverts.
        for (const auto [u, v] : {std::pair{0.5, 0.5}, std::pair{0.02, 0.03}, std::pair{0.97, 0.6}, std::pair{0.3, 0.99}}) {
            const auto s = rawrcam::geometry::sourceUv(d, u, v);
            const auto p = rawrcam::geometry::sourcePixel(d, u * d.width - 0.5, v * d.height - 0.5);
            assert(std::abs(s[0] - (p[0] + 0.5) / d.width) < 1e-12 && std::abs(s[1] - (p[1] + 0.5) / d.height) < 1e-12);
            const auto s2 = rawrcam::geometry::sourceUv(*big, u, v);
            assert(std::abs(s2[0] - s[0]) < 1e-9 && std::abs(s2[1] - s[1]) < 1e-9);
            const auto back = rawrcam::geometry::correctedUv(d, s[0], s[1]);
            assert(std::abs(back[0] - u) < 1e-7 && std::abs(back[1] - v) < 1e-7);
        }
        // Video undistorts only its 16:9 record crop: the contained scale zooms just enough
        // that every output pixel samples inside the crop.
        {
            const auto binned = *rawrcam::geometry::scaledTo(d, 4096, 3072);
            const double crop = rawrcam::geometry::containedOutputScale(binned, 128, 456, 3840, 2160);
            assert(crop > binned.outputScale && crop < 1.2);
            auto m = binned;
            m.outputScale = crop;
            for (const auto [x, y] : {std::pair{128.0, 456.0}, std::pair{3967.0, 456.0}, std::pair{2048.0, 456.0},
                                      std::pair{128.0, 1536.0}, std::pair{3967.0, 2615.0}}) {
                const auto src = rawrcam::geometry::sourcePixel(m, x, y);
                assert(src[0] >= 128.0 - 1e-6 && src[0] <= 3967.0 + 1e-6 && src[1] >= 456.0 - 1e-6 &&
                       src[1] <= 2615.0 + 1e-6);
            }
            // Cached answer is the same; a slightly smaller scale would leave the crop.
            assert(rawrcam::geometry::containedOutputScale(binned, 128, 456, 3840, 2160) == crop);
            m.outputScale = crop * 0.99;
            const auto top = rawrcam::geometry::sourcePixel(m, 2048.0, 456.0);
            const auto side = rawrcam::geometry::sourcePixel(m, 128.0, 1536.0);
            assert(top[1] < 456.0 || side[0] < 128.0);
        }
        // The source of the output centre is the centre; the round trip keeps the block.
        const auto c = rawrcam::geometry::sourcePixel(d, d.cx, d.cy);
        assert(std::abs(c[0] - d.cx) < 1e-9 && std::abs(c[1] - d.cy) < 1e-9);
        const auto again = parseCameraProfile(serializeCameraProfile(x300));
        const auto h14Again = routeForLens(*again, "H14");
        const auto& d2 = *h14Again->distortion;
        assert(std::abs(d2.fx - d.fx) < 1e-5 && std::abs(d2.k[6] - d.k[6]) < 1e-12 && d2.outputScale == d.outputScale);
        assert(!parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"0","distortion":{"width":4096,"height":3072,
            "intrinsic":[1,1,0],"coefficients":[0,0,0,0,0,0,0,0]}}]})"));
    }

    // vivo 50 MP lenses (seamless route): binned viewfinder stream, full-size capture stream, still-only keys.
    {
        const auto& x300 = builtInCameraProfile("vivo_x300_ultra");
        for (const auto& [lensId, cameraId, w, h, mode] :
             {std::tuple{"H14", "4", 4096u, 3072u, 0.0}, std::tuple{"H35", "3", 4080u, 3064u, 2.0},
              std::tuple{"H85", "5", 4080u, 3072u, 14.0}}) {
            const auto s = routeForLens(x300, lensId);
            assert(s && s->cameraId == cameraId && !s->unlistedStream && !s->readerUsage);
            assert(s->preferredStream.width == w && s->preferredStream.height == h);
            assert(s->captureStream && s->captureStream->width == 8192 && s->captureStream->height == 6144);
            assert(s->captureStream->format == RawPixelFormat::Raw16 && s->vendorGate == "vivoFullSize");
            bool stillUiModule = false, sessionUiModule = false, forced = false, stillMode = false;
            for (const auto& k : s->keys) {
                // The full-size mode only on stills: it forces the switch where the AI would stay binned.
                forced |= k.tag == "vivo.control.forceSensorMode" && k.scope != CameraKeySetting::Scope::Still;
                stillMode |= k.tag == "vivo.control.forceSensorMode" && k.scope == CameraKeySetting::Scope::Still &&
                             k.values == std::vector<double>{mode};
                if (k.tag != "com.vivo.current_ui_module") continue;
                stillUiModule |= k.scope == CameraKeySetting::Scope::Still;
                sessionUiModule |= k.scope != CameraKeySetting::Scope::Still;
            }
            // ui_module 74 on the session or repeating request silences the RAW viewfinder.
            assert(stillUiModule && !sessionUiModule && !forced && stillMode);
        }
        // vivo 200 MP lenses: the same route with the ultra flag and no forced mode (the HAL switches by itself).
        // The main camera's HAL fails queued 200 MP stills: its burst goes in one still at a time.
        for (const auto& [lensId, cameraId, h, sequential] :
             {std::tuple{"F35", "3", 12256u, true}, std::tuple{"F85", "5", 12288u, false}}) {
            const auto s = routeForLens(x300, lensId);
            assert(s && s->cameraId == cameraId && s->vendorGate == "vivoFullSize" && s->captureBurst == 3 &&
                   s->captureBurstSequential == sequential);
            assert(s->captureStream && s->captureStream->width == 16320 && s->captureStream->height == h);
            bool ultra = false, aiHighRes = false, forced = false, decisionUiModule = false;
            for (const auto& k : s->keys) {
                ultra |= k.tag == "vivo.control.ultra_highresolution" && k.values == std::vector<double>{1.0};
                aiHighRes |= k.tag == "vivo.control.ai_highresolution";
                forced |= k.tag == "vivo.control.forceSensorMode";
                // The HAL keeps its last full-size/binned choice until a frame with this key.
                decisionUiModule |= k.tag == "com.vivo.current_ui_module" &&
                                    k.scope == CameraKeySetting::Scope::Decision && k.values == std::vector<double>{74.0};
            }
            assert(ultra && !aiHighRes && !forced && decisionUiModule);
        }
        const auto still = parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"0",
            "captureStream":{"format":"RAW16","width":8,"height":6},"captureBurst":2,
            "captureBurstSequential":true,"keys":[{"tag":"vendor.still","scope":"still","values":[1]},
            {"tag":"vendor.decision","scope":"decision","values":[2]}]}]})");
        assert(still && still->lenses[0].captureStream && still->lenses[0].captureStream->height == 6);
        assert(still->lenses[0].captureBurst == 2 && still->lenses[0].captureBurstSequential);
        // Block CFA layout and colour order override.
        const auto cfa = parseCameraProfile(R"({"lenses":[{"id":"q","cameraId":"5","cfa":{"block":4,"order":"GRBG","live":"fast","photo":"standard"}},
            {"id":"o","cameraId":"0","cfa":{"order":"BGGR"}}]})");
        assert(cfa && cfa->lenses[0].cfaBlock == 4 && cfa->lenses[0].cfaOrder == 1u);
        assert(cfa->lenses[0].cfaLiveQuality == 0u && cfa->lenses[0].cfaPhotoQuality == 1u);
        assert(cfa->lenses[1].cfaBlock == 1 && cfa->lenses[1].cfaOrder == 3u);
        assert(cfa->lenses[1].cfaLiveQuality == 1u && cfa->lenses[1].cfaPhotoQuality == 2u);  // defaults
        assert(serializeCameraProfile(*parseCameraProfile(serializeCameraProfile(*cfa))) == serializeCameraProfile(*cfa));
        assert(!parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"0","cfa":{"block":3}}]})"));
        assert(!parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"0","cfa":{"order":"RGBG"}}]})"));
        assert(still->lenses[0].keys[0].scope == CameraKeySetting::Scope::Still);
        assert(still->lenses[0].keys[1].scope == CameraKeySetting::Scope::Decision);
        assert(serializeCameraProfile(*still).find("\"scope\":\"decision\"") != std::string::npos);
        assert(serializeCameraProfile(*parseCameraProfile(serializeCameraProfile(*still))) ==
               serializeCameraProfile(*still));
        assert(!parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"0","captureStream":{"width":0}}]})"));
        // Binned lenses keep the ladder's reader usage and no gate.
        const auto l35 = routeForLens(x300, "L35");
        assert(l35 && !l35->unlistedStream && !l35->readerUsage && l35->vendorGate.empty());

        const auto custom = parseCameraProfile(R"({"lenses":[{"id":"X","cameraId":"5",
            "stream":{"format":"RAW16","width":8160,"height":6144,"unlisted":true},
            "readerUsage":"none","vendorGate":"vivoFullSize"}]})", &error);
        assert(custom && custom->lenses[0].unlistedStream && custom->lenses[0].readerUsage == 0u);
        const auto again = parseCameraProfile(serializeCameraProfile(*custom));
        assert(again && again->lenses[0].readerUsage == 0u && again->lenses[0].vendorGate == "vivoFullSize");
        assert(!parseCameraProfile(R"({"lenses":[{"id":"X","cameraId":"5","readerUsage":"cpu"}]})", &error));
        assert(error == "unknown readerUsage cpu for lens X");
    }

    // Rejected: malformed JSON, no lenses, duplicate ids, unknown key type, static levels without white.
    assert(!parseCameraProfile("{\"lenses\":[", &error) && !error.empty());
    assert(!parseCameraProfile(R"({"lenses":[]})"));
    assert(!parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"0"},{"id":"a","cameraId":"1"}]})"));
    assert(!parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"0","keys":[{"tag":"x","type":"half"}]}]})"));
    assert(!parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"0","levels":{"static":true,"black":[1]}}]})"));
}

}  // namespace

int main() {
    // Wire values and EXIF composition agree for every rotation and reflection.
    constexpr uint16_t expected[4][8] = {
        {1, 2, 3, 4, 5, 6, 7, 8},
        {2, 1, 4, 3, 8, 7, 6, 5},
        {4, 3, 2, 1, 6, 5, 8, 7},
        {3, 4, 1, 2, 7, 8, 5, 6}};
    const char* names[] = {"none", "horizontal", "vertical", "both"};
    for (uint32_t mirror = 0; mirror < 4; ++mirror) {
        const std::string json = std::string(R"({"lenses":[{"id":"a","cameraId":"0","mirror":")") + names[mirror] + R"("}]})";
        const auto profile = rawrcam::camera::parseCameraProfile(json);
        assert(profile && profile->lenses.front().mirror == mirror);
        const auto again = rawrcam::camera::parseCameraProfile(rawrcam::camera::serializeCameraProfile(*profile));
        assert(again && again->lenses.front().mirror == mirror);
        for (uint16_t orientation = 1; orientation <= 8; ++orientation)
            assert(rawrcam::geometry::mirroredOrientation(orientation, mirror) == expected[mirror][orientation - 1]);
    }
    assert(!rawrcam::camera::parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"0","mirror":"invalid"}]})"));
    testGeometryGate();
    testNegotiation();
    testEmbeddedProfiles();
    testProfileMatching();
    testProfiles();
    testProfileJson();
    std::cout << "camera_profile_test passed\n";
    return 0;
}
