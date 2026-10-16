#pragma once
// DNG-to-DNG conversion for the renderer's DNG library: lossless re-encode
// at full resolution and 2x natural Bayer binning (200MP -> 50MP). Both keep
// the source metadata; binning rewrites the resolution-dependent parts.
#include <tinydng.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace rawrcam::convert {

// done/total output tile rows of the current file. May be empty.
using ConvertProgress = std::function<void(uint32_t done, uint32_t total)>;
// Returns true to abandon the conversion. May be empty.
using ConvertCancelled = std::function<bool()>;

struct ConvertResult {
    bool ok = false;
    bool cancelled = false;
    std::string error;
    uint32_t width = 0, height = 0;
};

// Full-resolution lossless JPEG re-encode (pixels bit-exact).
ConvertResult recompressDng(const std::string& source, const std::string& destination,
                            const ConvertProgress& progress = {}, const ConvertCancelled& cancelled = {});

// 2x natural binning to a lossless JPEG DNG (10-bit sources keep a 4x code scale).
ConvertResult binDng2x(const std::string& source, const std::string& destination,
                       const ConvertProgress& progress = {}, const ConvertCancelled& cancelled = {});

// Raw metadata of a 2x binned image. Owns the rewritten opcode parameters.
class BinnedRaw {
   public:
    // gain: output code scale (WhiteLevel and BlackLevel scale with it).
    bool build(const tinydng_raw_info& source, const tinydng_cfa& cfa, uint32_t sourceWidth,
               uint32_t sourceHeight, uint16_t gain, std::string* error);
    const tinydng_raw_info& raw() const { return raw_; }
    const tinydng_cfa& cfa() const { return cfa_; }

   private:
    tinydng_raw_info raw_{};
    tinydng_cfa cfa_{};
    std::vector<tinydng_opcode> opcodes_;
    std::vector<std::vector<uint8_t>> params_;
};

}  // namespace rawrcam::convert
