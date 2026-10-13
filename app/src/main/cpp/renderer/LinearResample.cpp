#include "LinearResample.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>
namespace rawrcam::renderer {
namespace {
struct Tap {
    int index;
    float weight;
};
float lanczos(float x) {
    x = std::abs(x);
    if (x < 1e-6f) return 1;
    if (x >= 3) return 0;
    float p = 3.14159265358979323846f * x;
    return std::sin(p) * std::sin(p / 3) / (p * p / 3);
}
std::vector<std::vector<Tap>> taps(uint32_t source, uint32_t destination, uint32_t start, uint32_t extent) {
    std::vector<std::vector<Tap>> table(destination);
    double scale = double(extent) / destination, filter = std::max(1.0, scale);
    for (uint32_t i = 0; i < destination; ++i) {
        double center = start + (i + .5) * scale - .5;
        double sum = 0;
        for (int j = int(std::ceil(center - 3 * filter)); j <= int(std::floor(center + 3 * filter)); ++j) {
            float w = lanczos(float((j - center) / filter));
            if (std::abs(w) < 1e-8f) continue;
            table[i].push_back({std::clamp(j, 0, int(source) - 1), w});
            sum += w;
        }
        for (auto& tap : table[i]) tap.weight = float(tap.weight / sum);
    }
    return table;
}
}  // namespace
void resampleLinear(const _Float16* source, uint32_t sw, uint32_t sh, _Float16* destination, uint32_t dw, uint32_t dh,
                    std::array<uint32_t, 4> crop, const std::function<void(uint32_t)>& rowDone) {
    // Native-size exports are an exact crop, not a filtering operation.
    if (dw == crop[2] && dh == crop[3]) {
        for (uint32_t y = 0; y < dh; ++y) {
            rowDone(y);  // Retain cancellation checks even on the copy path.
            std::memcpy(destination + size_t(y) * dw * 4,
                        source + (size_t(y + crop[1]) * sw + crop[0]) * 4,
                        size_t(dw) * 4 * sizeof(_Float16));
        }
        return;
    }
    const auto xt = taps(sw, dw, crop[0], crop[2]), yt = taps(sh, dh, crop[1], crop[3]);
    std::map<int, std::vector<float>> rows;
    for (uint32_t y = 0; y < dh; ++y) {
        const auto& vertical = yt[y];
        for (auto it = rows.begin(); it != rows.end();)
            if (it->first < vertical.front().index)
                it = rows.erase(it);
            else
                ++it;
        for (auto v : vertical)
            if (!rows.count(v.index)) {
                std::vector<float> row(size_t(dw) * 3);
                for (uint32_t x = 0; x < dw; ++x) {
                    float total[3]{}, lo[3]{INFINITY, INFINITY, INFINITY}, hi[3]{-INFINITY, -INFINITY, -INFINITY};
                    for (auto t : xt[x]) {
                        const auto* p = source + (size_t(v.index) * sw + t.index) * 4;
                        for (int c = 0; c < 3; ++c) {
                            float f = float(p[c]);
                            total[c] += f * t.weight;
                            lo[c] = std::min(lo[c], f);
                            hi[c] = std::max(hi[c], f);
                        }
                    }
                    for (int c = 0; c < 3; ++c) row[size_t(x) * 3 + c] = std::clamp(total[c], lo[c], hi[c]);
                }
                rows.emplace(v.index, std::move(row));
            }
        for (uint32_t x = 0; x < dw; ++x) {
            float total[3]{}, lo[3]{INFINITY, INFINITY, INFINITY}, hi[3]{-INFINITY, -INFINITY, -INFINITY};
            for (auto t : vertical) {
                auto* p = rows.at(t.index).data() + size_t(x) * 3;
                for (int c = 0; c < 3; ++c) {
                    total[c] += p[c] * t.weight;
                    lo[c] = std::min(lo[c], p[c]);
                    hi[c] = std::max(hi[c], p[c]);
                }
            }
            for (int c = 0; c < 3; ++c)
                destination[(size_t(y) * dw + x) * 4 + c] = _Float16(std::clamp(total[c], lo[c], hi[c]));
            destination[(size_t(y) * dw + x) * 4 + 3] = 1;
        }
        rowDone(y);
    }
}
}  // namespace rawrcam::renderer
