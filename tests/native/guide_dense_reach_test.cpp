#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <utility>
#include <vector>

// Dense smoothing relies on every cell receiving colour after the complete
// jump-flood schedule, even from a single seed. Extra seeds only add coverage.
static void check(int width, int height, int seed) {
    std::vector<uint8_t> src(width * height), dst(src.size());
    src[seed] = 1;
    int step = 1;
    while (step * 2 < std::max(width, height)) step *= 2;
    for (;;) {
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                bool found = src[y * width + x];
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = x + dx * step, ny = y + dy * step;
                        if (nx >= 0 && nx < width && ny >= 0 && ny < height)
                            found = found || src[ny * width + nx];
                    }
                dst[y * width + x] = found;
            }
        }
        src.swap(dst);
        if (step == 1) break;
        step /= 2;
    }
    assert(std::all_of(src.begin(), src.end(), [](uint8_t v) { return v != 0; }));
}

int main() {
    for (int height = 1; height <= 12; ++height)
        for (int width = 1; width <= 12; ++width)
            for (int seed = 0; seed < width * height; ++seed) check(width, height, seed);
    for (auto [width, height] : {std::pair{1, 257}, {257, 1}, {2, 257}, {257, 2},
                                 {127, 65}, {65, 127}, {510, 383}})
        for (int seed : {0, width - 1, width * (height - 1), width * height - 1,
                         (height / 2) * width + width / 2})
            check(width, height, seed);
    std::cout << "GUIDE_DENSE_REACH_PASS\n";
}
