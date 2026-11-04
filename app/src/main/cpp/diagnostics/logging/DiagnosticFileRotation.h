#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

namespace rawrcam::diagnostics {

// Diagnostics record all the time, so every file is bounded: past the cap the
// file moves to "<path>.1" (replacing the older one) and a fresh file starts.
// A new session rotates too, so the session before a crash/restart survives.
inline constexpr std::uint64_t kDiagnosticFileCapBytes = 4ull << 20;

inline void rotateDiagnosticFile(const std::string& path) {
    const std::string previous = path + ".1";
    std::remove(previous.c_str());
    std::rename(path.c_str(), previous.c_str());
}

}  // namespace rawrcam::diagnostics
