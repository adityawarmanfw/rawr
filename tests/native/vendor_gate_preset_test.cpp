#include <cassert>
#include <iostream>

#include "camera/VendorGatePreset.h"

using namespace rawrcam::camera;

namespace {

// What captureFullSize built inline before the preset existed, kept as the oracle.
std::vector<CameraKeySetting> legacyVivoIds(double id) {
    std::vector<CameraKeySetting> keys;
    for (const auto& [tag, value] : {std::pair{"vivo.control.requestId", id}, std::pair{"vivo.control.frameFlag", id},
                                     std::pair{"vivo.control.globalCaptureId", id + 5}}) {
        CameraKeySetting k;
        k.tag = tag;
        k.type = CameraKeySetting::Type::Int64;
        k.scope = CameraKeySetting::Scope::Still;
        k.values = {value};
        keys.push_back(std::move(k));
    }
    return keys;
}

void testVivoStillIdsMatchTheOldInlineCode() {
    for (const double id : {0.0, 1.0, 1761644400123.0, 9007199254740991.0}) {
        const auto made = materializeStillKeys(gatePreset("vivoFullSize").stillKeys, id);
        const auto old = legacyVivoIds(id);
        assert(made.size() == old.size());
        for (size_t i = 0; i < old.size(); ++i) {
            assert(made[i].tag == old[i].tag && made[i].type == old[i].type && made[i].scope == old[i].scope);
            assert(made[i].values == old[i].values && made[i].enabled == old[i].enabled);
        }
    }
}

void testResultProbes() {
    const auto& probes = gatePreset("vivoFullSize").resultProbes;
    assert(probes.size() == 4);
    assert(probes[0].tag == "vcf.parameter.seamlessMode" && probes[0].alias == "seamlessMode");
    assert(probes[1].tag == "vivo.parameter.highResolutionDngType" && probes[1].alias.empty());
    assert(probes[2].tag == "vivo.control.seamless.remosaic.enable" && probes[2].alias.empty());
    assert(probes[3].tag == "vivo.control.sensorMode" && probes[3].alias.empty());
    // Only the seamless mode is kept on the frame.
    int aliased = 0;
    for (const auto& p : probes) aliased += !p.alias.empty();
    assert(aliased == 1);
}

void testOtherGatesBringNothing() {
    for (const char* gate : {"", "unknown", "someOtherGate"}) {
        assert(gatePreset(gate).stillKeys.empty() && gatePreset(gate).resultProbes.empty());
    }
}

void testTheVcfGateBringsTheSameStillKeysAndProbes() {
    const auto& plain = gatePreset("vivoFullSize");
    const auto& vcf = gatePreset("vivoFullSizeVcf");
    assert(vcf.stillKeys.size() == plain.stillKeys.size() && vcf.stillKeys.size() == 3);
    assert(vcf.resultProbes.size() == plain.resultProbes.size());
}

void testFixedKeysAreUntouched() {
    CameraKeySetting fixed;
    fixed.tag = "vendor.mode";
    fixed.values = {7.0};
    const auto made = materializeStillKeys({fixed}, 123.0);
    assert(made.size() == 1 && made[0].values == std::vector<double>{7.0});
}

}  // namespace

int main() {
    testVivoStillIdsMatchTheOldInlineCode();
    testResultProbes();
    testOtherGatesBringNothing();
    testTheVcfGateBringsTheSameStillKeysAndProbes();
    testFixedKeysAreUntouched();
    std::cout << "vendor_gate_preset_test passed\n";
    return 0;
}
