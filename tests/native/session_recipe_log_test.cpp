#include <cassert>
#include <iostream>
#include <string>

#include "camera/CameraRouting.h"
#include "camera/SessionRecipeLog.h"

using namespace rawrcam::camera;

namespace {
bool has(const std::string& line, const std::string& part) { return line.find(part) != std::string::npos; }

void testPlainLens() {
    SessionRecipeFacts f;
    f.lensId = "2x";
    f.cameraId = "2";
    f.rawFormat = "RAW16";
    f.rawWidth = 4080;
    f.rawHeight = 3064;
    f.requestTemplate = "preview";
    f.fpsRange = "12..30";
    f.requestKeys = 1;
    f.companion = SessionRecipeFacts::Companion{"YUV_420_888", 320, 240, true, "profile", "fallback"};
    f.readerUsage = 0x1000003;
    const std::string line = describeSessionRecipe(f);
    assert(line.rfind("CAMERA_SESSION_RECIPE ", 0) == 0);
    assert(has(line, "lens=2x camera=2 physical=none raw=RAW16:4080x3064 aspect=1.332"));
    assert(has(line, "template=preview fps=12..30 sessionParameters=1"));
    assert(has(line, "keys=session:0,request:1,still:0,decision:0"));
    assert(has(line, "companion=YUV_420_888:320x240:targeted:source=profile:shape=fallback"));
    assert(has(line, "auxJpeg=none captureStream=none readerUsage=0x1000003 captureReaderUsage=default"));
    assert(has(line, "gate=none still=none stopRepeatingForStill=0"));
}

void testFullSizeLens() {
    SessionRecipeFacts f;
    f.lensId = "F35";
    f.cameraId = "3";
    f.rawFormat = "RAW16";
    f.rawWidth = 4080;
    f.rawHeight = 3064;
    f.requestTemplate = "still";
    f.fpsRange = "none";
    f.sessionParameters = false;
    f.sessionKeys = 2;
    f.decisionKeys = 1;
    f.companion = SessionRecipeFacts::Companion{"PRIVATE", 640, 480, false, "profile", "raw"};
    f.auxJpeg = std::make_pair(8192u, 6144u);
    f.captureStream = std::make_pair(16320u, 12256u);
    f.captureReaderUsage = 1ull << 24;
    f.vendorGate = "vivoFullSize";
    f.gateHold = "closed";
    f.stillStrategy = "decisionFrame";
    f.stopRepeatingForStill = true;
    f.startupDelayMs = 500;
    f.fixedFps = 30;
    const std::string line = describeSessionRecipe(f);
    assert(has(line, "sessionParameters=0"));
    assert(has(line, "keys=session:2,request:0,still:0,decision:1"));
    assert(has(line, "companion=PRIVATE:640x480:untargeted:source=profile:shape=raw"));
    assert(has(line, "auxJpeg=8192x6144 captureStream=16320x12256"));
    assert(has(line, "readerUsage=ladder captureReaderUsage=0x1000000"));
    assert(has(line, "startupDelayMs=500 fixedFps=30 gate=vivoFullSize:closed still=decisionFrame stopRepeatingForStill=1"));
}

void testCarriesRecipe() {
    LensRoute plain;
    assert(!carriesRecipe(plain));
    LensRoute keyed;
    keyed.keys.push_back(CameraKeySetting{});
    assert(carriesRecipe(keyed));
    LensRoute fps;
    fps.fixedFps = 30;
    assert(carriesRecipe(fps));
    LensRoute noCompanion;
    noCompanion.disableCompanion = true;
    assert(carriesRecipe(noCompanion));
    LensRoute fullSize;
    fullSize.captureStream = rawrcam::geometry::RawStreamPreference{};
    assert(carriesRecipe(fullSize));
}

void testMissingFacts() {
    const std::string line = describeSessionRecipe({});
    assert(has(line, "lens=none camera=none physical=none raw=none:0x0 aspect=unknown"));
    assert(has(line, "companion=none"));
}
}  // namespace

int main() {
    testPlainLens();
    testFullSizeLens();
    testMissingFacts();
    testCarriesRecipe();
    std::cout << "session_recipe_log_test passed\n";
    return 0;
}
