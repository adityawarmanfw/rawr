#pragma once

#include <array>
#include <stdexcept>

#include "raw_highlight/Spirv.hpp"

namespace rawr::highlight {

// Our generated guide modules have one compute entry point named main and a
// literal LocalSize. Specialization-based workgroups require a different API.
// Parse the module once at pipeline creation so dispatch matches the shader.
inline std::array<uint32_t, 2> literalLocalSize2D(SpirvWords shader) {
    if (!shader.words || shader.count < 5 || shader.words[0] != 0x07230203u)
        throw std::runtime_error("highlight guide: invalid SPIR-V header");
    uint32_t entryId = 0, localEntryId = 0;
    uint32_t entryCount = 0;
    bool foundSize = false;
    std::array<uint32_t, 2> size{};
    for (size_t offset = 5; offset < shader.count;) {
        const uint32_t instruction = shader.words[offset];
        const uint32_t count = instruction >> 16u;
        const uint32_t opcode = instruction & 65535u;
        if (count == 0 || count > shader.count - offset)
            throw std::runtime_error("highlight guide: truncated SPIR-V instruction");
        // OpEntryPoint=15, GLCompute=5; "main\0" occupies two words.
        if (opcode == 15u) {
            ++entryCount;
            if (count < 5 || shader.words[offset + 1] != 5u || shader.words[offset + 3] != 0x6e69616du ||
                shader.words[offset + 4] != 0u)
                throw std::runtime_error("highlight guide: compute entry point main required");
            entryId = shader.words[offset + 2];
        }
        // OpExecutionMode=16, LocalSize=17.
        if (opcode == 16u && count >= 3 && shader.words[offset + 2] == 17u) {
            if (count != 6 || foundSize)
                throw std::runtime_error("highlight guide: one literal LocalSize required");
            const uint32_t x = shader.words[offset + 3], y = shader.words[offset + 4];
            if (x == 0 || y == 0 || uint64_t(x) * y > 256u || shader.words[offset + 5] != 1u)
                throw std::runtime_error("highlight guide: invalid 2D local size");
            size = {x, y};
            localEntryId = shader.words[offset + 1];
            foundSize = true;
        }
        offset += count;
    }
    if (entryCount != 1 || entryId == 0 || !foundSize || localEntryId != entryId)
        throw std::runtime_error("highlight guide: main requires a literal LocalSize");
    return size;
}

}  // namespace rawr::highlight
