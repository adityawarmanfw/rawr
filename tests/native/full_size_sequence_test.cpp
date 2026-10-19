#include <cassert>

#include "camera/FullSizeSequence.h"
using rawrcam::camera::FullSizeSequence;
int main() {
    FullSizeSequence shot;
    assert(shot.begin(1, 10));
    assert(!shot.begin(1, 11));  // includes focus preparation ownership
    shot.submitted(31, 1000, 10'000'000);
    assert(!shot.expire(8000));     // the recorded 5–7 second valid results fit
    assert(!shot.complete(0, 31));  // retired generation
    assert(!shot.complete(1, 30));  // repeating/other sequence
    shot.abandonPreparation();
    assert(shot.busy());  // cannot forget submitted hardware on error
    assert(shot.expire(9010));
    assert(!shot.begin(1, 11));
    assert(shot.complete(1, 31) == 10);
    assert(!shot.begin(1, 11));  // timeout still requires session retirement
    shot.reset();
    assert(shot.begin(2, 11));
    shot.abandonPreparation();
    assert(shot.begin(2, 12));
    shot.submitted(1, 0, 30'000'000'000);
    assert(!shot.expire(37999));  // long exposure included in acquisition budget
    assert(shot.complete(2, 1) == 12);
    assert(shot.begin(2, 15));
    shot.submitted(2, 1000, 66'000'000, 3);
    assert(!shot.expire(9066));  // a queued burst is not given a single-frame timeout
    assert(!shot.expire(25197));
    assert(shot.expire(25198));
    assert(!shot.begin(2, 16));  // the entire timed-out sequence still owns capture
    shot.reset();
    assert(shot.begin(2, 13));
    shot.fail();
    assert(!shot.begin(2, 14));
}
