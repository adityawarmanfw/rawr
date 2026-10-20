// GPU block-CFA remosaic (native/raw_ingress) vs the CPU reference
// (imaging/CfaRemosaic.h), from RAW16 rows (tight and padded) and MIPI RAW10,
// quad and 4x4 blocks, every colour order. Both round to whole DN; float
// evaluation order may flip a rounding, so a few 1 DN differences pass.
// Requires a Vulkan device (MoltenVK on macOS).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "imaging/CfaRemosaic.h"
#include "imaging/RawPixelSource.h"
#include "rawr/raw_ingress/RawRemosaicker.h"
#include "vk_test_common.hpp"

namespace {

struct Case {
    std::uint32_t width, height, block, order, strideBytes;
    bool raw10;
    bool fast = false;
};

bool runCase(vktest::Ctx& c, rawr::raw_ingress::RawRemosaicker& remosaicker, VkCommandBuffer cmd, VkFence fence,
             std::uint32_t slot, const Case& k) {
    // Scene: smooth colour field plus noise, 10-bit range.
    std::vector<std::uint16_t> pixels(std::size_t(k.width) * k.height);
    std::uint32_t state = 0x9e3779b9u ^ (k.block * 31u + k.order);
    for (std::uint32_t y = 0; y < k.height; ++y)
        for (std::uint32_t x = 0; x < k.width; ++x) {
            state = state * 1664525u + 1013904223u;
            const std::uint32_t ch = rawrcam::imaging::cfaChannel(k.order, x / k.block, y / k.block);
            const float v = 64.0f + (ch == 1u ? 500.0f : 260.0f) * (1.0f + 0.5f * std::sin(x * 0.05f + y * 0.03f)) +
                            float(state >> 28);
            pixels[std::size_t(y) * k.width + x] = std::uint16_t(std::min(1023.0f, v));
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
    auto expected = pixels;
    rawrcam::imaging::remosaicBlockCfa(
        expected.data(), k.width, k.height, k.block, k.order, 1,
        k.fast ? rawrcam::imaging::CfaRemosaicQuality::Fast : rawrcam::imaging::CfaRemosaicQuality::Standard);

    const VkDeviceSize srcBytes = (packed.size() + 3u) & ~VkDeviceSize(3);
    const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    auto src = vktest::mkBuf(c.pd, c.dev, srcBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host);
    auto dst = vktest::mkBuf(c.pd, c.dev, VkDeviceSize(k.width) * k.height * 2u, VK_BUFFER_USAGE_TRANSFER_DST_BIT, host);
    auto img = vktest::mkImg(c.pd, c.dev, k.width, k.height, VK_FORMAT_R16_UINT,
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
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &b);
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
    remosaicker.record(cmd, slot, f);
    b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {k.width, k.height, 1};
    vkCmdCopyImageToBuffer(cmd, img.i, VK_IMAGE_LAYOUT_GENERAL, dst.b, 1, &r);
    vktest::ck(vkEndCommandBuffer(cmd), "end");
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vktest::ck(vkResetFences(c.dev, 1, &fence), "reset fence");
    vktest::ck(vkQueueSubmit(c.q, 1, &si, fence), "submit");
    vktest::ck(vkWaitForFences(c.dev, 1, &fence, VK_TRUE, UINT64_MAX), "wait");

    vktest::ck(vkMapMemory(c.dev, dst.m, 0, VK_WHOLE_SIZE, 0, &mapped), "map dst");
    const auto* got = static_cast<const std::uint16_t*>(mapped);
    std::size_t off1 = 0, worse = 0, first = 0;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const int d = std::abs(int(got[i]) - int(expected[i]));
        if (d == 1) ++off1;
        if (d > 1 && worse++ == 0) first = i;
    }
    vkUnmapMemory(c.dev, dst.m);
    vktest::delImg(c.dev, img);
    vktest::delBuf(c.dev, dst);
    vktest::delBuf(c.dev, src);
    const bool ok = worse == 0 && off1 * 1000 < expected.size();
    std::printf("%s %ux%u block=%u order=%u %s %s stride=%u off1=%zu worse=%zu first=(%zu,%zu)\n",
                ok ? "PASS" : "FAIL", k.width, k.height, k.block, k.order, k.fast ? "fast" : "standard",
                k.raw10 ? "RAW10" : "RAW16", k.strideBytes, off1, worse,
                first % k.width, first / k.width);
    return ok;
}

}  // namespace

int main() {
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
            auto remosaicker = rawr::raw_ingress::RawRemosaicker::create({c.pd, c.dev, c.qf});
            constexpr std::uint32_t w = 4080, h = 48;  // a real sensor width
            std::uint32_t slot = 0;
            for (std::uint32_t block : {2u, 4u})
                for (std::uint32_t order = 0; order < 4; ++order)
                    ok = runCase(c, *remosaicker, cmd, fence, slot++ % 3, {w, h, block, order, w * 2, false}) && ok;
            ok = runCase(c, *remosaicker, cmd, fence, 0, {w, h, 2, 3, w * 2 + 128, false}) && ok;   // padded RAW16
            ok = runCase(c, *remosaicker, cmd, fence, 1, {w, h, 4, 0, w / 4 * 5 + 3, true}) && ok;  // RAW10
            ok = runCase(c, *remosaicker, cmd, fence, 2, {40, 24, 2, 0, 80, false}) && ok;         // tiny
            for (std::uint32_t block : {2u, 4u})
                for (std::uint32_t order : {0u, 3u})
                    ok = runCase(c, *remosaicker, cmd, fence, slot++ % 3, {w, h, block, order, w * 2, false, true}) && ok;
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
