#include <cassert>
#include <iostream>
#include <utility>
#include <vector>

#include "camera/CompanionStreamSize.h"

using rawrcam::camera::pickCompanionSize;
using Sizes = std::vector<std::pair<int32_t, int32_t>>;

namespace {

const Sizes kTypical = {{4096, 3072}, {1920, 1080}, {1280, 720}, {640, 480}, {352, 288}, {320, 240}, {320, 180}, {176, 144}};

void fourThreeRawKeepsTheBasePick() {
    const auto c = pickCompanionSize(kTypical, 4096, 3072);
    assert(c.width == 320 && c.height == 240 && c.sameShape);
    // 4080x3064 is not exactly 4:3 but within 2%.
    const auto oppo = pickCompanionSize(kTypical, 4080, 3064);
    assert(oppo.width == 320 && oppo.height == 240 && oppo.sameShape);
}

void sixteenNineRawGetsASixteenNineCompanion() {
    const auto c = pickCompanionSize(kTypical, 4096, 2304);
    assert(c.width == 320 && c.height == 180 && c.sameShape);
    // Smallest same-shape size at least 320 wide, even when it's larger than the base pick.
    const auto c2 = pickCompanionSize({{320, 240}, {1280, 720}, {1920, 1080}}, 4096, 2304);
    assert(c2.width == 1280 && c2.height == 720 && c2.sameShape);
}

void noSameShapeSizeKeepsTheBasePick() {
    const auto c = pickCompanionSize({{640, 480}, {320, 240}}, 4096, 2304);
    assert(c.width == 320 && c.height == 240 && !c.sameShape);
}

void sameShapeBelowTheWidthFloorIsIgnored() {
    const auto c = pickCompanionSize({{320, 240}, {256, 144}}, 4096, 2304);
    assert(c.width == 320 && c.height == 240 && !c.sameShape);
}

void unknownRawSizeKeepsTheBasePick() {
    const auto c = pickCompanionSize(kTypical, 0, 0);
    assert(c.width == 320 && c.height == 240 && !c.sameShape);
}

void nothingAdvertised() {
    assert(pickCompanionSize({}, 4096, 3072).width == 0);
    assert(pickCompanionSize({{176, 144}}, 4096, 3072).width == 0);
}

}  // namespace

int main() {
    fourThreeRawKeepsTheBasePick();
    sixteenNineRawGetsASixteenNineCompanion();
    noSameShapeSizeKeepsTheBasePick();
    sameShapeBelowTheWidthFloorIsIgnored();
    unknownRawSizeKeepsTheBasePick();
    nothingAdvertised();
    std::cout << "companion_stream_size_test passed\n";
    return 0;
}
