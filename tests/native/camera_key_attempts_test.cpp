#include <cassert>
#include <iostream>

#include "camera/CameraKeyAttempts.h"

using namespace rawrcam::camera;

int main() {
    assert(describeKeyAttempts({}) == "none");
    assert(describeKeyAttempts({{"int32", 0, false}}) == "int32:0");
    assert(describeKeyAttempts({{"int32", -10001, false}, {"byte", 0, false}}) == "int32:-10001,byte:0");
    assert(describeKeyAttempts({{"byte", 0, true}}) == "byte(cached):0");
    std::cout << "camera_key_attempts_test passed\n";
    return 0;
}
