#pragma once
#include <rawr/raw_merge_common/RawMergeCommon.h>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

namespace rawr::raw_gpu_pipeline {
// Resolved numerical noise model, not a setting name: replay remains exact
// even after a device calibration is updated. Appends to opaque RZSL TSV.
inline std::string serializeReplayNoise(const rawr::raw_merge_common::Config& c) {
    std::ostringstream out;
    out << std::setprecision(9) << "rawrReplayNoiseV1\t" << (c.sensorNoiseProfile ? 1 : 0)
        << ' ' << c.noiseAlpha << ' ' << c.noiseBeta;
    auto write = [&](const auto& values) { for (float v : values) out << ' ' << v; };
    if (c.sensorNoiseProfile) {
        write(c.sensorNoiseProfile->slopeBySite); write(c.sensorNoiseProfile->offsetBySite);
        const auto high = c.highSignalNoiseProfile.value_or(*c.sensorNoiseProfile);
        write(high.slopeBySite); write(high.offsetBySite); write(c.noiseKneeBySite);
    }
    return out.str() + '\n';
}
inline bool applyReplayNoise(const std::string& metadata, rawr::raw_merge_common::Config& c) {
    std::istringstream lines(metadata);
    std::string line;
    while (std::getline(lines, line)) {
        const std::string key = "rawrReplayNoiseV1\t";
        if (line.rfind(key, 0) != 0) continue;
        auto candidate = c;
        candidate.sensorNoiseProfile.reset(); candidate.highSignalNoiseProfile.reset();
        candidate.noiseKneeBySite.fill(0.f);
        std::istringstream in(line.substr(key.size()));
        int hasProfile = -1;
        if (!(in >> hasProfile >> candidate.noiseAlpha >> candidate.noiseBeta) || (hasProfile != 0 && hasProfile != 1))
            throw std::invalid_argument("RZSL: invalid resolved noise header");
        auto read = [&](auto& values) { for (float& v : values) if (!(in >> v)) throw std::invalid_argument("RZSL: truncated resolved noise"); };
        if (hasProfile) {
            rawr::raw_merge_common::CfaNoiseProfile low{}, high{};
            read(low.slopeBySite); read(low.offsetBySite); read(high.slopeBySite); read(high.offsetBySite);
            read(candidate.noiseKneeBySite);
            candidate.sensorNoiseProfile = low; candidate.highSignalNoiseProfile = high;
        }
        if (!rawr::raw_merge_common::valid(candidate)) throw std::invalid_argument("RZSL: invalid resolved noise");
        c = candidate;
        return true;
    }
    return false;
}
}  // namespace rawr::raw_gpu_pipeline
