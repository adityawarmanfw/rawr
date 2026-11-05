#include <cassert>
#include <iostream>
#include <string>

#include "camera/CameraKeyResult.h"

using rawrcam::camera::keyResultVerdict;

int main() {
    assert(std::string(keyResultVerdict({7}, std::vector<double>{7})) == "match");
    assert(std::string(keyResultVerdict({7}, std::vector<double>{0})) == "differs");
    assert(std::string(keyResultVerdict({7}, std::nullopt)) == "missing");
    assert(std::string(keyResultVerdict({1, 2}, std::vector<double>{1})) == "differs");
    // Byte-string keys (a client name) compare in full.
    const std::vector<double> name = {99, 111, 109, 46, 97, 110, 100, 114, 111, 105, 100, 46, 99, 97, 109, 101, 114, 97, 0};
    assert(std::string(keyResultVerdict(name, name)) == "match");
    assert(std::string(keyResultVerdict({0.5}, std::vector<double>{0.5f})) == "match");
    std::cout << "camera_key_result_test passed\n";
    return 0;
}
