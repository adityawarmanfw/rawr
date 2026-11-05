// Every built-in lens parses to the same route as when the fixture was written. Guards changes to the profile
// format and to the built-in files: run with --write <fixture> only when a route is meant to change.
#include <cassert>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "camera/CameraProfileJson.h"
#include "route_description.h"

using namespace rawrcam::camera;

namespace {
std::string describeAll() {
    static const char* const sources[] = {
#include "camera/BuiltInCameraProfiles.inc"
    };
    std::string out;
    for (const char* source : sources) {
        std::string error;
        const auto parsed = parseBuiltInProfile(source, &error);
        if (!parsed) std::cerr << "built-in profile rejected: " << error << '\n';
        assert(parsed);
        out += "# profile " + parsed->profile.id + '\n';
        for (const auto& lens : parsed->profile.lenses) out += testing::describeRouteForGolden(lens) + '\n';
        // The profile must also survive its own serialization unchanged.
        const auto reparsed = parseCameraProfile(serializeCameraProfile(parsed->profile), &error);
        if (!reparsed) std::cerr << "round trip rejected: " << error << '\n';
        assert(reparsed && reparsed->lenses.size() == parsed->profile.lenses.size());
        for (size_t i = 0; i < reparsed->lenses.size(); ++i)
            assert(testing::describeRouteForGolden(reparsed->lenses[i]) ==
                   testing::describeRouteForGolden(parsed->profile.lenses[i]));
    }
    return out;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--write") {
        std::ofstream(argv[2]) << describeAll();
        std::cout << "wrote " << argv[2] << '\n';
        return 0;
    }
    assert(argc == 2);
    std::ifstream in(argv[1]);
    std::stringstream expected;
    expected << in.rdbuf();
    const std::string actual = describeAll();
    if (actual != expected.str()) {
        std::ofstream("builtin_routes.actual.txt") << actual;
        std::cerr << "built-in routes differ from " << argv[1] << "; see builtin_routes.actual.txt\n";
        return 1;
    }
    std::cout << "builtin_route_golden_test passed\n";
    return 0;
}
