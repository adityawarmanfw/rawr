#include "tonemap/lut/Lut3D.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace tonemap::lut {
namespace {
color::Vec3 add(color::Vec3 a, color::Vec3 b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
color::Vec3 sub(color::Vec3 a, color::Vec3 b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
color::Vec3 mul(color::Vec3 a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
}  // namespace
Lut3D parseCubeFile(const std::string& path, uint32_t maxSize) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open cube: " + path);
    return parseCube(f, maxSize);
}
Lut3D parseCube(std::istream& f, uint32_t maxSize) {
    Lut3D out;
    std::string line;
    uint64_t lineNo = 0;
    while (std::getline(f, line)) {
        ++lineNo;
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos || line[first] == '#') continue;
        std::istringstream ss(line.substr(first));
        std::string head;
        ss >> head;
        if (head == "TITLE") continue;
        if (head == "LUT_1D_SIZE") throw std::runtime_error("1D LUT is not supported");
        if (head == "LUT_3D_SIZE") {
            uint32_t n = 0;
            if (!(ss >> n) || n < 2 || n > maxSize) throw std::runtime_error("invalid LUT_3D_SIZE");
            out.size = n;
            continue;
        }
        if (head == "DOMAIN_MIN" || head == "DOMAIN_MAX") {
            auto& d = head == "DOMAIN_MIN" ? out.domainMin : out.domainMax;
            if (!(ss >> d[0] >> d[1] >> d[2])) throw std::runtime_error("invalid cube domain");
            continue;
        }
        if ((head[0] >= '0' && head[0] <= '9') || head[0] == '-' || head[0] == '.') {
            if (out.size == 0) throw std::runtime_error("cube data before LUT_3D_SIZE");
            color::Vec3 v{};
            try {
                v[0] = std::stof(head);
            } catch (...) {
                throw std::runtime_error("invalid cube number");
            }
            if (!(ss >> v[1] >> v[2])) throw std::runtime_error("invalid cube RGB row");
            for (float x : v)
                if (!std::isfinite(x)) throw std::runtime_error("non-finite cube value");
            out.values.push_back(v);
            continue;
        }
        throw std::runtime_error("unsupported .cube directive at line " + std::to_string(lineNo) + ": " + head);
    }
    if (out.size == 0) throw std::runtime_error("missing LUT_3D_SIZE");
    const uint64_t expected = uint64_t(out.size) * out.size * out.size;
    if (out.values.size() != expected) throw std::runtime_error("cube table length mismatch");
    for (int c = 0; c < 3; ++c)
        if (!(out.domainMax[c] > out.domainMin[c])) throw std::runtime_error("invalid cube domain range");
    return out;
}
color::Vec3 Lut3D::sampleTetrahedral(color::Vec3 in) const {
    if (size < 2 || values.size() != uint64_t(size) * size * size) throw std::runtime_error("invalid LUT");
    float p[3];
    uint32_t i[3];
    float f[3];
    for (int c = 0; c < 3; ++c) {
        p[c] = std::clamp((in[c] - domainMin[c]) / (domainMax[c] - domainMin[c]), 0.0f, 1.0f) * (size - 1);
        i[c] = std::min(uint32_t(std::floor(p[c])), size - 2);
        f[c] = p[c] - i[c];
    }
    auto at = [&](uint32_t r, uint32_t g, uint32_t b) { return values[(uint64_t(b) * size + g) * size + r]; };
    const auto c000 = at(i[0], i[1], i[2]);
    color::Vec3 out = c000;
    if (f[0] >= f[1]) {
        if (f[1] >= f[2])
            out = add(out, add(mul(sub(at(i[0] + 1, i[1], i[2]), c000), f[0]),
                               add(mul(sub(at(i[0] + 1, i[1] + 1, i[2]), at(i[0] + 1, i[1], i[2])), f[1]),
                                   mul(sub(at(i[0] + 1, i[1] + 1, i[2] + 1), at(i[0] + 1, i[1] + 1, i[2])), f[2]))));
        else if (f[0] >= f[2])
            out = add(out, add(mul(sub(at(i[0] + 1, i[1], i[2]), c000), f[0]),
                               add(mul(sub(at(i[0] + 1, i[1], i[2] + 1), at(i[0] + 1, i[1], i[2])), f[2]),
                                   mul(sub(at(i[0] + 1, i[1] + 1, i[2] + 1), at(i[0] + 1, i[1], i[2] + 1)), f[1]))));
        else
            out = add(out, add(mul(sub(at(i[0], i[1], i[2] + 1), c000), f[2]),
                               add(mul(sub(at(i[0] + 1, i[1], i[2] + 1), at(i[0], i[1], i[2] + 1)), f[0]),
                                   mul(sub(at(i[0] + 1, i[1] + 1, i[2] + 1), at(i[0] + 1, i[1], i[2] + 1)), f[1]))));
    } else {
        if (f[0] >= f[2])
            out = add(out, add(mul(sub(at(i[0], i[1] + 1, i[2]), c000), f[1]),
                               add(mul(sub(at(i[0] + 1, i[1] + 1, i[2]), at(i[0], i[1] + 1, i[2])), f[0]),
                                   mul(sub(at(i[0] + 1, i[1] + 1, i[2] + 1), at(i[0] + 1, i[1] + 1, i[2])), f[2]))));
        else if (f[1] >= f[2])
            out = add(out, add(mul(sub(at(i[0], i[1] + 1, i[2]), c000), f[1]),
                               add(mul(sub(at(i[0], i[1] + 1, i[2] + 1), at(i[0], i[1] + 1, i[2])), f[2]),
                                   mul(sub(at(i[0] + 1, i[1] + 1, i[2] + 1), at(i[0], i[1] + 1, i[2] + 1)), f[0]))));
        else
            out = add(out, add(mul(sub(at(i[0], i[1], i[2] + 1), c000), f[2]),
                               add(mul(sub(at(i[0], i[1] + 1, i[2] + 1), at(i[0], i[1], i[2] + 1)), f[1]),
                                   mul(sub(at(i[0] + 1, i[1] + 1, i[2] + 1), at(i[0], i[1] + 1, i[2] + 1)), f[0]))));
    }
    return out;
}
}  // namespace tonemap::lut
