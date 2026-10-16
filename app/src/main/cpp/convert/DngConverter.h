#pragma once
// DNG-to-DNG conversion for the renderer's DNG library: lossless re-encode
// at full resolution, and Bayer downscales that keep a CFA DNG (natural 2x
// binning, chained for 4x, plus a per-site resample for the 2.83x step).
// Source metadata is carried; the resolution-dependent parts are rewritten.
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

// Output size as a fraction of the source pixel count.
enum class DngDownscale {
    Quarter,    // 2x per side (200MP -> 50MP): one natural bin
    Eighth,     // 2.83x per side (200MP -> 25MP): natural bin, then per-site resample
    Sixteenth,  // 4x per side (200MP -> 12.5MP): two natural bins
};

// Full-resolution lossless JPEG re-encode (pixels bit-exact).
ConvertResult recompressDng(const std::string& source, const std::string& destination,
                            const ConvertProgress& progress = {}, const ConvertCancelled& cancelled = {});

// Downscaled lossless JPEG DNG. 10-bit sources keep the precision binning
// gains as a 4x code scale (WhiteLevel and BlackLevel scale with it).
ConvertResult downscaleDng(const std::string& source, const std::string& destination, DngDownscale scale,
                           const ConvertProgress& progress = {}, const ConvertCancelled& cancelled = {});

// Raw metadata of a downscaled image. Owns the rewritten opcode parameters.
class BinnedRaw {
   public:
    // Source (sw x sh) -> output (ow x oh); gain is the output code scale.
    bool build(const tinydng_raw_info& source, const tinydng_cfa& cfa, uint32_t sourceWidth,
               uint32_t sourceHeight, uint32_t outputWidth, uint32_t outputHeight, uint16_t gain,
               std::string* error);
    const tinydng_raw_info& raw() const { return raw_; }
    const tinydng_cfa& cfa() const { return cfa_; }

   private:
    tinydng_raw_info raw_{};
    tinydng_cfa cfa_{};
    std::vector<tinydng_opcode> opcodes_;
    std::vector<std::vector<uint8_t>> params_;
};

}  // namespace rawrcam::convert
