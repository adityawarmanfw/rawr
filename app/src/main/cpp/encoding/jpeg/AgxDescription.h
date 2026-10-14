#pragma once

#include <cmath>
#include <cstdio>
#include <string>

#include "tonemap/Agx.h"

namespace rawrcam::encoding::jpeg {

// AgX controls for ImageDescription, one "<prefix><label>: <value>" line per
// control. Labels, display scale, decimals and units mirror the app's
// AgxCatalog so the metadata reads like the AgX settings screen (degrees
// spelled out: EXIF ImageDescription is ASCII).
inline std::string formatAgxControls(const tonemap::AgxParams& params, const char* prefix) {
    enum Unit { Plain, Ev, Percent, Degrees };
    struct Spec {
        const char* label;
        Unit unit;
        int decimals;
        bool signedValue;  // range straddles zero: show "+"
    };
    static constexpr Spec kSpecs[tonemap::kAgxControlCount] = {
        {"White Relative Exposure", Ev, 1, false},
        {"Black Relative Exposure", Ev, 1, false},
        {"Contrast", Plain, 2, false},
        {"Shoulder Power", Plain, 2, false},
        {"Toe Power", Plain, 2, false},
        {"Pivot Relative Exposure", Ev, 2, true},
        {"Pivot Target Output", Percent, 1, false},
        {"Shoulder Start", Percent, 0, false},
        {"Toe Start", Percent, 0, false},
        {"Target White", Percent, 1, false},
        {"Target Black", Percent, 2, false},
        {"Curve Y Gamma", Plain, 2, false},
        {"Slope", Plain, 2, false},
        {"Lift", Plain, 2, true},
        {"Brightness", Plain, 2, false},
        {"Saturation", Percent, 0, false},
        {"Preserve Hue", Percent, 0, false},
        {"Red Attenuation", Percent, 0, false},
        {"Red Rotation", Degrees, 1, true},
        {"Green Attenuation", Percent, 0, false},
        {"Green Rotation", Degrees, 1, true},
        {"Blue Attenuation", Percent, 0, false},
        {"Blue Rotation", Degrees, 1, true},
        {"Master Purity Boost", Percent, 0, false},
        {"Master Rotation Reversal", Percent, 0, false},
        {"Red Purity Boost", Percent, 0, false},
        {"Red Reverse Rotation", Degrees, 1, true},
        {"Green Purity Boost", Percent, 0, false},
        {"Green Reverse Rotation", Degrees, 1, true},
        {"Blue Purity Boost", Percent, 0, false},
        {"Blue Reverse Rotation", Degrees, 1, true},
    };
    const auto values = tonemap::agxToControls(params);
    std::string out;
    char line[96];
    for (size_t i = 0; i < values.size(); ++i) {
        const Spec& spec = kSpecs[i];
        const float scale = spec.unit == Percent ? 100.0f : spec.unit == Degrees ? 57.29578f : 1.0f;
        const char* unit = spec.unit == Ev ? " EV" : spec.unit == Percent ? "%" : spec.unit == Degrees ? " deg" : "";
        std::snprintf(line, sizeof(line), spec.signedValue ? "%s%s: %+.*f%s\n" : "%s%s: %.*f%s\n", prefix,
                      spec.label, spec.decimals, values[i] * scale, unit);
        out += line;
    }
    return out;
}

}  // namespace rawrcam::encoding::jpeg
