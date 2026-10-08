#include <cmath>
#include <iostream>
#include <rcd/Balance.hpp>
#include <rcd/Pipeline.hpp>
#include <stdexcept>
#include <vector>

int main() {
    try {
        for (uint32_t pattern = 0; pattern < 4; ++pattern) {
            std::vector<float> raw(66 * 50);
            for (uint32_t y = 0; y < 50; ++y)
                for (uint32_t x = 0; x < 66; ++x) {
                    uint32_t px = (x & 1) ^ (pattern & 1), py = (y & 1) ^ ((pattern >> 1) & 1);
                    raw[y * 66 + x] = (px == py ? (px ? .3f : .15f) : .6f) * 255;
                }
            rcd::BalanceAccumulator frame;
            for (uint32_t y = 0; y < 50; y += 8)
                for (uint32_t x = 0; x < 66; x += 8)
                    frame.addBlock(raw.data() + y * 66 + x, 66, std::min(8u, 66 - x), std::min(8u, 50 - y),
                                   static_cast<rcd::BayerPattern>(pattern));
            auto g = frame.gains();
            const float expected[3] = {1, .25f, .5f};
            for (int i = 0; i < 3; ++i)
                if (std::abs(g[i] - expected[i]) > 1e-5f) throw std::runtime_error("frame balance/CFA mismatch");
            // A single clipped or black sample invalidates the whole block.
            auto before = frame.gains();
            raw[0] = 255;
            frame.addBlock(raw.data(), 66, 8, 8, static_cast<rcd::BayerPattern>(pattern));
            if (frame.gains() != before) throw std::runtime_error("clipped tile admitted");
            raw[0] = 0;
            frame.addBlock(raw.data(), 66, 8, 8, static_cast<rcd::BayerPattern>(pattern));
            if (frame.gains() != before) throw std::runtime_error("black tile admitted");
        }
        if (rcd::BalanceAccumulator{}.gains() != std::array<float, 3>{1, 1, 1})
            throw std::runtime_error("empty fallback");
        rcd::PipelineConfig config{};
        config.width = config.height = 32;
        config.inputBalance = {1, 0, 1};
        if (rcd::RcdPipeline::validateConfig(config)) throw std::runtime_error("invalid fixed gains accepted");
        std::cout << "RCD_BALANCE_PASS all Bayer patterns, frame gain, partial blocks, clipped/black rejection, empty "
                     "fallback, invalid gain rejection\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
