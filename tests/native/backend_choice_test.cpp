#include <cassert>
#include <iostream>
#include <string>

#include "camera/CameraProfileJson.h"
#include "camera/CameraRouting.h"

using namespace rawrcam::camera;

namespace {

void testDerivedBackend() {
    LensRoute plain;
    assert(!needsJavaBackend(plain) && effectiveBackend(plain) == BackendChoice::Ndk);
    LensRoute mode;
    mode.opMode = 0x8001;
    assert(needsJavaBackend(mode) && effectiveBackend(mode) == BackendChoice::Java);
    for (int64_t LensRoute::*field : {&LensRoute::rawStreamUseCase, &LensRoute::captureStreamUseCase,
                                       &LensRoute::companionStreamUseCase, &LensRoute::auxJpegStreamUseCase}) {
        LensRoute r;
        r.*field = 524547;
        assert(needsJavaBackend(r) && effectiveBackend(r) == BackendChoice::Java && carriesRecipe(r));
    }
    // An explicit choice wins over the derived one.
    LensRoute java;
    java.backend = BackendChoice::Java;
    assert(effectiveBackend(java) == BackendChoice::Java && carriesRecipe(java));
    assert(std::string(backendChoiceName(BackendChoice::Auto)) == "auto");
}

void testJsonRoundTrip() {
    std::string error;
    const auto parsed = parseCameraProfile(R"({"lenses":[{"id":"M","cameraId":"2","backend":"java",
        "session":{"params":"full","opMode":32769},
        "stream":{"format":"RAW16","width":8192,"height":6144,"useCase":524547},
        "captureStream":{"format":"RAW16","width":8192,"height":6144,"useCase":6},
        "companion":{"width":640,"height":480,"useCase":3},
        "auxJpeg":{"width":8192,"height":6144,"useCase":2}}]})", &error);
    assert(parsed);
    const auto& r = parsed->lenses[0];
    assert(r.backend == BackendChoice::Java && r.opMode == 32769u && r.sessionParams == SessionParamsPolicy::Full);
    assert(r.rawStreamUseCase == 524547 && r.captureStreamUseCase == 6 && r.companionStreamUseCase == 3 &&
           r.auxJpegStreamUseCase == 2);
    assert(r.preferredStream.width == 8192 && r.captureStream && r.companionSize && r.auxJpegSize);
    const auto json = serializeCameraProfile(*parsed);
    const auto again = parseCameraProfile(json, &error);
    assert(again && serializeCameraProfile(*again) == json);
    assert(again->lenses[0].opMode == 32769u && again->lenses[0].rawStreamUseCase == 524547);
    assert(json.find(R"("backend":"java")") != std::string::npos);

    // Opt-in: nothing is written for a plain lens.
    const auto plain = serializeCameraProfile(*parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"2"}]})"));
    for (const char* field : {"backend", "opMode", "useCase"}) assert(plain.find(field) == std::string::npos);
    // opMode alone inside the session block keeps the other session fields default.
    const auto onlyMode = parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"2","session":{"opMode":40000}}]})");
    assert(onlyMode && onlyMode->lenses[0].sessionParams == SessionParamsPolicy::Cadence);
    assert(serializeCameraProfile(*onlyMode).find(R"("session":{"opMode":40000})") != std::string::npos);
}

void testRejections() {
    std::string error;
    const auto bad = [&](const char* fields) {
        return !parseCameraProfile(std::string(R"({"lenses":[{"id":"a","cameraId":"2",)") + fields + "}]}", &error);
    };
    assert(bad(R"("backend":"gpu")") && error.find("unknown backend") != std::string::npos);
    assert(bad(R"("backend":"ndk","session":{"opMode":32769})") && error.find("Java backend") != std::string::npos);
    assert(bad(R"("backend":"ndk","stream":{"useCase":3})"));
    assert(!bad(R"("backend":"java")") && !bad(R"("backend":"ndk")") && !bad(R"("backend":"auto","session":{"opMode":40000})"));
    for (const char* mode : {R"("session":{"opMode":1})", R"("session":{"opMode":2})", R"("session":{"opMode":-1})",
                             R"("session":{"opMode":2147483648})", R"("session":{"opMode":1.5})"})
        assert(bad(mode));
    assert(!bad(R"("session":{"opMode":0})") && !bad(R"("session":{"opMode":3})") && !bad(R"("session":{"opMode":2147483647})"));
    for (const char* use : {R"("stream":{"useCase":-1})", R"("stream":{"useCase":2.5})", R"("companion":{"useCase":-3})",
                            R"("auxJpeg":{"width":640,"height":480,"useCase":0.5})"})
        assert(bad(use));
    assert(bad(R"("streamUseCase":524547)") && error.find("not a profile field") != std::string::npos);
    // Stop-repeating stills stay on the NDK, whether Java is explicit or derived; the vivo gate runs on either API.
    assert(!bad(R"("backend":"java","vendorGate":"vivoFullSize")") && !bad(R"("vendorGate":"vivoFullSize","stream":{"useCase":1})"));
    assert(bad(R"("backend":"java","stopRepeatingForStill":true)") && error.find("stop-repeating") != std::string::npos);
    assert(!bad(R"("vendorGate":"vivoFullSize")") && !bad(R"("backend":"ndk","stopRepeatingForStill":true)"));
}

void testOverride() {
    LensRoute plain;
    assert(applyBackendOverride(plain, "").empty() && plain.backend == BackendChoice::Auto);
    assert(applyBackendOverride(plain, "java") == "java" && effectiveBackend(plain) == BackendChoice::Java);
    assert(applyBackendOverride(plain, "ndk") == "ndk" && effectiveBackend(plain) == BackendChoice::Ndk);
    assert(applyBackendOverride(plain, "gpu") == "ignored value=gpu" && plain.backend == BackendChoice::Ndk);
    LensRoute gated;
    gated.vendorGate = "vivoFullSize";
    assert(applyBackendOverride(gated, "java") == "java" && effectiveBackend(gated) == BackendChoice::Java);
    LensRoute decision;
    decision.stopRepeatingForStill = true;
    assert(applyBackendOverride(decision, "java").find("kept") == 0 && effectiveBackend(decision) == BackendChoice::Ndk);
    LensRoute moded;
    moded.opMode = 32769;
    assert(applyBackendOverride(moded, "ndk").find("kept") == 0 && effectiveBackend(moded) == BackendChoice::Java);
}
}  // namespace

int main() {
    testOverride();
    testDerivedBackend();
    testJsonRoundTrip();
    testRejections();
    std::cout << "backend_choice_test passed\n";
    return 0;
}
