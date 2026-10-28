#include <cassert>
#include <fstream>
#include <iterator>
#include <iostream>
#include <string>
#include "camera/CameraProfileJson.h"
#include "metadata/FrameMetadataSnapshot.h"
#include "camera/CameraKeySubmission.h"
#include "camera/CameraStartLadder.h"

// The Xiaomi full-size recipe fields: template, fixed fps, companion size,
// auxiliary JPEG, numeric reader usages. They round-trip and reject bad values.
void testFullSizeRecipeJson() {
    using namespace rawrcam::camera;
    const std::string json = R"({"id":"user","lenses":[{"id":"T50","cameraId":"4",
        "stream":{"format":"RAW16","width":8160,"height":6144,"unlisted":true},
        "readerUsage":"0x100003","captureReaderUsage":"gpuDataBuffer","template":"still","fps":24,
        "captureStream":{"format":"RAW16","width":8160,"height":6144},
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
    assert(again.find(R"("reader":{"usage":"0x100003"})") != std::string::npos);

    for (const auto* bad : {"0x", "0x-1", "0x 1", "0x10000000000000000"}) {
        assert(!parseCameraProfile(std::string(R"({"lenses":[{"id":"a","cameraId":"0","readerUsage":")") + bad + R"("}]})"));
    }
    const auto preview = parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"0","template":"preview"}]})");
    assert(preview && preview->lenses[0].stillTemplate == false);
    assert(serializeCameraProfile(*preview).find("preview") != std::string::npos);
    assert(!parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"0","streamUseCase":524547}]})", &error));
    assert(error.find("not a profile field") != std::string::npos);
    // Defaults keep today's behaviour.
    const auto plain = parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"0"}]})");
    assert(plain && !plain->lenses[0].stillTemplate && plain->lenses[0].fixedFps == 0);
    assert(!plain->lenses[0].companionSize && !plain->lenses[0].auxJpegSize && !plain->lenses[0].captureReaderUsage);
    assert(!plain->lenses[0].disableCompanion);

    assert(!parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"0","template":"burst"}]})", &error));
    assert(!parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"0","companion":{"width":0,"height":480}}]})", &error));
    assert(!parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"0","readerUsage":"0xZZ"}]})", &error));
}

void testCompanionOptOut() {
    using namespace rawrcam::camera;
    const auto profile = parseCameraProfile(R"({"lenses":[{"id":"T50","cameraId":"4",
        "disableCompanion":true,"captureStream":{"width":8160,"height":6144}}]})");
    assert(profile);
    const auto& route = profile->lenses[0];
    assert(route.disableCompanion);
    for (bool forced : {false, true}) {
        for (bool automatic : {false, true}) assert(!companionStreamEnabled(route, forced, automatic));
    }
    assert(requiresStillExposureGuard(route));
    const auto encoded = serializeCameraProfile(*profile);
    const auto decoded = parseCameraProfile(encoded);
    assert(decoded && decoded->lenses[0].disableCompanion);
    assert(requiresStillExposureGuard(decoded->lenses[0]));
    assert(serializeCameraProfile(*decoded) == encoded);

    auto legacy = route;
    legacy.disableCompanion = false;
    assert(companionStreamEnabled(legacy, false, false)); // Existing vivo capture streams.
    assert(!requiresStillExposureGuard(legacy));
    legacy.companionSize = std::pair{640u, 480u};
    assert(companionStreamEnabled(legacy, false, false));
    assert(requiresStillExposureGuard(legacy));
    assert(!CameraStartLadder::eligible(true, false, true, false));
    legacy.companionSize.reset();
    legacy.captureStream.reset();
    assert(!companionStreamEnabled(legacy, false, false));
    assert(companionStreamEnabled(legacy, false, true));
    assert(companionStreamEnabled(legacy, true, false));
    legacy.disableCompanion = true;
    assert(!companionStreamEnabled(legacy, true, true)); // Full-size reference also stays RAW-only.
    assert(!requiresStillExposureGuard(legacy));

    for (const auto* fields : {
             R"("disableCompanion":"true")", R"("disableCompanion":1)", R"("disableCompanion":null)",
             R"("disableCompanion":true,"companion":{"width":640,"height":480})"}) {
        std::string error;
        assert(!parseCameraProfile(std::string(R"({"lenses":[{"id":"a","cameraId":"4",)") + fields + "}]}", &error));
        assert(error.find("disableCompanion") != std::string::npos);
    }
}


int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::ifstream input(argv[i]);
        assert(input.good());
        const std::string json((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        std::string error;
        const auto profile = rawrcam::camera::parseCameraProfile(json, &error);
        if (!profile) std::cerr << argv[i] << ": " << error << '\n';
        assert(profile);
        const auto encoded = rawrcam::camera::serializeCameraProfile(*profile);
        assert(rawrcam::camera::serializeCameraProfile(*rawrcam::camera::parseCameraProfile(encoded)) == encoded);
    }
    testFullSizeRecipeJson();
    testCompanionOptOut();
    using rawrcam::metadata::isCaptureStreamStill;
    assert(isCaptureStreamStill(true, 42));
    assert(!isCaptureStreamStill(false, 42));
    assert(!isCaptureStreamStill(true, 0));
    assert(!isCaptureStreamStill(true, rawrcam::metadata::kSensitivityProbeRequestId));
    assert(!isCaptureStreamStill(true, rawrcam::metadata::kFullSizeDecisionRequestId));
    using namespace rawrcam::camera;
    const auto levels = parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"0",
        "levels":{"static":true,"black":[64],"white":1023},
        "viewfinderLevels":{"static":true,"black":[512],"white":4095},
        "captureStream":{"format":"RAW16","width":8160,"height":6144},
        "captureLevels":{"static":false}}]})");
    assert(levels);
    const auto& route = levels->lenses[0];
    assert(staticLevelsForStream(route, false)->blackRggb[0] == 512);
    assert(!staticLevelsForStream(route, true));
    auto inherited = route;
    inherited.captureLevels.reset();
    assert(staticLevelsForStream(inherited, true)->white == 1023);
    const auto encoded = serializeCameraProfile(*levels);
    assert(serializeCameraProfile(*parseCameraProfile(encoded)) == encoded);
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
