#pragma once

#include <string>
#include <vector>

#include "camera/CameraRouting.h"

namespace rawrcam::camera {

// A result entry read from a full-size still's capture result and logged. The alias says what the app does
// with it: "seamlessMode" is kept on the frame (it shows whether the HAL really switched sensor mode); an
// empty alias is only logged.
struct ResultProbe {
    std::string tag;
    std::string alias;
};

// What a vendor gate brings to a lens that carries it, so profiles, imported ones included, do not repeat it.
struct GatePreset {
    // Still-scope keys whose value is computed per capture (see CameraKeySetting::Generator).
    std::vector<CameraKeySetting> stillKeys;
    std::vector<ResultProbe> resultProbes;
};

[[nodiscard]] inline const GatePreset& gatePreset(const std::string& gate) {
    static const GatePreset none;
    static const GatePreset vivoFullSize = [] {
        GatePreset p;
        // vivo's per-still ids, its camera app's values: request id = frame flag = a millisecond stamp, global
        // capture id = stamp + 5. A still without them crashes the provider's request metadata handling.
        const std::pair<const char*, double> ids[] = {
            {"vivo.control.requestId", 0}, {"vivo.control.frameFlag", 0}, {"vivo.control.globalCaptureId", 5}};
        for (const auto& [tag, offset] : ids) {
            CameraKeySetting k;
            k.tag = tag;
            k.type = CameraKeySetting::Type::Int64;
            k.scope = CameraKeySetting::Scope::Still;
            k.generator = CameraKeySetting::Generator::StampMs;
            k.generatorOffset = offset;
            p.stillKeys.push_back(std::move(k));
        }
        p.resultProbes = {{"vcf.parameter.seamlessMode", "seamlessMode"},
                          {"vivo.parameter.highResolutionDngType", ""},
                          {"vivo.control.seamless.remosaic.enable", ""},
                          {"vivo.control.sensorMode", ""}};
        return p;
    }();
    return gate == "vivoFullSize" || gate == "vivoFullSizeVcf" ? vivoFullSize : none;
}

// The keys with their per-capture values filled in from the capture's stamp.
[[nodiscard]] inline std::vector<CameraKeySetting> materializeStillKeys(std::vector<CameraKeySetting> keys, double stamp) {
    for (auto& k : keys)
        if (k.generator == CameraKeySetting::Generator::StampMs) k.values = {stamp + k.generatorOffset};
    return keys;
}

}  // namespace rawrcam::camera
