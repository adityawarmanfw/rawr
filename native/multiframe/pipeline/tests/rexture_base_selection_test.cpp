#include <rawr/raw_merge_rexture_gpu/BaseSelection.h>

#include <cassert>

int main() {
    using namespace rawr::raw_merge_rexture_gpu;
    // window 0.25 s before the press at t = 1.0: frames at 0.70 (outside), 0.80, 0.90, 0.99
    std::vector<double> t{0.70, 0.80, 0.90, 0.99};
    std::vector<FrameScore> s{{100, 0}, {10, 0}, {10, 0.1}, {10, 0}};
    assert(chooseBaseAtPress(s, t, 1.0) == 3);  // sharp equal; the newest, no clipping, least age penalty
    s[1] = {40, 0};
    assert(chooseBaseAtPress(s, t, 1.0) == 1);  // 40 * (1 - .5 * .2/.25) = 24 beats 10
    s[1] = {40, 0.5};
    assert(chooseBaseAtPress(s, t, 1.0) == 3);  // clipping zeroes it: 40 * max(1 - 2 * .5, 0) = 0
    // no frame in the window: the latest before the press
    assert(chooseBaseAtPress(s, t, 2.0) == 3);
    // all frames after the press: the nearest
    assert(chooseBaseAtPress(s, t, 0.1) == 0);
    // ties go to the newest
    std::vector<FrameScore> same(4, FrameScore{5, 0});
    std::vector<double> t2{0.80, 0.80, 0.80, 0.80};
    assert(chooseBaseAtPress(same, t2, 1.0) == 3);
    return 0;
}
