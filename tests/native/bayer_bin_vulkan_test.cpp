// GPU Bayer downscales (native/raw_ingress BayerBinner) vs CPU references:
//  - the linear 2x bin (shaders/bayer_lin.comp) against a CPU copy of the same algorithm (kernels from
//    BayerBinKernels.h, clamp to the 3x3 same-colour source samples). Float summation order differs: no
//    code may differ by more than two, few by one.
//  - the direct 3x bin (shaders/bayer_bin3.comp) against a CPU copy that picks each output's RGGB kernel
//    by matching the actual window colours (not the shader's phase rule), all orders, centred crops;
//  - flat colours through the 3x bin in every order come out exact (a wrong phase would mix colours);
//  - mirrored Bayer orders: a mirrored scene binned with the mirrored order equals the mirrored result;
//  - the 50 -> 25 MP resample against the DNG converter's BayerResample.
// Optional: bayer_bin_vulkan_test - <width> <height> <order> times a synthetic frame.
// Requires a Vulkan device (MoltenVK on macOS).
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "convert/BayerResample.h"
#include "rawr/raw_ingress/BayerBinKernels.h"
#include "rawr/raw_ingress/BayerBinner.h"
#include "vk_test_common.hpp"

namespace {

using Frame = std::vector<std::uint16_t>;
constexpr std::array<std::array<std::uint8_t, 4>, 4> kOrders{
    {{0, 1, 1, 2}, {1, 0, 2, 1}, {1, 2, 0, 1}, {2, 1, 1, 0}}};  // RGGB GRBG GBRG BGGR

struct Params {
    std::array<float, 4> black{}, gains{};  // by buffer phase
    float white = 1023;
    std::uint32_t order = 0;
};

// Colour field, hard edges, thin lines, a zone plate corner and noise, 10-bit.
Frame scene(std::uint32_t w, std::uint32_t h, const std::array<std::uint8_t, 4>& cfa, std::uint32_t seed) {
    Frame f(std::size_t(w) * h);
    std::uint32_t state = seed;
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            state = state * 1664525u + 1013904223u;
            const std::uint8_t c = cfa[2 * (y & 1) + (x & 1)];
            const float rgb[3] = {0.35f + 0.25f * std::sin(x * 0.021f), 0.5f + 0.2f * std::cos(y * 0.017f),
                                  0.3f + 0.2f * std::sin((x + y) * 0.013f)};
            float v = rgb[c];
            if ((x / 29 + y / 41) % 3 == 0) v *= 0.35f;
            if ((x + 2 * y) % 53 < 2) v = 0.95f;
            if (x > w / 2 && y > h / 2) {
                const float dx = float(x) - w * 0.75f, dy = float(y) - h * 0.75f;
                v = 0.45f + 0.4f * std::cos((dx * dx + dy * dy) * 0.004f);
            }
            const float code = 64.0f + v * 900.0f + float(state >> 27) - 16.0f;
            f[std::size_t(y) * w + x] = std::uint16_t(std::clamp(code, 0.0f, 1023.0f));
        }
    return f;
}

// CPU copy of shaders/bayer_lin.comp.
Frame cpuBin(const Frame& in, std::uint32_t w, std::uint32_t h, const Params& p) {
    const int ow = int(w / 2), oh = int(h / 2);
    float maxBlack = 0, maxGain = 0;
    for (int i = 0; i < 4; ++i) maxBlack = std::max(maxBlack, p.black[i]), maxGain = std::max(maxGain, p.gains[i]);
    const float span = p.white - maxBlack, codeGain = 4;
    std::array<float, 4> g{};
    for (int i = 0; i < 4; ++i) g[i] = p.gains[i] / maxGain;
    auto sample = [&](int x, int y) {
        x = x < 0 ? -x : (x >= int(w) ? 2 * int(w) - 2 - x : x);
        y = y < 0 ? -y : (y >= int(h) ? 2 * int(h) - 2 - y : y);
        const int site = 2 * (y & 1) + (x & 1);
        return (float(in[std::size_t(y) * w + x]) - p.black[site]) * (g[site] / span);
    };
    auto toCode = [&](float v, int site) {
        const float c = std::clamp(v * span + p.black[site], 0.0f, p.white);
        return std::uint16_t(std::clamp(c * codeGain, 0.0f, p.white * codeGain) + 0.5f);
    };
    const bool fx = p.order & 1u, fy = p.order & 2u;
    Frame out(std::size_t(ow) * oh);
    for (int Y = 0; Y < oh; ++Y)
        for (int X = 0; X < ow; ++X) {
            const int q = 2 * ((Y & 1) ^ int(fy)) + ((X & 1) ^ int(fx));
            float b = 0;
            for (int ky = 0; ky < 12; ++ky)
                for (int kx = 0; kx < 12; ++kx)
                    b += rawr::raw_ingress::kBayerBinPlain[q * 144 + ky * 12 + kx] *
                         sample(2 * X - 5 + (fx ? 11 - kx : kx), 2 * Y - 5 + (fy ? 11 - ky : ky));
            // Same-colour source samples around the one nearest the output centre.
            const int cx = 2 * X + (X & 1), cy = 2 * Y + (Y & 1);
            float lo = sample(cx, cy), hi = lo;
            for (int u = -2; u <= 2; u += 2)
                for (int v = -2; v <= 2; v += 2) {
                    const float a = sample(cx + v, cy + u);
                    lo = std::min(lo, a);
                    hi = std::max(hi, a);
                }
            const int site = 2 * (Y & 1) + (X & 1);
            out[std::size_t(Y) * ow + X] = toCode(std::clamp(b, lo, hi) / g[site], site);
        }
    return out;
}

// CPU copy of shaders/bayer_bin3.comp, the RGGB phase found from the window's colours.
Frame cpuBin3(const Frame& in, std::uint32_t w, std::uint32_t h, const Params& p) {
    const auto size = rawr::raw_ingress::BayerBinner::outputSize(w, h, 3);
    const int ow = int(size.width), oh = int(size.height), ox = int(size.offsetX), oy = int(size.offsetY);
    float maxBlack = 0, maxGain = 0;
    for (int i = 0; i < 4; ++i) maxBlack = std::max(maxBlack, p.black[i]), maxGain = std::max(maxGain, p.gains[i]);
    const float span = p.white - maxBlack, codeGain = 4;
    std::array<float, 4> g{};
    for (int i = 0; i < 4; ++i) g[i] = p.gains[i] / maxGain;
    auto reflect = [](int v, int n) { return v < 0 ? -v : (v >= n ? 2 * n - 2 - v : v); };
    auto sample = [&](int x, int y) {
        x = reflect(x, int(w));
        y = reflect(y, int(h));
        const int site = 2 * (y & 1) + (x & 1);
        return (float(in[std::size_t(y) * w + x]) - p.black[site]) * (g[site] / span);
    };
    auto toCode = [&](float v, int site) {
        const float c = std::clamp(v * span + p.black[site], 0.0f, p.white);
        return std::uint16_t(std::clamp(c * codeGain, 0.0f, p.white * codeGain) + 0.5f);
    };
    const auto& cfa = kOrders[p.order];
    static constexpr std::uint8_t rggb[4] = {0, 1, 1, 2};
    Frame out(std::size_t(ow) * oh);
    for (int Y = 0; Y < oh; ++Y)
        for (int X = 0; X < ow; ++X) {
            const int cx = 3 * X + 1 + ox, cy = 3 * Y + 1 + oy;
            const std::uint8_t colour = cfa[2 * (Y & 1) + (X & 1)];
            // The RGGB output phase whose window (centred on 3X'+1) has this window's colours.
            int q = -1;
            for (int t = 0; t < 4 && q < 0; ++t) {
                const int tcx = 3 * (t & 1) + 1, tcy = 3 * (t >> 1) + 1;
                bool same = rggb[t] == colour;
                for (int dy = 0; dy < 2 && same; ++dy)
                    for (int dx = 0; dx < 2 && same; ++dx)
                        same = rggb[2 * ((tcy + dy) & 1) + ((tcx + dx) & 1)] == cfa[2 * ((cy + dy) & 1) + ((cx + dx) & 1)];
                if (same) q = t;
            }
            if (q < 0) throw std::runtime_error("no RGGB phase for a 3x output");
            float b = 0, lo = 1e30f, hi = -1e30f;
            for (int ky = 0; ky < 17; ++ky)
                for (int kx = 0; kx < 17; ++kx) {
                    const int sx = cx + kx - 8, sy = cy + ky - 8;
                    const float v = sample(sx, sy);
                    b += rawr::raw_ingress::kBayerBin3[q * 289 + ky * 17 + kx] * v;
                    if (std::abs(kx - 8) <= 3 && std::abs(ky - 8) <= 3 &&
                        cfa[2 * (reflect(sy, int(h)) & 1) + (reflect(sx, int(w)) & 1)] == colour) {
                        lo = std::min(lo, v);
                        hi = std::max(hi, v);
                    }
                }
            const int site = 2 * (Y & 1) + (X & 1);
            out[std::size_t(Y) * ow + X] = toCode(std::clamp(b, lo, hi) / g[site], site);
        }
    return out;
}

Frame cpuResample(const Frame& in, std::uint32_t iw, std::uint32_t ih, std::uint32_t ow, std::uint32_t oh,
                  std::uint16_t maxCode) {
    rawrcam::convert::BayerResample r;
    if (!r.configure(iw, ih, ow, oh, maxCode)) throw std::runtime_error("cpu resample configure");
    Frame out(std::size_t(ow) * oh);
    for (std::uint32_t oy = 0; oy < oh; oy += 64) {
        const std::uint32_t rows = std::min(64u, oh - oy);
        r.band(oy, rows, [&](std::uint32_t x, std::uint32_t y) { return double(in[std::size_t(y) * iw + x]); },
               out.data() + std::size_t(oy) * ow, ow);
    }
    return out;
}

struct Gpu {
    vktest::Ctx& c;
    rawr::raw_ingress::BayerBinner& binner;
    VkCommandBuffer cmd;
    VkFence fence;
    double lastMs = 0;

    void toGeneral(VkImage image, VkAccessFlags srcAccess) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = srcAccess ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcAccessMask = srcAccess;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &b);
    }
    // Bin (and optionally resample to rw x rh); returns the final codes.
    Frame run(const Frame& in, std::uint32_t w, std::uint32_t h, const Params& p, std::uint32_t rw = 0,
              std::uint32_t rh = 0, std::uint32_t ratio = 2) {
        const auto size = rawr::raw_ingress::BayerBinner::outputSize(w, h, ratio);
        const std::uint32_t ow = size.width, oh = size.height, fw = rw ? rw : ow, fh = rh ? rh : oh;
        const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        auto src = vktest::mkBuf(c.pd, c.dev, in.size() * 2, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host);
        auto dst = vktest::mkBuf(c.pd, c.dev, VkDeviceSize(fw) * fh * 2, VK_BUFFER_USAGE_TRANSFER_DST_BIT, host);
        const auto usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        auto binned = vktest::mkImg(c.pd, c.dev, ow, oh, VK_FORMAT_R16_UINT, usage);
        vktest::Img resampled{};
        if (rw) resampled = vktest::mkImg(c.pd, c.dev, rw, rh, VK_FORMAT_R16_UINT, usage);
        void* mapped = nullptr;
        vktest::ck(vkMapMemory(c.dev, src.m, 0, VK_WHOLE_SIZE, 0, &mapped), "map src");
        std::memcpy(mapped, in.data(), in.size() * 2);
        vkUnmapMemory(c.dev, src.m);
        vktest::ck(vkResetCommandBuffer(cmd, 0), "reset");
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vktest::ck(vkBeginCommandBuffer(cmd, &bi), "begin");
        toGeneral(binned.i, 0);
        if (rw) toGeneral(resampled.i, 0);
        rawr::raw_ingress::BayerBinner::Bin f;
        f.source = src.b;
        f.destination = binned.v;
        f.width = w;
        f.height = h;
        f.rowStrideBytes = w * 2;
        f.black = p.black;
        f.wbGains = p.gains;
        f.white = p.white;
        f.order = p.order;
        f.codeGain = 4;
        f.ratio = ratio;
        binner.bin(cmd, 0, f);
        if (rw) {
            toGeneral(binned.i, VK_ACCESS_SHADER_WRITE_BIT);
            binner.resample(cmd, 0, {binned.v, resampled.v, ow, oh, rw, rh, p.white * 4.0f});
        }
        toGeneral(rw ? resampled.i : binned.i, VK_ACCESS_SHADER_WRITE_BIT);
        VkBufferImageCopy r{};
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.imageExtent = {fw, fh, 1};
        vkCmdCopyImageToBuffer(cmd, rw ? resampled.i : binned.i, VK_IMAGE_LAYOUT_GENERAL, dst.b, 1, &r);
        vktest::ck(vkEndCommandBuffer(cmd), "end");
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        vktest::ck(vkResetFences(c.dev, 1, &fence), "reset fence");
        const auto start = std::chrono::steady_clock::now();
        vktest::ck(vkQueueSubmit(c.q, 1, &si, fence), "submit");
        vktest::ck(vkWaitForFences(c.dev, 1, &fence, VK_TRUE, UINT64_MAX), "wait");
        lastMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        Frame out(std::size_t(fw) * fh);
        vktest::ck(vkMapMemory(c.dev, dst.m, 0, VK_WHOLE_SIZE, 0, &mapped), "map dst");
        std::memcpy(out.data(), mapped, out.size() * 2);
        vkUnmapMemory(c.dev, dst.m);
        vktest::delImg(c.dev, resampled);
        vktest::delImg(c.dev, binned);
        vktest::delBuf(c.dev, dst);
        vktest::delBuf(c.dev, src);
        return out;
    }
};

bool compare(const char* name, const Frame& got, const Frame& want, std::uint32_t w) {
    std::size_t off1 = 0, worse = 0, first = 0;
    int maxError = 0;
    for (std::size_t i = 0; i < want.size(); ++i) {
        const int d = std::abs(int(got[i]) - int(want[i]));
        maxError = std::max(maxError, d);
        if (d == 1) ++off1;
        if (d > 2 && worse++ == 0) first = i;
    }
    const bool ok = worse == 0 && off1 * 50 < want.size();
    std::printf("%s %s off1=%zu/%zu max=%d worse=%zu first=(%zu,%zu)\n", ok ? "PASS" : "FAIL", name, off1, want.size(),
                maxError, worse, first % w, first / w);
    return ok;
}

Params params(std::uint32_t order) {
    Params p;
    p.order = order;
    const float neutral[3] = {0.45f, 1.0f, 0.62f};  // camera neutral (R, G, B)
    for (int i = 0; i < 4; ++i) {
        p.black[i] = 64.0f + float(i & 1);
        p.gains[i] = neutral[1] / neutral[kOrders[order][i]];
    }
    return p;
}

}  // namespace

int main(int argc, char** argv) {
    auto c = vktest::ctx();
    bool ok = true;
    {
        auto binner = rawr::raw_ingress::BayerBinner::create({c.pd, c.dev, c.qf});
        VkCommandPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = c.qf;
        VkCommandPool pool{};
        vktest::ck(vkCreateCommandPool(c.dev, &pi, nullptr, &pool), "pool");
        VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cmd{};
        vktest::ck(vkAllocateCommandBuffers(c.dev, &ai, &cmd), "cmd");
        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence{};
        vktest::ck(vkCreateFence(c.dev, &fi, nullptr, &fence), "fence");
        Gpu gpu{c, *binner, cmd, fence};
        if (argc >= 5) {
            const std::uint32_t w = std::uint32_t(std::atoi(argv[2])), h = std::uint32_t(std::atoi(argv[3]));
            const auto p = params(std::uint32_t(std::atoi(argv[4])));
            const auto in = scene(w, h, kOrders[p.order], 7);
            for (int i = 0; i < 3; ++i) {
                gpu.run(in, w, h, p);
                std::printf("bin %ux%u: %.1f ms (submit+wait)\n", w, h, gpu.lastMs);
            }
            const std::uint32_t rw = std::uint32_t(w / 2 / std::sqrt(2.0)) & ~7u, rh = std::uint32_t(h / 2 / std::sqrt(2.0)) & ~7u;
            gpu.run(in, w, h, p, rw, rh);
            std::printf("bin + resample to %ux%u: %.1f ms\n", rw, rh, gpu.lastMs);
            for (int i = 0; i < 3; ++i) {
                gpu.run(in, w, h, p, 0, 0, 3);
                const auto s3 = rawr::raw_ingress::BayerBinner::outputSize(w, h, 3);
                std::printf("bin3 to %ux%u: %.1f ms (submit+wait)\n", s3.width, s3.height, gpu.lastMs);
            }
        } else {
            struct Case {
                std::uint32_t w, h, order;
            };
            for (const Case& k : {Case{384, 256, 0}, Case{388, 260, 1}, Case{256, 200, 2}, Case{320, 240, 3}, Case{32, 32, 0}}) {
                const auto p = params(k.order);
                const auto in = scene(k.w, k.h, kOrders[k.order], 0x9e3779b9u ^ k.order);
                const auto want = cpuBin(in, k.w, k.h, p);
                const auto got = gpu.run(in, k.w, k.h, p);
                char name[96];
                std::snprintf(name, sizeof(name), "bin %ux%u order=%u", k.w, k.h, k.order);
                ok &= compare(name, got, want, k.w / 2);
                const std::uint32_t ow = k.w / 2, oh = k.h / 2;
                if (ow >= 64 && oh >= 64) {
                    const std::uint32_t rw = std::uint32_t(ow / std::sqrt(2.0)) & ~1u, rh = std::uint32_t(oh / std::sqrt(2.0)) & ~1u;
                    const auto gotR = gpu.run(in, k.w, k.h, p, rw, rh);
                    std::snprintf(name, sizeof(name), "resample %ux%u -> %ux%u", ow, oh, rw, rh);
                    ok &= compare(name, gotR, cpuResample(got, ow, oh, rw, rh, std::uint16_t(p.white * 4)), rw);
                }
            }
            // Direct 3x bin: every order, sizes with and without a centred crop.
            for (const Case& k : {Case{384, 288, 0}, Case{404, 292, 1}, Case{336, 260, 2}, Case{396, 300, 3}, Case{48, 48, 0}}) {
                const auto p = params(k.order);
                const auto in = scene(k.w, k.h, kOrders[k.order], 0x51ed2701u ^ k.order);
                const auto want = cpuBin3(in, k.w, k.h, p);
                const auto got = gpu.run(in, k.w, k.h, p, 0, 0, 3);
                char name[96];
                std::snprintf(name, sizeof(name), "bin3 %ux%u order=%u", k.w, k.h, k.order);
                ok &= compare(name, got, want, rawr::raw_ingress::BayerBinner::outputSize(k.w, k.h, 3).width);
            }
            // Flat colours through the 3x bin: each output is its own colour's level, in every order.
            for (std::uint32_t order = 0; order < 4; ++order) {
                const auto p = params(order);
                const std::uint32_t w = 192, h = 144;
                const float level[3] = {300.0f, 700.0f, 450.0f};
                Frame in(std::size_t(w) * h);
                for (std::uint32_t y = 0; y < h; ++y)
                    for (std::uint32_t x = 0; x < w; ++x)
                        in[std::size_t(y) * w + x] = std::uint16_t(level[kOrders[order][2 * (y & 1) + (x & 1)]] + p.black[2 * (y & 1) + (x & 1)]);
                const auto got = gpu.run(in, w, h, p, 0, 0, 3);
                const auto size = rawr::raw_ingress::BayerBinner::outputSize(w, h, 3);
                int worst = 0;
                for (std::uint32_t y = 0; y < size.height; ++y)
                    for (std::uint32_t x = 0; x < size.width; ++x) {
                        const int site = 2 * (y & 1) + (x & 1);
                        const int want = int(std::lround((level[kOrders[order][site]] + p.black[site]) * 4));
                        worst = std::max(worst, std::abs(int(got[std::size_t(y) * size.width + x]) - want));
                    }
                std::printf("%s bin3 flat colours order=%u max error %d codes\n", worst <= 2 ? "PASS" : "FAIL", order, worst);
                ok &= worst <= 2;
            }
            // Mirror symmetry: an RGGB scene mirrored in x is GRBG; its bin must be the mirrored bin.
            {
                const std::uint32_t w = 256, h = 192;
                const auto pr = params(0), pg = params(1);
                const auto in = scene(w, h, kOrders[0], 11);
                Frame mirrored(in.size());
                for (std::uint32_t y = 0; y < h; ++y)
                    for (std::uint32_t x = 0; x < w; ++x) mirrored[y * w + x] = in[y * w + (w - 1 - x)];
                Params pm = pg;
                for (int i = 0; i < 4; ++i) pm.black[i] = pr.black[i ^ 1], pm.gains[i] = pr.gains[i ^ 1];
                const auto a = gpu.run(in, w, h, pr), b = gpu.run(mirrored, w, h, pm);
                Frame bm(b.size());
                for (std::uint32_t y = 0; y < h / 2; ++y)
                    for (std::uint32_t x = 0; x < w / 2; ++x) bm[y * (w / 2) + x] = b[y * (w / 2) + (w / 2 - 1 - x)];
                ok &= compare("mirror x (RGGB vs GRBG)", bm, a, w / 2);
            }
        }
        vkDestroyFence(c.dev, fence, nullptr);
        vkDestroyCommandPool(c.dev, pool, nullptr);
    }
    vktest::delCtx(c);
    std::puts(ok ? "ALL PASS" : "SOME FAILED");
    return ok ? 0 : 1;
}
