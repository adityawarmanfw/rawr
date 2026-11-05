#include <cassert>
#include <iostream>

#include "camera/CameraProfileJson.h"
#include "camera/CameraRouting.h"
#include "camera/CompanionStreamSize.h"

using namespace rawrcam::camera;

namespace {
using Sizes = std::vector<std::pair<int32_t, int32_t>>;

void testFallbackPolicies() {
    // A 21:9 RAW on a camera that advertises only 4:3, 16:9 and 1:1 outputs.
    const Sizes sizes = {{320, 240}, {640, 480}, {1280, 720}, {640, 640}, {1920, 1080}};
    const auto base = pickCompanionSize(sizes, 5040, 2160);
    assert(base.width == 320 && base.height == 240 && !base.sameShape);  // today's choice
    assert(pickCompanionSize(sizes, 5040, 2160, CompanionFallback::Base).width == 320);
    const auto none = pickCompanionSize(sizes, 5040, 2160, CompanionFallback::None);
    assert(none.width == 0 && none.height == 0);
    // 16:9 (1.778) is the closest to 21:9 (2.333) among the advertised shapes; the smaller 16:9 wins.
    const auto nearest = pickCompanionSize(sizes, 5040, 2160, CompanionFallback::Nearest);
    assert(nearest.width == 1280 && nearest.height == 720 && !nearest.sameShape);
    // A matching shape wins whatever the policy.
    for (const auto f : {CompanionFallback::Base, CompanionFallback::Nearest, CompanionFallback::None}) {
        const auto same = pickCompanionSize(sizes, 4096, 2304, f);
        assert(same.width == 1280 && same.height == 720 && same.sameShape);
        const auto four3 = pickCompanionSize(sizes, 4080, 3064, f);
        assert(four3.width == 320 && four3.height == 240 && four3.sameShape);
    }
    // Nothing advertised that qualifies.
    assert(pickCompanionSize({{160, 120}}, 4096, 3072, CompanionFallback::Nearest).width == 0);
    assert(pickCompanionSize({}, 4096, 3072, CompanionFallback::None).width == 0);
}

void testJsonRoundTrip() {
    std::string error;
    const auto parsed = parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"2",
        "companion":{"format":"private","targeted":false,"usage":"0x100","maxImages":4,"physical":false,"detach":true,
                     "fallback":"nearest"},
        "auxJpeg":{"width":8192,"height":6144,"usage":"0x3"},
        "reader":{"maxImages":12,"probe":true,"demote":true}}]})", &error);
    assert(parsed);
    const auto& r = parsed->lenses[0];
    assert(!r.companionSize && r.companionPrivate && !r.companionTargeted);
    assert(r.companionBufferUsage == 0x100u && r.companionBufferCount == 4u);
    assert(r.companionPhysical == false && r.companionDetach == true);
    assert(r.companionFallback == CompanionFallback::Nearest);
    assert(r.auxJpegSize == std::make_pair(8192u, 6144u) && r.auxJpegBufferUsage == 0x3u);
    assert(r.rawBufferCount == 12u && r.rawBufferTestFirst && r.rawBufferFallBack);
    assert(carriesRecipe(r));
    // A companion block without a size does not force a companion on.
    assert(!companionStreamEnabled(r, false, false));
    assert(companionStreamEnabled(r, false, true));
    const auto json = serializeCameraProfile(*parsed);
    const auto again = parseCameraProfile(json, &error);
    assert(again && serializeCameraProfile(*again) == json);
    assert(again->lenses[0].companionFallback == CompanionFallback::Nearest && again->lenses[0].rawBufferCount == 12u);

    // A sized companion is written as a fixed one.
    const auto sized = parseCameraProfile(
        R"({"lenses":[{"id":"a","cameraId":"2","companion":{"width":640,"height":480}}]})");
    assert(sized && sized->lenses[0].companionSize == std::make_pair(640u, 480u));
    assert(serializeCameraProfile(*sized).find(R"("companion":{"mode":"fixed","width":640,"height":480,"format":"yuv","targeted":true})") !=
           std::string::npos);
    // Defaults write nothing.
    const auto plain = serializeCameraProfile(*parseCameraProfile(R"({"lenses":[{"id":"a","cameraId":"2"}]})"));
    for (const char* field : {"companion", "auxJpeg", "reader"}) assert(plain.find(field) == std::string::npos);
}

void testRejections() {
    std::string error;
    const auto bad = [&](const char* fields) {
        return !parseCameraProfile(std::string(R"({"lenses":[{"id":"a","cameraId":"2",)") + fields + "}]}", &error);
    };
    assert(bad(R"("companion":{"fallback":"sideways"})") && error.find("fallback") != std::string::npos);
    assert(bad(R"("companion":{"width":640,"height":480,"fallback":"nearest"})") &&
           error.find("automatic size") != std::string::npos);
    assert(bad(R"("companion":{"maxImages":1})") && bad(R"("companion":{"maxImages":17})") &&
           bad(R"("companion":{"maxImages":2.5})"));
    assert(bad(R"("companion":{"usage":"cpu"})") && bad(R"("companion":{"physical":"yes"})") &&
           bad(R"("companion":{"detach":1})"));
    assert(bad(R"("companion":{"width":640})"));  // half a size is not an automatic size
    assert(bad(R"("auxJpeg":{"width":640,"height":480,"usage":"cpu"})"));
    assert(bad(R"("reader":{"maxImages":1})") && bad(R"("reader":{"maxImages":17})") && !bad(R"("reader":{"maxImages":4})") && !bad(R"("reader":{"maxImages":2})"));
    assert(!bad(R"("reader":{"maxImages":6})") && !bad(R"("reader":{"maxImages":16})"));
}

}  // namespace

int main() {
    testFallbackPolicies();
    testJsonRoundTrip();
    testRejections();
    std::cout << "companion_block_test passed\n";
    return 0;
}
