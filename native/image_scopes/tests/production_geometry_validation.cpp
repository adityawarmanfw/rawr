#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

#include "image_scopes/image_scopes.h"
#include "image_scopes_cpu.h"
#include "layouts.hpp"
#include "vulkan_test_context.hpp"

using namespace image_scopes;
namespace cpu = image_scopes::cpu;
namespace t = image_scopes::tests;

namespace {
void require(bool v, const char* m) {
    if (!v) throw std::runtime_error(m);
}

std::vector<std::uint8_t> pattern(std::uint32_t w, std::uint32_t h) {
    std::vector<std::uint8_t> a(static_cast<std::size_t>(w) * h * 4u);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            auto* p = &a[(static_cast<std::size_t>(y) * w + x) * 4u];
            p[0] = static_cast<std::uint8_t>((x * 17u + y * 3u) & 255u);
            p[1] = static_cast<std::uint8_t>((x * 5u + y * 29u) & 255u);
            p[2] = static_cast<std::uint8_t>(((x ^ y) * 11u) & 255u);
            p[3] = 255u;
        }
    return a;
}

std::vector<std::uint8_t> samplerInvariant(std::uint32_t w, std::uint32_t h) {
    std::vector<std::uint8_t> a(static_cast<std::size_t>(w) * h * 4u, 255u);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::uint32_t bx = x >> 1u, by = y >> 1u;
            const auto v = static_cast<std::uint8_t>((bx * 7u + by * 11u) & 255u);
            auto* p = &a[(static_cast<std::size_t>(y) * w + x) * 4u];
            p[0] = p[1] = p[2] = v;
        }
    return a;
}

double exposure(const std::vector<std::uint8_t>& p) {
    double s = 0.0;
    for (std::size_t i = 0; i + 3 < p.size(); i += 4u) s += double(p[i]) / 255.0;
    return s / double(p.size() / 4u);
}

VkCommandBuffer allocSecondaryPrimary(t::VulkanTestContext& vk) {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = vk.commandPool();
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    require(vkAllocateCommandBuffers(vk.device(), &ai, &cb) == VK_SUCCESS, "extra command-buffer allocation failed");
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    require(vkBeginCommandBuffer(cb, &bi) == VK_SUCCESS, "extra command-buffer begin failed");
    return cb;
}

void finishAndFree(t::VulkanTestContext& vk, VkCommandBuffer cb) {
    require(vkEndCommandBuffer(cb) == VK_SUCCESS, "extra command-buffer end failed");
    vkFreeCommandBuffers(vk.device(), vk.commandPool(), 1, &cb);
}

// Independent pixel-by-pixel oracle: sampling is in source coordinates, X bins in display coordinates.
// Each slot is reused Full -> 25% -> 50%: maxima must decrease with the current
// histogram, including a zero-sample 1x1 frame. Rotation also switches RGB back to luma.
void validateRotatedSampling(t::VulkanTestContext& vk, unsigned w, unsigned h) {
    const auto px = pattern(w, h);
    auto input = vk.createRgbaImage(w, h);
    vk.uploadRgba(input, px);
    auto host = vk.createBuffer(sizeof(detail::DisplayWaveformStd430), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    ImageScopes scopes({{vk.physicalDevice(), vk.device(), nullptr}, 1});
    for (unsigned q = 0; q < 4; ++q)
        for (auto mode : {WaveformMode::Luma, WaveformMode::RgbOverlay})
            for (auto sampling :
                 {SamplingMode::FullReference, SamplingMode::Production25, SamplingMode::Production50}) {
                const unsigned planes = mode == WaveformMode::Luma ? 1u : 3u;
                std::vector<std::uint32_t> expected(planes * detail::kWavePlaneElements);
                unsigned count = 0;
                for (unsigned y = 0; y < h; ++y)
                    for (unsigned x = 0; x < w; ++x) {
                        if (!cpu::sampleSelected(x, y, sampling)) continue;
                        ++count;
                        unsigned dx = q == 0 ? x : q == 1 ? h - 1u - y : q == 2 ? w - 1u - x : y;
                        unsigned column = dx * 512u / ((q & 1u) ? h : w);
                        const auto* c = &px[(std::size_t(y) * w + x) * 4u];
                        for (unsigned ch = 0; ch < planes; ++ch) {
                            const unsigned bin = planes == 1 ? cpu::encodedLumaQ16(c[0], c[1], c[2]) : c[ch];
                            ++expected[ch * detail::kWavePlaneElements + bin * 512u + column];
                        }
                    }
                vk.beginCommands();
                DisplayWaveformRecordInfo r{};
                r.commandBuffer = vk.commandBuffer();
                r.input = {input.view, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_GENERAL, w, h};
                r.render.mode = mode;
                r.samplingMode = sampling;
                r.sourceQuarterTurns = q;
                r.renderEnabled = false;
                scopes.recordDisplayWaveform(r);
                scopes.recordCopyDisplayWaveform(vk.commandBuffer(), 0, host.buffer, 0);
                vk.submitAndWait();
                scopes.retireFrameSlot(0);
                const auto* actual = static_cast<const detail::DisplayWaveformStd430*>(vk.map(host));
                vk.invalidate(host);
                require(std::equal(expected.begin(), expected.end(), actual->density),
                        "rotated waveform density mismatch");
                require(actual->sampledPixelCount == count, "rotated waveform sample count mismatch");
                for (unsigned ch = 0; ch < planes; ++ch) {
                    const auto first = expected.begin() + ch * detail::kWavePlaneElements;
                    require(actual->maxima[ch] == *std::max_element(first, first + detail::kWavePlaneElements),
                            "rotated waveform maximum mismatch");
                }
                for (unsigned ch = planes; ch < 3u; ++ch)
                    require(actual->maxima[ch] == 0u, "inactive waveform maximum not reset");
                vk.unmap(host);
            }
    // Odd edges and short stripes also exercise both vectorscope grids.
    for (auto grid : {VectorscopeGrid::Compact128, VectorscopeGrid::Reference256})
        for (auto sampling : {SamplingMode::FullReference, SamplingMode::Production25, SamplingMode::Production50}) {
            vk.beginCommands();
            VectorscopeRecordInfo r{};
            r.commandBuffer = vk.commandBuffer();
            r.input = {input.view, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_GENERAL, w, h};
            r.grid = grid;
            r.samplingMode = sampling;
            r.renderEnabled = false;
            scopes.recordVectorscope(r);
            scopes.recordCopyVectorscope(vk.commandBuffer(), 0, host.buffer, 0);
            vk.submitAndWait();
            scopes.retireFrameSlot(0);
            const auto ref = cpu::measureVectorscope({px.data(), w, h, w * 4u}, sampling, static_cast<unsigned>(grid));
            const auto* actual = static_cast<const detail::VectorscopeStd430*>(vk.map(host));
            vk.invalidate(host);
            require(std::equal(ref.density.begin(), ref.density.end(), actual->density),
                    "edge vectorscope density mismatch");
            require(actual->sampledPixelCount == ref.sampling.sampledPixelCount, "edge vectorscope count mismatch");
            require(actual->maximum == *std::max_element(ref.density.begin(), ref.density.end()),
                    "edge vectorscope maximum mismatch");
            vk.unmap(host);
        }
    vk.destroyBuffer(host);
    vk.destroyRgbaImage(input);
    std::cout << "ROTATED_SAMPLING_PARITY_PASS " << w << "x" << h << "\n";
}

void validateDisplayGeometry(t::VulkanTestContext& vk, std::uint32_t w, std::uint32_t h) {
    auto px = pattern(w, h);
    auto input = vk.createRgbaImage(w, h);
    vk.uploadRgba(input, px);
    DisplayImageView iv{input.view, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_GENERAL, w, h};
    ImageScopes scopes({{vk.physicalDevice(), vk.device(), nullptr}, 3});
    auto host = vk.createBuffer(sizeof(detail::DisplayWaveformStd430), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    auto waveA = vk.createRgbaImage(768, 512), waveB = vk.createRgbaImage(768, 512);
    auto vecA = vk.createRgbaImage(512, 512), vecB = vk.createRgbaImage(512, 512);
    std::vector<std::uint8_t> wz(768u * 512u * 4u, 0u), vz(512u * 512u * 4u, 0u);
    vk.uploadRgba(waveA, wz);
    vk.uploadRgba(waveB, wz);
    vk.uploadRgba(vecA, vz);
    vk.uploadRgba(vecB, vz);
    const ScopeRenderTarget wta{waveA.view, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_GENERAL, 768, 512};
    const ScopeRenderTarget wtb{waveB.view, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_GENERAL, 768, 512};
    const ScopeRenderTarget vta{vecA.view, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_GENERAL, 512, 512};
    const ScopeRenderTarget vtb{vecB.view, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_GENERAL, 512, 512};

    // RGB + luma waveform: all sampling modes, numerical parity and render-only equality.
    for (const auto mode : {WaveformMode::RgbOverlay, WaveformMode::Luma}) {
        for (const auto sm : {SamplingMode::FullReference, SamplingMode::Production50, SamplingMode::Production25}) {
            vk.beginCommands();
            DisplayWaveformRecordInfo r{};
            r.commandBuffer = vk.commandBuffer();
            r.input = iv;
            r.render.mode = mode;
            r.samplingMode = sm;
            r.renderEnabled = false;
            scopes.recordDisplayWaveform(r);
            scopes.recordCopyDisplayWaveform(vk.commandBuffer(), 0, host.buffer, 0);
            vk.submitAndWait();
            scopes.retireFrameSlot(0);
            auto* gp = static_cast<const detail::DisplayWaveformStd430*>(vk.map(host));
            vk.invalidate(host);
            const auto ref = cpu::measureDisplayWaveform({px.data(), w, h, w * 4u}, mode, sm, 512, 256);
            require(std::equal(ref.density.begin(), ref.density.end(), gp->density),
                    "geometry waveform numerical mismatch");
            require(gp->sampledPixelCount == ref.sampling.sampledPixelCount,
                    "geometry waveform sampled count mismatch");
            vk.unmap(host);

            // Immediate measure+render must equal retire -> render-only.
            vk.beginCommands();
            DisplayWaveformRecordInfo ia{};
            ia.commandBuffer = vk.commandBuffer();
            ia.input = iv;
            ia.renderTarget = wta;
            ia.render.mode = mode;
            ia.render.showGrid = false;
            ia.render.showBackground = false;
            ia.samplingMode = sm;
            ia.frameSlot = 1;
            ia.renderEnabled = true;
            scopes.recordDisplayWaveform(ia);
            vk.submitAndWait();
            scopes.retireFrameSlot(1);
            const auto immediate = vk.downloadRgba(waveA);
            vk.beginCommands();
            DisplayWaveformRecordInfo rm{};
            rm.commandBuffer = vk.commandBuffer();
            rm.input = iv;
            rm.render.mode = mode;
            rm.samplingMode = sm;
            rm.frameSlot = 2;
            rm.renderEnabled = false;
            scopes.recordDisplayWaveform(rm);
            vk.submitAndWait();
            scopes.retireFrameSlot(2);
            vk.beginCommands();
            DisplayWaveformRenderInfo ro{};
            ro.commandBuffer = vk.commandBuffer();
            ro.renderTarget = wtb;
            ro.render.mode = mode;
            ro.render.showGrid = false;
            ro.render.showBackground = false;
            ro.frameSlot = 2;
            scopes.recordDisplayWaveformRender(ro);
            vk.submitAndWait();
            scopes.retireFrameSlot(2);
            require(immediate == vk.downloadRgba(waveB), "geometry waveform render-only differs from measure+render");
        }
    }
    std::cout << "PRODUCTION_GEOMETRY_WAVEFORM_NUMERICAL_RENDER_PASS " << w << "x" << h << "\n";

    // Waveform pending/retired state and descriptor invocation lifetime at exact geometry.
    vk.beginCommands();
    DisplayWaveformRecordInfo wseed{};
    wseed.commandBuffer = vk.commandBuffer();
    wseed.input = iv;
    wseed.render.mode = WaveformMode::Luma;
    wseed.samplingMode = SamplingMode::FullReference;
    wseed.renderEnabled = false;
    scopes.recordDisplayWaveform(wseed);
    vk.submitAndWait();
    scopes.retireFrameSlot(0);
    vk.beginCommands();
    DisplayWaveformRecordInfo wp{};
    wp.commandBuffer = vk.commandBuffer();
    wp.input = iv;
    wp.render.mode = WaveformMode::RgbOverlay;
    wp.samplingMode = SamplingMode::Production25;
    wp.renderEnabled = false;
    scopes.recordDisplayWaveform(wp);
    VkCommandBuffer wOther = allocSecondaryPrimary(vk);
    DisplayWaveformRenderInfo wx{};
    wx.commandBuffer = wOther;
    wx.renderTarget = wtb;
    wx.render.mode = WaveformMode::RgbOverlay;
    bool wRejected = false;
    try {
        scopes.recordDisplayWaveformRender(wx);
    } catch (const std::logic_error&) {
        wRejected = true;
    }
    require(wRejected, "geometry waveform pending state allowed cross-command-buffer render");
    finishAndFree(vk, wOther);
    DisplayWaveformRenderInfo ws{};
    ws.commandBuffer = vk.commandBuffer();
    ws.renderTarget = wta;
    ws.render.mode = WaveformMode::RgbOverlay;
    ws.render.showGrid = false;
    ws.render.showBackground = false;
    scopes.recordDisplayWaveformRender(ws);
    vk.submitAndWait();
    scopes.retireFrameSlot(0);
    require(scopes.displayWaveformView(0).mode == WaveformMode::RgbOverlay,
            "geometry waveform pending metadata not retired");
    vk.beginCommands();
    DisplayWaveformRenderInfo wdA{};
    wdA.commandBuffer = vk.commandBuffer();
    wdA.renderTarget = wta;
    wdA.render.mode = WaveformMode::RgbOverlay;
    wdA.render.showGrid = false;
    wdA.render.showBackground = false;
    auto wdB = wdA;
    wdB.renderTarget = wtb;
    scopes.recordDisplayWaveformRender(wdA);
    scopes.recordDisplayWaveformRender(wdB);
    vk.submitAndWait();
    scopes.retireFrameSlot(0);
    require(vk.downloadRgba(waveA) == vk.downloadRgba(waveB), "geometry waveform invocation descriptor retargeted");
    std::cout << "PRODUCTION_GEOMETRY_WAVEFORM_STATE_DESCRIPTOR_PASS " << w << "x" << h << "\n";

    // Vectorscope 128/256, all sampling modes, numerical parity and render-only equality.
    for (const auto grid : {VectorscopeGrid::Compact128, VectorscopeGrid::Reference256}) {
        const std::uint32_t bins = (grid == VectorscopeGrid::Compact128) ? 128u : 256u;
        for (const auto sm : {SamplingMode::FullReference, SamplingMode::Production50, SamplingMode::Production25}) {
            vk.beginCommands();
            VectorscopeRecordInfo r{};
            r.commandBuffer = vk.commandBuffer();
            r.input = iv;
            r.grid = grid;
            r.samplingMode = sm;
            r.renderEnabled = false;
            scopes.recordVectorscope(r);
            scopes.recordCopyVectorscope(vk.commandBuffer(), 0, host.buffer, 0);
            vk.submitAndWait();
            scopes.retireFrameSlot(0);
            auto* gp = static_cast<const detail::VectorscopeStd430*>(vk.map(host));
            vk.invalidate(host);
            const auto ref = cpu::measureVectorscope({px.data(), w, h, w * 4u}, sm, bins);
            const std::size_t n = static_cast<std::size_t>(bins) * bins;
            require(std::equal(ref.density.begin(), ref.density.end(), gp->density),
                    "geometry vectorscope numerical mismatch");
            require(ref.density.size() == n, "geometry vectorscope reference size mismatch");
            require(gp->sampledPixelCount == ref.sampling.sampledPixelCount,
                    "geometry vectorscope sampled count mismatch");
            vk.unmap(host);

            vk.beginCommands();
            VectorscopeRecordInfo ia{};
            ia.commandBuffer = vk.commandBuffer();
            ia.input = iv;
            ia.renderTarget = vta;
            ia.grid = grid;
            ia.samplingMode = sm;
            ia.renderEnabled = true;
            ia.frameSlot = 1;
            ia.render.showGrid = false;
            ia.render.showTargets = false;
            ia.render.showBackground = false;
            scopes.recordVectorscope(ia);
            vk.submitAndWait();
            scopes.retireFrameSlot(1);
            const auto immediate = vk.downloadRgba(vecA);
            vk.beginCommands();
            VectorscopeRecordInfo rm{};
            rm.commandBuffer = vk.commandBuffer();
            rm.input = iv;
            rm.grid = grid;
            rm.samplingMode = sm;
            rm.renderEnabled = false;
            rm.frameSlot = 2;
            scopes.recordVectorscope(rm);
            vk.submitAndWait();
            scopes.retireFrameSlot(2);
            vk.beginCommands();
            VectorscopeRenderInfo ro{};
            ro.commandBuffer = vk.commandBuffer();
            ro.renderTarget = vtb;
            ro.frameSlot = 2;
            ro.render.showGrid = false;
            ro.render.showTargets = false;
            ro.render.showBackground = false;
            scopes.recordVectorscopeRender(ro);
            vk.submitAndWait();
            scopes.retireFrameSlot(2);
            require(immediate == vk.downloadRgba(vecB), "geometry vectorscope render-only differs from measure+render");
        }
    }
    std::cout << "PRODUCTION_GEOMETRY_VECTORSCOPE_NUMERICAL_RENDER_PASS " << w << "x" << h << "\n";

    // Pending/retired vectorscope semantics plus stale Full->P25 normalization proof.
    vk.beginCommands();
    VectorscopeRecordInfo vf{};
    vf.commandBuffer = vk.commandBuffer();
    vf.input = iv;
    vf.grid = VectorscopeGrid::Reference256;
    vf.samplingMode = SamplingMode::FullReference;
    vf.renderEnabled = false;
    scopes.recordVectorscope(vf);
    vk.submitAndWait();
    scopes.retireFrameSlot(0);
    vk.beginCommands();
    VectorscopeRecordInfo vp{};
    vp.commandBuffer = vk.commandBuffer();
    vp.input = iv;
    vp.grid = VectorscopeGrid::Reference256;
    vp.samplingMode = SamplingMode::Production25;
    vp.renderEnabled = false;
    scopes.recordVectorscope(vp);
    VkCommandBuffer vOther = allocSecondaryPrimary(vk);
    VectorscopeRenderInfo vx{};
    vx.commandBuffer = vOther;
    vx.renderTarget = vtb;
    bool vRejected = false;
    try {
        scopes.recordVectorscopeRender(vx);
    } catch (const std::logic_error&) {
        vRejected = true;
    }
    require(vRejected, "geometry vectorscope pending state allowed cross-command-buffer render");
    finishAndFree(vk, vOther);
    VectorscopeRenderInfo vs{};
    vs.commandBuffer = vk.commandBuffer();
    vs.renderTarget = vta;
    vs.render.showGrid = false;
    vs.render.showTargets = false;
    vs.render.showBackground = false;
    scopes.recordVectorscopeRender(vs);
    vk.submitAndWait();
    scopes.retireFrameSlot(0);
    const auto pendingP25 = vk.downloadRgba(vecA);
    require(scopes.vectorscopeView(0).sampling.mode == SamplingMode::Production25,
            "geometry vectorscope pending metadata not retired");

    // Clean P25 reference in another slot.
    vk.beginCommands();
    VectorscopeRecordInfo vc{};
    vc.commandBuffer = vk.commandBuffer();
    vc.input = iv;
    vc.grid = VectorscopeGrid::Reference256;
    vc.samplingMode = SamplingMode::Production25;
    vc.frameSlot = 2;
    vc.renderEnabled = false;
    scopes.recordVectorscope(vc);
    vk.submitAndWait();
    scopes.retireFrameSlot(2);
    vk.beginCommands();
    VectorscopeRenderInfo vr{};
    vr.commandBuffer = vk.commandBuffer();
    vr.renderTarget = vtb;
    vr.frameSlot = 2;
    vr.render.showGrid = false;
    vr.render.showTargets = false;
    vr.render.showBackground = false;
    scopes.recordVectorscopeRender(vr);
    vk.submitAndWait();
    scopes.retireFrameSlot(2);
    require(pendingP25 == vk.downloadRgba(vecB),
            "geometry vectorscope pending render used stale sample-count normalization");

    // Descriptor target lifetime using retired P25 measurement.
    vk.beginCommands();
    VectorscopeRenderInfo vdA{};
    vdA.commandBuffer = vk.commandBuffer();
    vdA.renderTarget = vta;
    vdA.render.showGrid = false;
    vdA.render.showTargets = false;
    vdA.render.showBackground = false;
    auto vdB = vdA;
    vdB.renderTarget = vtb;
    scopes.recordVectorscopeRender(vdA);
    scopes.recordVectorscopeRender(vdB);
    vk.submitAndWait();
    scopes.retireFrameSlot(0);
    require(vk.downloadRgba(vecA) == vk.downloadRgba(vecB), "geometry vectorscope invocation descriptor retargeted");
    std::cout << "PRODUCTION_GEOMETRY_VECTORSCOPE_STATE_NORMALIZATION_DESCRIPTOR_PASS " << w << "x" << h << "\n";

    // Exact-size Waveform E sampling-normalization gate. 2x2 sampler blocks are
    // constant so Full/50/25 differ only in population, not waveform topology.
    auto np = samplerInvariant(w, h);
    auto ni = vk.createRgbaImage(w, h);
    vk.uploadRgba(ni, np);
    DisplayImageView niv{ni.view, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_GENERAL, w, h};
    auto renderNorm = [&](SamplingMode sm) {
        vk.beginCommands();
        DisplayWaveformRecordInfo n{};
        n.commandBuffer = vk.commandBuffer();
        n.input = niv;
        n.renderTarget = wta;
        n.render.mode = WaveformMode::Luma;
        n.render.showGrid = false;
        n.render.showBackground = false;
        n.samplingMode = sm;
        n.frameSlot = 1;
        n.renderEnabled = true;
        scopes.recordDisplayWaveform(n);
        vk.submitAndWait();
        scopes.retireFrameSlot(1);
        return vk.downloadRgba(waveA);
    };
    const auto nf = renderNorm(SamplingMode::FullReference), n50 = renderNorm(SamplingMode::Production50),
               n25 = renderNorm(SamplingMode::Production25);
    const double ef = exposure(nf), e50 = exposure(n50), e25 = exposure(n25);
    require(ef > 0.0, "geometry waveform normalization empty");
    const double q50 = e50 / ef, q25 = e25 / ef;
    require(q50 > .90 && q50 < 1.10, "geometry waveform 50 normalization changed exposure");
    require(q25 > .85 && q25 < 1.15, "geometry waveform 25 normalization changed exposure");
    std::cout << "PRODUCTION_GEOMETRY_WAVEFORM_NORMALIZATION_PASS " << w << "x" << h << " ratios=" << q50 << "," << q25
              << "\n";
    vk.destroyRgbaImage(ni);

    require(vk.downloadRgba(input) == px, "geometry validation mutated display source");
    vk.destroyRgbaImage(vecB);
    vk.destroyRgbaImage(vecA);
    vk.destroyRgbaImage(waveB);
    vk.destroyRgbaImage(waveA);
    vk.destroyBuffer(host);
    vk.destroyRgbaImage(input);
    std::cout << "PRODUCTION_GEOMETRY_DISPLAY_MATRIX_PASS " << w << "x" << h << "\n";
}

}  // namespace

int main() {
    try {
        t::VulkanTestContext vk;
        std::cout << "Using GPU: " << vk.deviceName() << "\n";
        for (const auto& [w, h] : {std::pair<unsigned, unsigned>{1, 1},
                                   {1, 17},
                                   {19, 1},
                                   {7, 9},
                                   {511, 513},
                                   {2041, 1533},
                                   {2048, 1536},
                                   {2040, 1532},
                                   {2040, 1536}})
            validateRotatedSampling(vk, w, h);
        validateDisplayGeometry(vk, 2048u, 1536u);
        validateDisplayGeometry(vk, 2040u, 1532u);
        validateDisplayGeometry(vk, 2040u, 1536u);
        std::cout << "IMAGE_SCOPES_EXACT_PRODUCTION_GEOMETRY_PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
