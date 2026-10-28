// Schema 2 groups a lens by what it configures; schema 1 files are upgraded into it. Both must give the same route.
#include <cassert>
#include <iostream>
#include <string>

#include "camera/CameraProfileJson.h"
#include "route_description.h"

using namespace rawrcam::camera;

namespace {
std::string describe(const std::string& json) {
    std::string error;
    const auto parsed = parseCameraProfile(json, &error);
    if (!parsed) std::cerr << error << '\n';
    assert(parsed && parsed->lenses.size() == 1);
    return testing::describeRouteForGolden(parsed->lenses[0]);
}

const char* kV1 = R"({"id":"user","lenses":[{"id":"T","cameraId":"0","physicalCameraId":"4",
  "stream":{"format":"RAW10","width":4096,"height":3072,"unlisted":true,"useCase":1},
  "readerUsage":"gpuDataBuffer","reader":{"maxImages":8,"probe":true,"demote":true},
  "viewfinderLevels":{"static":true,"black":[64],"white":1023},
  "captureStream":{"format":"RAW16","width":8192,"height":6144,"useCase":524547},
  "captureReaderUsage":"0x3","captureBurst":3,"captureBurstSequential":true,
  "captureLevels":{"static":true,"black":[256],"white":4095},
  "companion":{"width":640,"height":480,"format":"private","targeted":false,"usage":"0x100","maxImages":4,
               "physical":false,"detach":true,"useCase":4},
  "auxJpeg":{"width":4096,"height":3072,"usage":"none","useCase":2},
  "backend":"java","session":{"params":"full","opMode":32769},"template":"still","fps":30,"startupDelayMs":200,
  "quirks":["noTapAf"],"request":{"defaults":"minimal"},
  "keys":[{"tag":"x.y","type":"int32","scope":"sessionAndRequest","values":[1],"enabled":true}],
  "levels":{"static":true,"black":[64,64,64,64],"white":1023},
  "cfa":{"block":2,"order":"GRBG","live":"fast","photo":"standard"},"mirror":"horizontal",
  "exposure":{"extended":true,"isoMax":12800,"shutterMaxNs":2000000000}}]})";

const char* kV2 = R"({"schema":2,"id":"user","lenses":[{"id":"T","camera":{"id":"0","physical":"4"},
  "streams":{
    "raw":{"format":"RAW10","width":4096,"height":3072,"unlisted":true,"useCase":1,
           "reader":{"usage":"gpuDataBuffer","maxImages":8,"probe":true,"demote":true},
           "levels":{"static":true,"black":[64],"white":1023}},
    "capture":{"format":"RAW16","width":8192,"height":6144,"useCase":524547,"reader":{"usage":"0x3"},
               "burst":{"max":3,"sequential":true},"levels":{"static":true,"black":[256],"white":4095}},
    "companion":{"mode":"fixed","width":640,"height":480,"format":"private","targeted":false,"usage":"0x100",
                 "maxImages":4,"physical":false,"detach":true,"useCase":4},
    "auxJpeg":{"width":4096,"height":3072,"usage":"none","useCase":2}},
  "session":{"backend":"java","params":"full","opMode":32769,"template":"still","fps":30,"startupDelayMs":200,
             "quirks":["noTapAf"]},
  "request":{"defaults":"minimal"},
  "keys":[{"tag":"x.y","type":"int32","scope":"sessionAndRequest","values":[1],"enabled":true}],
  "image":{"levels":{"static":true,"black":[64,64,64,64],"white":1023},
           "cfa":{"block":2,"order":"GRBG","live":"fast","photo":"standard"},"mirror":"horizontal",
           "exposure":{"extended":true,"isoMax":12800,"shutterMaxNs":2000000000}}}]})";

void testSameRoute() {
    const auto v1 = describe(kV1);
    assert(v1 == describe(kV2));
    // Spot checks that the shared description really carries the values.
    assert(v1.find("stream=1:4096x3072 unlisted:1 useCase:1") != std::string::npos);
    assert(v1.find("captureStream=0:8192x6144 useCase:524547 burst:3 sequential:1") != std::string::npos);
    assert(v1.find("companionSize=640x480") != std::string::npos);
    assert(v1.find("reader=maxImages:8 probe:1 demote:1") != std::string::npos);
    assert(v1.find("key.session=x.y") != std::string::npos && v1.find("key.request=x.y") != std::string::npos);
}

void testGatedAndCompanionModes() {
    const auto gated = describe(R"({"schema":2,"lenses":[{"id":"F","camera":{"id":"3"},
        "streams":{"raw":{"format":"RAW16"},"capture":{"format":"RAW16","width":16320,"height":12256},
                   "companion":{"mode":"off"}},
        "session":{"gate":{"name":"vivoFullSize","hold":"closed"},"stopRepeatingForStill":true}}]})");
    assert(gated.find("gate=vivoFullSize hold:closed stopRepeatingForStill:1") != std::string::npos);
    assert(gated.find("companion=disabled:1") != std::string::npos);
    const auto v1Off = describe(R"({"lenses":[{"id":"F","cameraId":"3","stream":{"format":"RAW16"},
        "captureStream":{"format":"RAW16","width":16320,"height":12256},"disableCompanion":true,
        "vendorGate":"vivoFullSize","gateHold":"closed","stopRepeatingForStill":true}]})");
    assert(gated == v1Off);
    const auto automatic = describe(R"({"schema":2,"lenses":[{"id":"A","camera":{"id":"2"},
        "streams":{"companion":{"mode":"auto","fallback":"nearest","maxImages":4}}}]})");
    assert(automatic.find("companionSize=none") != std::string::npos && automatic.find("fallback:1") != std::string::npos);
}

void testRejections() {
    std::string error;
    const auto bad = [&](const std::string& json) { return !parseCameraProfile(json, &error); };
    assert(bad(R"({"schema":3,"lenses":[{"id":"a","camera":{"id":"2"}}]})") && error.find("schema") != std::string::npos);
    assert(bad(R"({"schema":2,"lenses":[{"id":"a","cameraId":"2"}]})") && error.find("cameraId") != std::string::npos);
    assert(bad(R"({"schema":2,"lenses":[{"id":"a","camera":{"id":"2"},"streams":{"companion":{"mode":"sometimes"}}}]})") &&
           error.find("companion mode") != std::string::npos);
    assert(bad(R"({"schema":2,"lenses":[{"id":"a","camera":{"id":"2"},"streams":{"companion":{"mode":"fixed"}}}]})"));
    assert(bad(R"({"schema":2,"lenses":[{"id":"a","camera":{"id":"2"},"session":{"gate":{"hold":"closed"}}}]})") &&
           error.find("needs a vendorGate") != std::string::npos);
    assert(bad(R"({"schema":2,"lenses":[{"id":"a","camera":{"id":"2"},"streams":{"capture":{"format":"RAW16"}}}]})") &&
           error.find("captureStream") != std::string::npos);
}
}  // namespace

int main() {
    testSameRoute();
    testGatedAndCompanionModes();
    testRejections();
    std::cout << "profile_schema_test passed\n";
    return 0;
}
