#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "image_scopes/image_scopes.h"
#include "vulkan_test_context.hpp"
using namespace image_scopes;
using namespace image_scopes::tests;
namespace {
struct Pixels {
    std::uint32_t w = 0, h = 0;
    std::vector<std::uint8_t> rgba;
};
std::string tok(std::istream& f) {
    std::string s;
    while (f >> s) {
        if (!s.empty() && s[0] == '#') {
            std::getline(f, s);
            continue;
        }
        return s;
    }
    throw std::runtime_error("ppm eof");
}
Pixels ppm(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f || tok(f) != "P6") throw std::runtime_error("bad ppm " + p.string());
    Pixels x{};
    x.w = std::stoul(tok(f));
    x.h = std::stoul(tok(f));
    if (std::stoul(tok(f)) != 255u) throw std::runtime_error("bad maxval");
    f.get();
    std::vector<std::uint8_t> rgb(size_t(x.w) * x.h * 3u);
    f.read(reinterpret_cast<char*>(rgb.data()), std::streamsize(rgb.size()));
    if (size_t(f.gcount()) != rgb.size()) throw std::runtime_error("short ppm");
    x.rgba.resize(size_t(x.w) * x.h * 4u);
    for (size_t i = 0; i < size_t(x.w) * x.h; ++i) {
        x.rgba[i * 4] = rgb[i * 3];
        x.rgba[i * 4 + 1] = rgb[i * 3 + 1];
        x.rgba[i * 4 + 2] = rgb[i * 3 + 2];
        x.rgba[i * 4 + 3] = 255;
    }
    return x;
}
void save(const std::filesystem::path& p, std::uint32_t w, std::uint32_t h, const std::vector<std::uint8_t>& rgba) {
    std::ofstream f(p, std::ios::binary);
    f << "P6\n" << w << " " << h << "\n255\n";
    for (size_t i = 0; i < size_t(w) * h; ++i) f.write(reinterpret_cast<const char*>(&rgba[i * 4]), 3);
}
void renderMode(VulkanTestContext& vk, ImageScopes& scopes, const DisplayImageView& iv,
                const std::filesystem::path& out, const std::string& base, WaveformMode mode, const char* name,
                std::uint32_t OW, std::uint32_t OH) {
    auto dst = vk.createRgbaImage(OW, OH);
    std::vector<std::uint8_t> zero(size_t(OW) * OH * 4u, 0);
    vk.uploadRgba(dst, zero);
    ScopeRenderTarget rt{dst.view, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_GENERAL, OW, OH};
    vk.beginCommands();
    DisplayWaveformRecordInfo r{};
    r.commandBuffer = vk.commandBuffer();
    r.input = iv;
    r.renderTarget = rt;
    r.samplingMode = SamplingMode::FullReference;
    r.render.mode = mode;
    r.render.showGrid = true;
    r.render.showBackground = true;
    scopes.recordDisplayWaveform(r);
    vk.submitAndWait();
    scopes.retireFrameSlot(0);
    save(out / (base + "_waveform_" + name + ".ppm"), OW, OH, vk.downloadRgba(dst));
    // Data-only rerender proves the accepted render-only API and gives machine-checkable pixels.
    auto data = vk.createRgbaImage(OW, OH);
    vk.uploadRgba(data, zero);
    ScopeRenderTarget drt{data.view, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_GENERAL, OW, OH};
    vk.beginCommands();
    DisplayWaveformRenderInfo rr{};
    rr.commandBuffer = vk.commandBuffer();
    rr.renderTarget = drt;
    rr.render.mode = mode;
    rr.render.showGrid = false;
    rr.render.showBackground = false;
    scopes.recordDisplayWaveformRender(rr);
    vk.submitAndWait();
    scopes.retireFrameSlot(0);
    save(out / (base + "_waveform_" + name + "_data.ppm"), OW, OH, vk.downloadRgba(data));
    vk.destroyRgbaImage(data);
    vk.destroyRgbaImage(dst);
}
}  // namespace
int main(int argc, char** argv) {
    try {
        if (argc != 4 && argc != 6)
            throw std::invalid_argument("usage: image_scopes_waveform_photo_artifacts FIXTURE_DIR OUT_DIR basename [WIDTH HEIGHT]");
        const std::filesystem::path fixtures = argv[1], out = argv[2];
        const std::string base = argv[3];
        const auto width = argc == 6 ? std::stoul(argv[4]) : 768u;
        const auto height = argc == 6 ? std::stoul(argv[5]) : 512u;
        if (!width || !height || width > 2048u || height > 2048u)
            throw std::invalid_argument("output extent must be 1..2048");
        std::filesystem::create_directories(out);
        const auto px = ppm(fixtures / (base + ".ppm"));
        VulkanTestContext vk;
        ImageScopes scopes({{vk.physicalDevice(), vk.device(), nullptr}, 1});
        auto in = vk.createRgbaImage(px.w, px.h);
        vk.uploadRgba(in, px.rgba);
        DisplayImageView iv{in.view, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_GENERAL, px.w, px.h};
        renderMode(vk, scopes, iv, out, base, WaveformMode::RgbOverlay, "rgb", width, height);
        renderMode(vk, scopes, iv, out, base, WaveformMode::Luma, "luma", width, height);
        vk.destroyRgbaImage(in);
        std::cout << "WAVEFORM_E_PHOTO_ARTIFACT_PASS " << base << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "WAVEFORM_E_PHOTO_ARTIFACT_FAIL: " << e.what() << "\n";
        return 1;
    }
}
