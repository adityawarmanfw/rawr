// Prints one letter (I/P/B) per picture of an Annex B HEVC stream, in decode order.
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

#include "video/HevcFrameType.h"

using rawrcam::video::HevcFrameTypeParser;

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::ifstream file(argv[1], std::ios::binary);
    const std::vector<uint8_t> stream((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    HevcFrameTypeParser parser;
    parser.setCodecConfig(stream.data(), stream.size());
    // Split at each start code that begins a first slice segment; classify each NAL on its own.
    for (size_t i = 0; i + 4 < stream.size(); ++i) {
        if (stream[i] || stream[i + 1] || stream[i + 2] != 1) continue;
        const int type = (stream[i + 3] >> 1) & 0x3f;
        if (type > 21 || (stream[i + 5] & 0x80) == 0) continue;
        size_t end = i + 3;
        while (end + 2 < stream.size() && !(stream[end] == 0 && stream[end + 1] == 0 && stream[end + 2] == 1)) ++end;
        switch (parser.classify(stream.data() + i, end - i)) {
            case HevcFrameTypeParser::Type::I: std::putchar('I'); break;
            case HevcFrameTypeParser::Type::P: std::putchar('P'); break;
            case HevcFrameTypeParser::Type::B: std::putchar('B'); break;
            default: std::putchar('?'); break;
        }
    }
    std::putchar('\n');
    return 0;
}
