#include <cassert>
#include <string>
#include "camera/CameraProfileJson.h"
#include "camera/CameraKeySubmission.h"

// The Xiaomi full-size recipe fields: template, fixed fps, companion size,
// auxiliary JPEG, numeric reader usages. They round-trip and reject bad values.
void testFullSizeRecipeJson() {
    using namespace rawrcam::camera;
    const std::string json = R"({"id":"user","lenses":[{"id":"T50","cameraId":"4",
        "stream":{"format":"RAW16","width":8160,"height":6144,"unlisted":true},
        "readerUsage":"0x100003","captureReaderUsage":"gpuDataBuffer","template":"still","fps":24,
        "companion":{"width":640,"height":480,"format":"private","targeted":false},"startupDelayMs":500,"stopRepeatingForStill":true,"auxJpeg":{"width":8160,"height":6144}}]})";
    std::string error;
    const auto parsed = parseCameraProfile(json, &error);
    assert(parsed && error.empty());
    const auto& r = parsed->lenses[0];
    assert(r.readerUsage && *r.readerUsage == 0x100003u);
    assert(r.captureReaderUsage && *r.captureReaderUsage == kReaderUsageGpuDataBuffer);
    assert(r.stillTemplate == true && r.fixedFps == 24);
    assert(r.companionPrivate && !r.companionTargeted && r.startupDelayMs == 500 && r.stopRepeatingForStill);
    assert(r.companionSize && r.companionSize->first == 640 && r.companionSize->second == 480);
    assert(r.auxJpegSize && r.auxJpegSize->first == 8160 && r.auxJpegSize->second == 6144);
    const auto again = serializeCameraProfile(*parsed);
    assert(serializeCameraProfile(*parseCameraProfile(again)) == again);
    assert(again.find("\"readerUsage\":\"0x100003\"") != std::string::npos);

    for (const auto* bad : {"0x", "0x-1", "0x 1", "0x10000000000000000"}) {
        assert(!parseCameraProfile(std::string(R"({"lenses":[{"id":"a","cameraId":"0","readerUsage":")") + bad + R"("}]})"));
    }
    const auto preview = parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"0","template":"preview"}]})");
    assert(preview && preview->lenses[0].stillTemplate == false);
    assert(serializeCameraProfile(*preview).find("preview") != std::string::npos);
    assert(!parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"0","streamUseCase":524547}]})", &error));
    assert(error.find("NDK") != std::string::npos);
    // Defaults keep today's behaviour.
    const auto plain = parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"0"}]})");
    assert(plain && !plain->lenses[0].stillTemplate && plain->lenses[0].fixedFps == 0);
    assert(!plain->lenses[0].companionSize && !plain->lenses[0].auxJpegSize && !plain->lenses[0].captureReaderUsage);

    assert(!parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"0","template":"burst"}]})", &error));
    assert(!parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"0","companion":{"width":0,"height":480}}]})", &error));
    assert(!parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"0","readerUsage":"0xZZ"}]})", &error));
}


int main() {
    testFullSizeRecipeJson();
    using rawrcam::camera::submitCameraKey;
    int calls = 0;
    const auto setter = [&] { ++calls; return 0; };
    const auto unlisted = submitCameraKey(true, true, true, "", setter);
    assert(calls == 1 && unlisted.failure.empty() && unlisted.warnUnlistedSession);
    const auto listed = submitCameraKey(true, true, false, "", setter);
    assert(calls == 2 && listed.failure.empty() && !listed.warnUnlistedSession);
    assert(!submitCameraKey(false, true, true, "missing", setter).failure.empty());
    assert(!submitCameraKey(true, false, true, "", setter).failure.empty());
    assert(calls == 2);
    assert(submitCameraKey(true, true, true, "", [] { return -22; }).failure == "set_status=-22");
}
