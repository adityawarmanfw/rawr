// The presentation window of a crop (video framing, padded raw edges) in the rotated frame the shader samples.
#include <cassert>
#include <cmath>

#include "geometry/DisplayToSensorMapper.h"

using rawrcam::geometry::SourceCropRect;
using rawrcam::geometry::cropWindowFractions;

static bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

// What the window was before it could be off-centre: centred in the rotated frame, axes swapped on odd turns.
static void centredWindow(uint32_t sw, uint32_t sh, const SourceCropRect& crop, uint32_t turns, float& x0, float& y0,
                          float& x1, float& y1) {
    const bool swapped = turns & 1u;
    const float rotatedW = swapped ? float(sh) : float(sw), rotatedH = swapped ? float(sw) : float(sh);
    const float cropW = swapped ? float(crop.h) : float(crop.w), cropH = swapped ? float(crop.w) : float(crop.h);
    const float fx = cropW / rotatedW, fy = cropH / rotatedH;
    x0 = (1.0f - fx) * 0.5f;
    y0 = (1.0f - fy) * 0.5f;
    x1 = 1.0f - x0;
    y1 = 1.0f - y0;
}

int main() {
    // Every centred window (the video 4K / 1080p crops of the live preview) is unchanged, at every turn.
    const uint32_t sizes[][2] = {{4096, 3072}, {4080, 3064}, {2048, 1536}, {8192, 6144}};
    const uint32_t crops[][2] = {{3840, 2160}, {1920, 1080}, {3072, 2304}, {1280, 720}, {2048, 1536}};
    for (const auto& s : sizes)
        for (const auto& c : crops) {
            if (c[0] > s[0] || c[1] > s[1]) continue;
            const SourceCropRect crop{(s[0] - c[0]) / 2, (s[1] - c[1]) / 2, c[0], c[1], true};
            for (uint32_t turns = 0; turns < 4; ++turns) {
                float a[4], b[4];
                cropWindowFractions(s[0], s[1], crop, turns, a[0], a[1], a[2], a[3]);
                centredWindow(s[0], s[1], crop, turns, b[0], b[1], b[2], b[3]);
                for (int i = 0; i < 4; ++i) assert(near(a[i], b[i]));
            }
        }

    // A window that is not centred (padded bottom rows dropped: the top 2560 of 3072 rows) lands on the right edge
    // of the rotated frame for each turn.
    const SourceCropRect keepTop{0, 0, 4096, 2560, true};
    const float kept = 2560.0f / 3072.0f;
    float x0, y0, x1, y1;
    cropWindowFractions(4096, 3072, keepTop, 0, x0, y0, x1, y1);
    assert(near(x0, 0) && near(x1, 1) && near(y0, 0) && near(y1, kept));
    cropWindowFractions(4096, 3072, keepTop, 1, x0, y0, x1, y1);  // sourceUv = (q.y, 1 - q.x)
    assert(near(x0, 1 - kept) && near(x1, 1) && near(y0, 0) && near(y1, 1));
    cropWindowFractions(4096, 3072, keepTop, 2, x0, y0, x1, y1);  // (1 - q.x, 1 - q.y)
    assert(near(x0, 0) && near(x1, 1) && near(y0, 1 - kept) && near(y1, 1));
    cropWindowFractions(4096, 3072, keepTop, 3, x0, y0, x1, y1);  // (1 - q.y, q.x)
    assert(near(x0, 0) && near(x1, kept) && near(y0, 0) && near(y1, 1));

    // Disabled, empty or out-of-range windows leave the whole frame.
    for (const SourceCropRect& bad : {SourceCropRect{0, 0, 4096, 2560, false}, SourceCropRect{0, 0, 0, 100, true},
                                      SourceCropRect{0, 600, 4096, 2560, true}, SourceCropRect{10, 0, 4096, 100, true}}) {
        cropWindowFractions(4096, 3072, bad, 1, x0, y0, x1, y1);
        assert(near(x0, 0) && near(y0, 0) && near(x1, 1) && near(y1, 1));
    }
    return 0;
}
