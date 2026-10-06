#include "encoding/dng/DngGainMapOpcode.h"

#include <cmath>
#include <cstring>
#include <vector>

#include "geometry/CfaPattern.h"

namespace rawrcam::encoding::dng {
namespace {

// --- GainMap opcode params (big-endian), ported from the retired native
// DNG writer (opcode serializer + Camera2 gain-map adapter). ---
void be32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xffu));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xffu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xffu));
    out.push_back(static_cast<uint8_t>(v & 0xffu));
}
void be64(std::vector<uint8_t>& out, uint64_t v) {
    for (int s = 56; s >= 0; s -= 8) out.push_back(static_cast<uint8_t>((v >> s) & 0xffu));
}
void beFloat(std::vector<uint8_t>& out, float v) {
    uint32_t u;
    std::memcpy(&u, &v, 4);
    be32(out, u);
}
void beDouble(std::vector<uint8_t>& out, double v) {
    uint64_t u;
    std::memcpy(&u, &v, 8);
    be64(out, u);
}

// GainMap params for one channel; returns false on invalid input.
bool appendGainMapChannel(std::vector<uint8_t>& params, const DngGainMapArea& area, int channel, uint32_t mapRows,
                          uint32_t mapCols, const std::vector<float>& interleaved) {
    const auto src = rawrcam::geometry::cfaColorIndices(area.storedPattern);
    // Phase of `channel` (0=R,1=Ge,2=Go,3=B) within ActiveArea origin.
    int want = -1;
    uint32_t pr = 0, pc = 0;
    for (uint32_t r = 0; r < 2; ++r)
        for (uint32_t c = 0; c < 2; ++c) {
            const uint8_t color = src[((r + (area.top & 1u)) & 1u) * 2u + ((c + (area.left & 1u)) & 1u)];
            if (channel == 0 && color == 0) {
                want = 0;
                pr = r;
                pc = c;
            }
            if (channel == 3 && color == 2) {
                want = 3;
                pr = r;
                pc = c;
            }
            if (channel == 1 && color == 1 && r == 0) {
                want = 1;
                pr = r;
                pc = c;
            }
            if (channel == 2 && color == 1 && r == 1) {
                want = 2;
                pr = r;
                pc = c;
            }
        }
    if (want != channel) return false;
    const double spacingH = 1.0 / double(mapCols - 1u);
    const double spacingV = 1.0 / double(mapRows - 1u);
    be32(params, pr);
    be32(params, pc);
    be32(params, area.height);
    be32(params, area.width);
    be32(params, 0);  // plane
    be32(params, 1);  // planes
    be32(params, 2);  // rowPitch
    be32(params, 2);  // colPitch
    be32(params, mapRows);
    be32(params, mapCols);
    beDouble(params, spacingV);
    beDouble(params, spacingH);
    beDouble(params, 0.0);
    beDouble(params, 0.0);
    be32(params, 1);  // mapPlanes
    for (uint32_t y = 0; y < mapRows; ++y)
        for (uint32_t x = 0; x < mapCols; ++x)
            beFloat(params, interleaved[(size_t(y) * mapCols + x) * 4u + size_t(channel)]);
    return true;
}

}  // namespace

bool appendLensShadingGainMaps(TinyDngWriteParams& p, const rawrcam::metadata::FrameMetadataSnapshot& metadata,
                               const DngGainMapArea& area, std::string* error) {
    const auto& ls = metadata.lensShadingMap;
    const uint32_t cols = metadata.lensShadingMapWidth;
    const uint32_t rows = metadata.lensShadingMapHeight;
    const uint64_t points = uint64_t(cols) * uint64_t(rows);
    bool valid = cols >= 2 && rows >= 2 && points <= 65536u && ls.size() == size_t(points) * 4u;
    if (valid)
        for (float v : ls)
            if (!std::isfinite(v) || v < 1.0f) {
                valid = false;
                break;
            }
    if (!valid) {
        if (error) *error = "lens shading: invalid gain map";
        return false;
    }
    for (int ch = 0; ch < 4; ++ch) {
        std::vector<uint8_t> params;
        if (!appendGainMapChannel(params, area, ch, rows, cols, ls)) {
            if (error) *error = "lens shading: CFA phase resolution failed";
            return false;
        }
        p.opcodeParams.push_back(std::move(params));
    }
    p.opcodes.reserve(4);
    for (int ch = 0; ch < 4; ++ch) {
        tinydng_opcode op{};
        op.list = 2;
        op.id = 9;
        op.version = 0x01030000u;
        op.flags = 1;  // optional, matches legacy writer policy
        op.params = p.opcodeParams[size_t(ch)].data();
        op.params_size = p.opcodeParams[size_t(ch)].size();
        p.opcodes.push_back(op);
    }
    p.raw.opcodes = p.opcodes.data();
    p.raw.opcode_count = p.opcodes.size();
    return true;
}

}  // namespace rawrcam::encoding::dng
