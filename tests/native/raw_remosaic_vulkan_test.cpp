// GPU block-CFA remosaic (native/raw_ingress) vs the CPU reference
// (imaging/CfaRemosaic.h), from RAW16 rows (tight and padded) and MIPI RAW10,
// quad and 4x4 blocks, every colour order. Both round to whole DN; float
// evaluation order may flip a rounding, so a few 1 DN differences pass.
// Requires a Vulkan device (MoltenVK on macOS).
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "capture/StripRemosaic.h"
#include "imaging/CfaRemosaic.h"
#include "imaging/PhaseFit.h"
#include "imaging/RawPixelSource.h"
#include "rawr/raw_ingress/RawRemosaicker.h"
#include "vk_test_common.hpp"

namespace {

struct Case {
    std::uint32_t width, height, block, order, strideBytes;
    bool raw10;
    bool fast = false;
    bool high = false;
    bool reduce = false;
    bool detail = false;
    bool fit = false;  // Hexadeca reduction with the phase-response fit; scene gets a per-position response
};

bool runCase(vktest::Ctx& c, rawr::raw_ingress::RawRemosaicker& remosaicker, VkCommandBuffer cmd, VkFence fence,
             std::uint32_t slot, const Case& k, const char* input = nullptr, const char* output = nullptr) {
    // Scene: smooth colour field plus noise, 10-bit range.
    std::vector<std::uint16_t> pixels(std::size_t(k.width) * k.height);
    std::uint32_t state = 0x9e3779b9u ^ (k.block * 31u + k.order);
    if (!input)
        for (std::uint32_t y = 0; y < k.height; ++y)
            for (std::uint32_t x = 0; x < k.width; ++x) {
                state = state * 1664525u + 1013904223u;
                const std::uint32_t ch = rawrcam::imaging::cfaChannel(k.order, x / k.block, y / k.block);
                const float v = 64.0f + (ch == 1u ? 500.0f : 260.0f) * (1.0f + 0.5f * std::sin(x * 0.05f + y * 0.03f)) +
                                float(state >> 28);
                float value = v;
                if (k.fit) {
                    const std::uint32_t type = (((y >> 2) & 1u) << 1) | ((x >> 2) & 1u), p = ((y & 3u) << 2) | (x & 3u);
                    value = 64.0f + (v - 64.0f) * (1.0f + 0.06f * std::sin(1.7f * float(type * 16 + p)));
                }
                pixels[std::size_t(y) * k.width + x] = std::uint16_t(std::min(1023.0f, value));
            }
    if (input) {
        std::ifstream file(input, std::ios::binary | std::ios::ate);
        if (!file || file.tellg() != std::streamoff(pixels.size() * 2))
            throw std::runtime_error("RAW16 input size mismatch");
        file.seekg(0);
        file.read(reinterpret_cast<char*>(pixels.data()), pixels.size() * 2);
        if (!file) throw std::runtime_error("RAW16 input read failed");
        for (auto v : pixels)
            if (v > 16383) throw std::runtime_error("fixed-point source exceeds 14-bit range");
    }
    std::vector<std::uint8_t> packed(std::size_t(k.strideBytes) * k.height, 0);
    for (std::uint32_t y = 0; y < k.height; ++y) {
        auto* row = packed.data() + std::size_t(y) * k.strideBytes;
        const auto* src = pixels.data() + std::size_t(y) * k.width;
        if (!k.raw10) {
            std::memcpy(row, src, std::size_t(k.width) * 2u);
            continue;
        }
        for (std::uint32_t x = 0; x < k.width; x += 4) {
            std::uint8_t* g = row + x / 4 * 5;
            g[4] = 0;
            for (std::uint32_t i = 0; i < 4; ++i) {
                g[i] = std::uint8_t(src[x + i] >> 2);
                g[4] |= std::uint8_t((src[x + i] & 3u) << (2 * i));
            }
        }
    }
    const auto ow = k.reduce ? k.width / 2 : k.width, oh = k.reduce ? k.height / 2 : k.height;
    auto expected = pixels;
    if (k.reduce && k.fit) {
        expected.resize(std::size_t(ow) * oh);
        rawrcam::imaging::hexadecaSumWithFit(pixels.data(), k.width, k.height, 64.0f, 3, expected.data(), 2);
    } else if (k.reduce) {
        expected.resize(std::size_t(ow) * oh);
        for (unsigned y = 0; y < oh; ++y)
            for (unsigned x = 0; x < ow; ++x) {
                const auto i = std::size_t(2 * y) * k.width + 2 * x;
                expected[std::size_t(y) * ow + x] =
                    pixels[i] + pixels[i + 1] + pixels[i + k.width] + pixels[i + k.width + 1];
            }
    }
    rawrcam::imaging::remosaicBlockCfa(
        expected.data(), ow, oh, k.reduce ? 2u : k.block, k.order, 1,
        k.detail ? rawrcam::imaging::CfaRemosaicQuality::Detail
        : k.high
            ? rawrcam::imaging::CfaRemosaicQuality::High
            : (k.fast ? rawrcam::imaging::CfaRemosaicQuality::Fast : rawrcam::imaging::CfaRemosaicQuality::Standard),
        k.reduce ? 4.0f : 1.0f, 64.0f * (k.reduce ? 4.0f : 1.0f));  // Frame::blackLevel default, in sample units

    const VkDeviceSize srcBytes = (packed.size() + 3u) & ~VkDeviceSize(3);
    const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    auto src = vktest::mkBuf(c.pd, c.dev, srcBytes,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, host);
    auto dst = vktest::mkBuf(c.pd, c.dev, VkDeviceSize(ow) * oh * 2u, VK_BUFFER_USAGE_TRANSFER_DST_BIT, host);
    auto img = vktest::mkImg(c.pd, c.dev, ow, oh, VK_FORMAT_R16_UINT,
                             VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    void* mapped = nullptr;
    vktest::ck(vkMapMemory(c.dev, src.m, 0, VK_WHOLE_SIZE, 0, &mapped), "map src");
    std::memset(mapped, 0, srcBytes);
    std::memcpy(mapped, packed.data(), packed.size());
    vkUnmapMemory(c.dev, src.m);

    vktest::ck(vkResetCommandBuffer(cmd, 0), "reset");
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vktest::ck(vkBeginCommandBuffer(cmd, &bi), "begin");
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img.i;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    rawr::raw_ingress::RawRemosaicker::Frame f;
    f.source = src.b;
    f.destination = img.v;
    f.width = k.width;
    f.height = k.height;
    f.rowStrideBytes = k.strideBytes;
    f.raw10 = k.raw10;
    f.block = k.block;
    f.order = k.order;
    f.quality = k.fast ? rawr::raw_ingress::RawRemosaicker::Quality::Fast
                       : rawr::raw_ingress::RawRemosaicker::Quality::Standard;
    if (k.high) f.quality = rawr::raw_ingress::RawRemosaicker::Quality::High;
    if (k.detail) f.quality = rawr::raw_ingress::RawRemosaicker::Quality::Detail;
    f.hexadecaToQuad = k.reduce;
    f.phaseFit = k.fit;
    remosaicker.record(cmd, slot, f);
    b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {ow, oh, 1};
    vkCmdCopyImageToBuffer(cmd, img.i, VK_IMAGE_LAYOUT_GENERAL, dst.b, 1, &r);
    vktest::ck(vkEndCommandBuffer(cmd), "end");
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vktest::ck(vkResetFences(c.dev, 1, &fence), "reset fence");
    const auto start = std::chrono::steady_clock::now();
    vktest::ck(vkQueueSubmit(c.q, 1, &si, fence), "submit");
    vktest::ck(vkWaitForFences(c.dev, 1, &fence, VK_TRUE, UINT64_MAX), "wait");

    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    vktest::ck(vkMapMemory(c.dev, dst.m, 0, VK_WHOLE_SIZE, 0, &mapped), "map dst");
    const auto* got = static_cast<const std::uint16_t*>(mapped);
    std::size_t off1 = 0, off2 = 0, worse = 0, first = 0;
    int maxError = 0;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const int d = std::abs(int(got[i]) - int(expected[i]));
        if (d == 1) ++off1;
        if (d == 2) ++off2;
        maxError = std::max(maxError, d);
        // High's quantized green is reused in chroma statistics: a one-code
        // rounding difference can propagate to a second output code.
        if (d > (k.fit ? 4 : (k.high || k.detail ? 2 : 1)) && worse++ == 0) first = i;
    }
    if (output) {
        std::ofstream file(output, std::ios::binary);
        file.write(reinterpret_cast<const char*>(got), expected.size() * 2);
        if (!file) throw std::runtime_error("output write failed");
        std::printf("Submit+wait (includes readback): %.6f seconds, %zu output samples\n", elapsed, expected.size());
    }
    vkUnmapMemory(c.dev, dst.m);
    vktest::delImg(c.dev, img);
    vktest::delBuf(c.dev, dst);
    vktest::delBuf(c.dev, src);
    // The fit's curve comes from float log/interp of integer statistics: a block on the
    // flatness or bin boundary can flip between GPU and CPU, moving the curve a little.
    // Its own noise is far larger than 1 DN (4 codes), so the fit cases get that slack.
    const bool ok = worse == 0 && (off1 + off2) * (k.fit ? 200 : 1000) < expected.size();
    std::printf("%s %ux%u block=%u order=%u %s %s stride=%u off1=%zu off2=%zu max=%d worse=%zu first=(%zu,%zu)\n",
                ok ? "PASS" : "FAIL", k.width, k.height, k.block, k.order,
                k.reduce ? (k.fit ? "reduce-detail-fit" : (k.detail ? "reduce-detail" : "reduce-high"))
                         : (k.detail ? "detail" : (k.high ? "high" : (k.fast ? "fast" : "standard"))),
                k.raw10 ? "RAW10" : "RAW16", k.strideBytes, off1, off2, maxError, worse, first % ow, first / ow);
    return ok;
}

// BlockRemosaicGpu's strip loop (capture/StripRemosaic.h, the production header) with a GPU window pass,
// against the same pass over the whole frame (must be bit-identical) and the CPU reference.
bool runStripped(vktest::Ctx& c, rawr::raw_ingress::RawRemosaicker& remosaicker, VkCommandBuffer cmd, VkFence fence,
                 std::uint32_t width, std::uint32_t height, std::uint32_t block, std::uint32_t order,
                 rawrcam::imaging::CfaRemosaicQuality quality, std::uint32_t strips, const char* input = nullptr,
                 const char* output = nullptr) {
    std::vector<std::uint8_t> frame(std::size_t(width) * height * 2);
    auto* px = reinterpret_cast<std::uint16_t*>(frame.data());
    if (input) {
        std::ifstream file(input, std::ios::binary);
        file.read(reinterpret_cast<char*>(frame.data()), frame.size());
        if (!file || file.peek() != EOF) throw std::runtime_error("RAW16 input size mismatch");
    } else {
        std::uint32_t state = 0x9e3779b9u ^ (block * 31u + order);
        for (std::uint32_t y = 0; y < height; ++y)
            for (std::uint32_t x = 0; x < width; ++x) {
                state = state * 1664525u + 1013904223u;
                const auto ch = rawrcam::imaging::cfaChannel(order, x / block, y / block);
                const float v = 64.0f + (ch == 1u ? 500.0f : 260.0f) * (1.0f + 0.5f * std::sin(x * 0.05f + y * 0.03f)) +
                                ((x / 37 + y / 23) % 2 ? 150.0f : 0.0f) + float(state >> 28);
                px[std::size_t(y) * width + x] = std::uint16_t(std::min(1023.0f, v));
            }
    }
    const auto gpu = [&](std::uint32_t nStrips) {
        const auto layout = rawrcam::capture::StripLayout::make(height, nStrips);
        const VkDeviceSize bytes = VkDeviceSize(width) * layout.window * 2u;
        const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        auto buf = vktest::mkBuf(c.pd, c.dev, bytes,
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 host);
        auto img = vktest::mkImg(c.pd, c.dev, width, layout.window, VK_FORMAT_R16_UINT,
                                 VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        void* mapped = nullptr;
        vktest::ck(vkMapMemory(c.dev, buf.m, 0, VK_WHOLE_SIZE, 0, &mapped), "map");
        auto out = frame;
        rawrcam::capture::remosaicInStrips(
            out, width, height, layout, static_cast<std::uint8_t*>(mapped),
            [&](std::uint8_t*, std::uint32_t) {
                vktest::ck(vkResetCommandBuffer(cmd, 0), "reset");
                VkCommandBufferBeginInfo bi{};
                bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                vktest::ck(vkBeginCommandBuffer(cmd, &bi), "begin");
                VkImageMemoryBarrier b{};
                b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.image = img.i;
                b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                                     nullptr, 0, nullptr, 1, &b);
                rawr::raw_ingress::RawRemosaicker::Frame f;
                f.source = buf.b;
                f.destination = img.v;
                f.width = width;
                f.height = layout.window;
                f.rowStrideBytes = width * 2;
                f.block = block;
                f.order = order;
                f.quality = static_cast<rawr::raw_ingress::RawRemosaicker::Quality>(static_cast<unsigned>(quality));
                remosaicker.record(cmd, 0, f);
                b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                     nullptr, 0, nullptr, 1, &b);
                VkBufferImageCopy r{};
                r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                r.imageExtent = {width, layout.window, 1};
                vkCmdCopyImageToBuffer(cmd, img.i, VK_IMAGE_LAYOUT_GENERAL, buf.b, 1, &r);  // result over the source
                vktest::ck(vkEndCommandBuffer(cmd), "end");
                VkSubmitInfo si{};
                si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                si.commandBufferCount = 1;
                si.pCommandBuffers = &cmd;
                vktest::ck(vkResetFences(c.dev, 1, &fence), "reset fence");
                vktest::ck(vkQueueSubmit(c.q, 1, &si, fence), "submit");
                vktest::ck(vkWaitForFences(c.dev, 1, &fence, VK_TRUE, UINT64_MAX), "wait");
            });
        vkUnmapMemory(c.dev, buf.m);
        vktest::delImg(c.dev, img);
        vktest::delBuf(c.dev, buf);
        return std::make_pair(out, layout);
    };
    const auto [whole, wholeLayout] = gpu(1);
    const auto [stripped, layout] = gpu(strips);
    auto cpu = frame;
    rawrcam::imaging::remosaicBlockCfa(cpu, width, height, block, order, 1, quality, 1.0f, 64.0f);  // Frame::blackLevel default
    std::size_t differsFromWhole = 0, cpuOff = 0, cpuWorse = 0;
    const auto* a = reinterpret_cast<const std::uint16_t*>(whole.data());
    const auto* b2 = reinterpret_cast<const std::uint16_t*>(stripped.data());
    const auto* r = reinterpret_cast<const std::uint16_t*>(cpu.data());
    for (std::size_t i = 0; i < frame.size() / 2; ++i) {
        differsFromWhole += a[i] != b2[i];
        const int d = std::abs(int(b2[i]) - int(r[i]));
        cpuOff += d != 0;
        cpuWorse += d > 2;
    }
    if (output) {
        std::ofstream file(output, std::ios::binary);
        file.write(reinterpret_cast<const char*>(stripped.data()), stripped.size());
    }
    const bool ok = differsFromWhole == 0 && (input || cpuWorse == 0);  // a file: CPU numbers are informational
    std::printf("%s %ux%u block=%u order=%u q=%u strips=%u (core=%u window=%u) vs-whole-gpu=%zu cpu-off=%zu cpu-worse=%zu\n",
                ok ? "PASS" : "FAIL", width, height, block, order, unsigned(quality), layout.count(height), layout.core,
                layout.window, differsFromWhole, cpuOff, cpuWorse);
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        auto c = vktest::ctx();
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
        bool ok = true;
        {
            auto programs = rawr::raw_ingress::RawRemosaicker::prepare({c.pd, c.dev, c.qf});
            auto remosaicker = rawr::raw_ingress::RawRemosaicker::create({c.pd, c.dev, c.qf}, programs);
            if (argc == 1) {
                // Capture-local descriptors/scratch may die while the programs remain reusable.
                for (int capture = 0; capture < 3; ++capture) {
                    auto next = rawr::raw_ingress::RawRemosaicker::create({c.pd, c.dev, c.qf}, programs);
                    ok = runCase(c, *next, cmd, fence, 0,
                                 {136, 72, 2, 3, 272, false, false, false, false, true, false}) && ok;
                }
            }
            programs.reset();  // the surviving remosaicker must retain its programs

            if (argc != 1) {
                if (argc != 6 && argc != 7)
                    throw std::runtime_error(
                        "usage: raw_remosaic_vulkan_test RAW16 WIDTH HEIGHT ORDER OUTPUT_RAW16 [high|detail|fit|quad]");
                unsigned iw = std::stoul(argv[2]), ih = std::stoul(argv[3]), order = std::stoul(argv[4]);
                if (argc == 7 && std::string(argv[6]) == "strips")  // quad Detail in 4 strips vs whole frame
                    ok = runStripped(c, *remosaicker, cmd, fence, iw, ih, 2, order,
                                     rawrcam::imaging::CfaRemosaicQuality::Detail, 4, argv[1], argv[5]);
                else if (argc == 7 && std::string(argv[6]) == "quad")  // native 2x2 quad, Detail (real-image parity)
                    ok = runCase(c, *remosaicker, cmd, fence, 0,
                                 {iw, ih, 2, order, iw * 2, false, false, false, false, true, false}, argv[1], argv[5]);
                else
                    ok = runCase(
                        c, *remosaicker, cmd, fence, 0,
                        {iw, ih, 4, order, iw * 2, false, false, true, true,
                         !(argc == 7 && std::string(argv[6]) == "high"), argc == 7 && std::string(argv[6]) == "fit"},
                        argv[1], argv[5]);
            } else {
                for (unsigned order = 0; order < 4; ++order) {
                    for (bool raw10 : {false, true}) {
                        ok = runCase(c, *remosaicker, cmd, fence, order % 3,
                                     {136, 72, 4, order, raw10 ? 173u : 280u, raw10, false, true, true}) &&
                             ok;
                        ok = runCase(c, *remosaicker, cmd, fence, order % 3,
                                     {68, 36, 2, order, raw10 ? 88u : 144u, raw10, false, true, false}) &&
                             ok;
                    }
                }
                // Detail: native Quad and Hexadeca reduction, both orders of storage.
                for (unsigned order = 0; order < 4; ++order)
                    for (bool raw10 : {false, true}) {
                        ok = runCase(c, *remosaicker, cmd, fence, order % 3,
                                     {68, 36, 2, order, raw10 ? 88u : 144u, raw10, false, false, false, true}) &&
                             ok;
                        ok = runCase(c, *remosaicker, cmd, fence, order % 3,
                                     {136, 72, 4, order, raw10 ? 173u : 280u, raw10, false, false, true, true}) &&
                             ok;
                    }
                // Detail across several 32x32 workgroup tiles and partial edge tiles, every order and both storages.
                for (unsigned order = 0; order < 4; ++order)
                    for (bool raw10 : {false, true})
                        ok = runCase(c, *remosaicker, cmd, fence, order % 3,
                                     {136, 72, 2, order, raw10 ? 173u : 272u, raw10, false, false, false, true, false}) &&
                             ok;
                for (unsigned order : {0u, 3u})
                    ok = runCase(c, *remosaicker, cmd, fence, 0,
                                 {250, 186, 2, order, 500, false, false, false, false, true, false}) &&
                         ok;
                // Partial bottom block: the public record API permits odd heights.
                for (unsigned order = 0; order < 4; ++order)
                    ok = runCase(c, *remosaicker, cmd, fence, order % 3,
                                 {136, 73, 2, order, 272, false, false, false, false, true, false}) && ok;
                // Phase-response fit: a scene big enough for the statistics, both storages.
                for (unsigned order : {0u, 3u})
                    for (bool raw10 : {false, true})
                        ok = runCase(c, *remosaicker, cmd, fence, order % 3,
                                     {1024, 512, 4, order, raw10 ? 1283u : 2048u, raw10, false, false, true, true, true}) &&
                             ok;
                // Strip loop of BlockRemosaicGpu: every quality, both blocks, heights with and without a layout.
                for (unsigned order : {0u, 3u})
                    for (auto q : {rawrcam::imaging::CfaRemosaicQuality::Standard,
                                   rawrcam::imaging::CfaRemosaicQuality::High,
                                   rawrcam::imaging::CfaRemosaicQuality::Detail})
                        for (unsigned h : {1024u, 1000u, 520u})
                            ok = runStripped(c, *remosaicker, cmd, fence, 256, h, 2, order, q, 4) && ok;
                ok = runStripped(c, *remosaicker, cmd, fence, 256, 1024, 4, 1,
                                 rawrcam::imaging::CfaRemosaicQuality::Standard, 3) &&
                     ok;
                // Identical geometry and slot, but new source/destination lifetimes.
                // Driver-recycled handles must not skip descriptor refresh.
                for (int repeat = 0; repeat < 3; ++repeat)
                    ok = runCase(c, *remosaicker, cmd, fence, 0,
                                 {136, 72, 4, 3, 280, false, false, true, true}) && ok;
                constexpr std::uint32_t w = 4080, h = 48;  // a real sensor width
                std::uint32_t slot = 0;
                for (std::uint32_t block : {2u, 4u})
                    for (std::uint32_t order = 0; order < 4; ++order)
                        ok = runCase(c, *remosaicker, cmd, fence, slot++ % 3, {w, h, block, order, w * 2, false}) && ok;
                ok = runCase(c, *remosaicker, cmd, fence, 0, {w, h, 2, 3, w * 2 + 128, false}) && ok;   // padded RAW16
                ok = runCase(c, *remosaicker, cmd, fence, 1, {w, h, 4, 0, w / 4 * 5 + 3, true}) && ok;  // RAW10
                ok = runCase(c, *remosaicker, cmd, fence, 2, {40, 24, 2, 0, 80, false}) && ok;          // tiny
                for (std::uint32_t block : {2u, 4u})
                    for (std::uint32_t order : {0u, 3u})
                        ok = runCase(c, *remosaicker, cmd, fence, slot++ % 3,
                                     {w, h, block, order, w * 2, false, true}) &&
                             ok;
            }
        }
        vkDestroyFence(c.dev, fence, nullptr);
        vkDestroyCommandPool(c.dev, pool, nullptr);
        vktest::delCtx(c);
        return ok ? 0 : 1;
    } catch (const std::exception& e) {
        std::printf("FAIL %s\n", e.what());
        return 1;
    }
}
