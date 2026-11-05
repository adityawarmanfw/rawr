#include <cassert>
#include <iostream>

#include "camera/CameraProfileJson.h"
#include "camera/CameraRouting.h"

using namespace rawrcam::camera;
using Scope = CameraKeySetting::Scope;

int main() {
    // A key goes out in the phases its scope names and in no other.
    const Scope all[] = {Scope::Session, Scope::Request, Scope::Still, Scope::Decision, Scope::SessionAndRequest};
    for (const Scope declared : all) {
        for (const Scope phase : {Scope::Session, Scope::Request, Scope::Still, Scope::Decision}) {
            const bool expected = declared == phase || (declared == Scope::SessionAndRequest &&
                                                        (phase == Scope::Session || phase == Scope::Request));
            assert(keyAppliesIn(declared, phase) == expected);
        }
    }
    // The existing scopes keep their meaning.
    assert(keyAppliesIn(Scope::Session, Scope::Session) && !keyAppliesIn(Scope::Session, Scope::Request));
    assert(keyAppliesIn(Scope::Request, Scope::Request) && !keyAppliesIn(Scope::Request, Scope::Session));

    std::string error;
    const auto parsed = parseCameraProfile(
        R"({"lenses":[{"id":"a","cameraId":"0","keys":[{"tag":"vendor.mode","type":"int32","scope":"sessionAndRequest","values":[3]}]}]})",
        &error);
    assert(parsed && parsed->lenses[0].vendorKeys.size() == 1 && parsed->lenses[0].vendorKeys[0].scope == Scope::SessionAndRequest);
    const auto json = serializeCameraProfile(*parsed);
    assert(json.find(R"("scope":"sessionAndRequest")") != std::string::npos);
    const auto again = parseCameraProfile(json);
    assert(again && again->lenses[0].vendorKeys[0].scope == Scope::SessionAndRequest && serializeCameraProfile(*again) == json);
    LensRoute lens = parsed->lenses[0];
    assert(carriesRecipe(lens));
    std::cout << "camera_key_scope_test passed\n";
    return 0;
}
