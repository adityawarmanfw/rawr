#pragma once

#include <cmath>
#include <optional>
#include <vector>

namespace rawrcam::camera {

// What a capture result says about a key the profile set: "match" when the
// camera returned the values it was sent, "differs" when it returned others
// (it overrode or ignored the request), "missing" when the result doesn't
// carry the key (unverifiable: many vendor keys are request-only).
inline const char* keyResultVerdict(const std::vector<double>& sent, const std::optional<std::vector<double>>& got) {
    if (!got) return "missing";
    if (got->size() != sent.size()) return "differs";
    for (size_t i = 0; i < sent.size(); ++i)
        if (std::fabs((*got)[i] - sent[i]) > 1e-6 * std::fmax(1.0, std::fabs(sent[i]))) return "differs";
    return "match";
}

}  // namespace rawrcam::camera
