#include "raw_highlight/SpirvLocalSize.hpp"

#include <cassert>
#include <fstream>
#include <iostream>
#include <vector>

using rawr::highlight::literalLocalSize2D;

// Minimal instruction streams exercise parsing, not Vulkan module validation.
static std::vector<uint32_t> module(uint32_t x, uint32_t y) {
    return {0x07230203u, 0x00010000u, 0u, 2u, 0u,
            (5u << 16u) | 15u, 5u, 1u, 0x6e69616du, 0u,
            (6u << 16u) | 16u, 1u, 17u, x, y, 1u};
}

static void rejected(const std::vector<uint32_t>& words) {
    bool threw = false;
    try {
        (void)literalLocalSize2D({words.data(), words.size()});
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
}

int main(int argc, char** argv) {
    for (auto shape : {std::array<uint32_t, 2>{8, 8}, {16, 16}, {16, 8}, {1, 256}}) {
        auto words = module(shape[0], shape[1]);
        assert(literalLocalSize2D({words.data(), words.size()}) == shape);
    }
    rejected({});
    auto bad = module(8, 8);
    bad[0] = 0;
    rejected(bad);
    bad = module(8, 8);
    bad[5] = 0;  // zero word count must never loop forever
    rejected(bad);
    bad = module(8, 8);
    bad.pop_back();
    rejected(bad);
    bad = module(8, 8);
    bad.resize(10);  // entry point without LocalSize
    rejected(bad);
    bad = module(8, 8);
    bad[10] = (6u << 16u) | 331u;  // OpExecutionModeId / LocalSizeId is unsupported
    bad[12] = 38u;
    rejected(bad);
    bad = module(8, 8);
    bad[6] = 4;  // Fragment, not GLCompute
    rejected(bad);
    bad = module(8, 8);
    bad[8] = 0;  // entry point other than main
    rejected(bad);
    bad = module(8, 8);
    bad[11] = 2;  // LocalSize belongs to a different entry point
    rejected(bad);
    bad = module(8, 8);
    const auto other = module(16, 16);
    bad.insert(bad.end(), other.begin() + 5, other.begin() + 10);
    rejected(bad);
    rejected(module(0, 8));
    rejected(module(32, 16));
    bad = module(8, 8);
    bad[15] = 2;
    rejected(bad);
    bad = module(8, 8);
    bad.push_back(0);  // malformed instruction after LocalSize
    rejected(bad);

    // Optional real modules verify the compiler contract on the build host.
    for (int i = 1; i < argc; ++i) {
        std::ifstream input(argv[i], std::ios::binary | std::ios::ate);
        assert(input);
        const std::streamoff length = input.tellg();
        assert(length > 0 && length % static_cast<std::streamoff>(sizeof(uint32_t)) == 0);
        std::vector<uint32_t> words(static_cast<size_t>(length) / sizeof(uint32_t));
        input.seekg(0);
        input.read(reinterpret_cast<char*>(words.data()), length);
        assert(input);
        const auto shape = literalLocalSize2D({words.data(), words.size()});
        std::cout << argv[i] << ": " << shape[0] << 'x' << shape[1] << '\n';
    }
}
