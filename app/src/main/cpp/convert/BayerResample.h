#pragma once
// Non-integer Bayer downscale for the DNG converter (e.g. 50MP -> 25MP).
//
// Each output CFA site is resampled from the source samples of the same
// 2x2 buffer phase (R, G1, G2, B planes, each a lattice with a 2-pixel
// pitch), at the exact source position of the output pixel centre. The
// kernel is Catmull-Rom widened by the scale factor so it also low-passes
// each plane, and the result is clamped to the nearest same-site samples so
// its negative lobes cannot ring at edges. On a 200MP -> 25MP vivo frame it
// was 1.5 dB closer to a Lanczos downscale of the 50MP render than the softer
// Mitchell (B = C = 1/3), with the same colour error and no false colour.
// Output keeps the input's buffer phase and code scale.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace rawrcam::convert {

class BayerResample {
   public:
    // Output dimensions must be even; the scale per axis is input/output.
    bool configure(uint32_t inWidth, uint32_t inHeight, uint32_t outWidth, uint32_t outHeight, uint16_t maxCode) {
        if (outWidth < 2 || outHeight < 2 || (outWidth & 1u) || (outHeight & 1u) || outWidth > inWidth ||
            outHeight > inHeight || inWidth < 8 || inHeight < 8)
            return false;
        iw_ = inWidth;
        ih_ = inHeight;
        ow_ = outWidth;
        oh_ = outHeight;
        sx_ = double(inWidth) / outWidth;
        sy_ = double(inHeight) / outHeight;
        maxCode_ = maxCode;
        // Kernel support is 2 plane samples (4 source pixels) times the scale.
        rx_ = int(std::ceil(2.0 * std::max(1.0, sx_))) + 1;
        ry_ = int(std::ceil(2.0 * std::max(1.0, sy_))) + 1;
        return true;
    }

    uint32_t outputWidth() const { return ow_; }
    uint32_t outputHeight() const { return oh_; }

    // Input rows needed for output rows [oy, oy + rows).
    void sourceRows(uint32_t oy, uint32_t rows, uint32_t* top, uint32_t* bottom) const {
        const double first = centre(oy, sy_), last = centre(oy + rows - 1, sy_);
        const int pad = 2 * ry_ + 2;
        *top = uint32_t(std::clamp(int(std::floor(first)) - pad, 0, int(ih_)));
        *bottom = uint32_t(std::clamp(int(std::ceil(last)) + pad + 1, 0, int(ih_)));
    }

    // in(x, y) returns input codes for rows inside sourceRows().
    template <class In>
    void band(uint32_t oy, uint32_t rows, In in, uint16_t* out, size_t stride) const {
        // Column taps depend only on x; build them once per band.
        std::vector<std::vector<Tap>> columns(ow_);
        for (uint32_t x = 0; x < ow_; ++x) columns[x] = taps(centre(x, sx_), x & 1u, iw_, sx_, rx_);
        for (uint32_t y = 0; y < rows; ++y) {
            const uint32_t oyy = oy + y;
            const auto rowTaps = taps(centre(oyy, sy_), oyy & 1u, ih_, sy_, ry_);
            for (uint32_t x = 0; x < ow_; ++x) {
                double sum = 0;
                for (const auto& r : rowTaps) {
                    double line = 0;
                    for (const auto& c : columns[x]) line += c.weight * in(c.index, r.index);
                    sum += r.weight * line;
                }
                // Clamp to the nearest same-site samples (within one plane pitch).
                double lo = INFINITY, hi = -INFINITY;
                const double px = centre(x, sx_), py = centre(oyy, sy_);
                for (const auto& r : rowTaps) {
                    if (std::fabs(double(r.index) - py) >= 2.0) continue;
                    for (const auto& c : columns[x]) {
                        if (std::fabs(double(c.index) - px) >= 2.0) continue;
                        const double v = in(c.index, r.index);
                        lo = std::min(lo, v);
                        hi = std::max(hi, v);
                    }
                }
                if (lo <= hi) sum = std::clamp(sum, lo, hi);
                out[size_t(y) * stride + x] = uint16_t(std::clamp(sum, 0.0, double(maxCode_)) + 0.5);
            }
        }
    }

   private:
    struct Tap {
        uint32_t index;
        double weight;
    };

    static double centre(uint32_t o, double scale) { return (o + 0.5) * scale - 0.5; }

    static double cubic(double x) {
        constexpr double B = 0.0, C = 0.5;
        x = std::fabs(x);
        if (x < 1)
            return ((12 - 9 * B - 6 * C) * x * x * x + (-18 + 12 * B + 6 * C) * x * x + (6 - 2 * B)) / 6;
        if (x < 2)
            return ((-B - 6 * C) * x * x * x + (6 * B + 30 * C) * x * x + (-12 * B - 48 * C) * x + (8 * B + 24 * C)) / 6;
        return 0;
    }

    // Same-phase input positions around source coordinate p, normalized weights.
    // Plane distance is (input - p) / 2; the kernel is widened by the scale.
    static std::vector<Tap> taps(double p, uint32_t phase, uint32_t size, double scale, int radius) {
        std::vector<Tap> out;
        const double width = std::max(1.0, scale);
        int start = int(std::floor(p)) - 2 * radius;
        start += ((start & 1) != int(phase)) ? 1 : 0;
        double total = 0;
        for (int i = start; i <= int(std::ceil(p)) + 2 * radius; i += 2) {
            const double w = cubic((i - p) / 2.0 / width);
            if (w == 0) continue;
            // Reflect at the borders within the same phase.
            int j = i;
            const int last = int(size) - 2 + int(phase);
            while (j < int(phase) || j > last) j = j < int(phase) ? 2 * int(phase) - j : 2 * last - j;
            out.push_back({uint32_t(j), w});
            total += w;
        }
        for (auto& t : out) t.weight /= total;
        return out;
    }

    uint32_t iw_ = 0, ih_ = 0, ow_ = 0, oh_ = 0;
    double sx_ = 1, sy_ = 1;
    int rx_ = 3, ry_ = 3;
    uint16_t maxCode_ = 65535;
};

}  // namespace rawrcam::convert
