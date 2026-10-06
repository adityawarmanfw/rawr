#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>

namespace rawrcam::geometry {

// Canonical CFA pattern shared by raw_preview, the DNG encoder and the
// demosaic backends. Integer codes match the app-wide convention
// (0=RGGB, 1=GRBG, 2=GBRG, 3=BGGR), which coincides with the Camera2
// COLOR_FILTER_ARRANGEMENT values and with every backend BayerPattern enum
// (rcd/vng4/dual::BayerPattern, raw_preview::BayerPattern are all 0..3
// ordered).
// Backend conversions range-check through here and static_cast, instead of
// each repeating the 0..3 table.
enum class CfaPattern : std::uint32_t { Rggb = 0, Grbg = 1, Gbrg = 2, Bggr = 3 };

[[nodiscard]] inline std::optional<CfaPattern> cfaPatternFromU32(std::uint32_t cfa) noexcept {
    switch (cfa) {
        case 0u:
            return CfaPattern::Rggb;
        case 1u:
            return CfaPattern::Grbg;
        case 2u:
            return CfaPattern::Gbrg;
        case 3u:
            return CfaPattern::Bggr;
        default:
            return std::nullopt;
    }
}

// Reorders a physical RGGB-ordered quad into pattern order. Single owner of
// the table previously triplicated as RcdStillProcessor::parityBlackLevels,
// DngMetadataAdapter::blackAtPattern and
// CameraMetadataReader::mapBlackPatternToPhysicalRggb (the permutation is an
// involution, so both directions share it). Out-of-range codes fall back to
// identity, matching the legacy default arms.
[[nodiscard]] inline std::array<float, 4> reorderRggbByCode(const std::array<float, 4>& physicalRggb,
                                                            std::uint32_t code) noexcept {
    switch (code) {
        case 0u:
            return {physicalRggb[0], physicalRggb[1], physicalRggb[2], physicalRggb[3]};
        case 1u:
            return {physicalRggb[1], physicalRggb[0], physicalRggb[3], physicalRggb[2]};
        case 2u:
            return {physicalRggb[2], physicalRggb[3], physicalRggb[0], physicalRggb[1]};
        case 3u:
            return {physicalRggb[3], physicalRggb[2], physicalRggb[1], physicalRggb[0]};
        default:
            return physicalRggb;
    }
}

// Pattern code seen from an origin offset by (x, y) sensor pixels; only the
// offset parity matters. Out-of-range codes behave as BGGR (legacy default).
[[nodiscard]] inline std::uint32_t shiftCfaPatternCode(std::uint32_t code, std::uint32_t x, std::uint32_t y) noexcept {
    const bool sx = (x & 1u) != 0;
    const bool sy = (y & 1u) != 0;
    if (!sx && !sy) return code;
    switch (code) {
        case 0u:
            return sy ? (sx ? 3u : 2u) : 1u;
        case 1u:
            return sy ? (sx ? 2u : 3u) : 0u;
        case 2u:
            return sy ? (sx ? 1u : 0u) : 3u;
        default:
            return sy ? (sx ? 0u : 1u) : 2u;
    }
}

// Row-major 2x2 color indices (0=R, 1=G, 2=B) for a pattern code, the TIFF/DNG
// CFAPattern byte layout. Out-of-range codes behave as BGGR (legacy default).
[[nodiscard]] inline std::array<std::uint8_t, 4> cfaColorIndices(std::uint32_t code) noexcept {
    switch (code) {
        case 0u:
            return {0, 1, 1, 2};
        case 1u:
            return {1, 0, 2, 1};
        case 2u:
            return {1, 2, 0, 1};
        default:
            return {2, 1, 1, 0};
    }
}

// Converts an app CFA code to a backend BayerPattern enum. All backend enums
// (rcd/vng4/dual::BayerPattern) are 0..3 ordered like CfaPattern; the layout
// is pinned per instantiation and out-of-range codes throw with the caller's
// message. Single owner of the table previously repeated per backend.
template <typename BayerPattern>
[[nodiscard]] BayerPattern toBackendPattern(std::uint32_t code, const char* what) {
    static_assert(
        static_cast<std::uint32_t>(BayerPattern::RGGB) == 0u && static_cast<std::uint32_t>(BayerPattern::BGGR) == 3u,
        "backend BayerPattern layout must match CfaPattern codes");
    if (!cfaPatternFromU32(code)) throw std::invalid_argument(what);
    return static_cast<BayerPattern>(code);
}

}  // namespace rawrcam::geometry
