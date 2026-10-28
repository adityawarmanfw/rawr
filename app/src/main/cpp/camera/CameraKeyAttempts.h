#pragma once

#include <string>
#include <vector>

namespace rawrcam::camera {

// One setter call made while submitting a vendor key. A HAL tag has exactly
// one type and the metadata layer rejects every other, so the order tried and
// each status say which type the HAL declared. "cached" marks a first try that
// came from the per-tag memory of an earlier success, not from the profile.
struct KeyAttempt {
    const char* type = "";
    int status = 0;
    bool cached = false;
};

// "int32:-10001,byte(cached):0"
inline std::string describeKeyAttempts(const std::vector<KeyAttempt>& attempts) {
    if (attempts.empty()) return "none";
    std::string out;
    for (const auto& a : attempts) {
        if (!out.empty()) out += ',';
        out += a.type;
        if (a.cached) out += "(cached)";
        out += ':' + std::to_string(a.status);
    }
    return out;
}

}  // namespace rawrcam::camera
