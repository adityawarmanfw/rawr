// GPU RAW10 ingress must be bit-identical to the CPU fallback
// (imaging::unpackRaw10Row), on tight and padded row strides. Requires a
// Vulkan device (MoltenVK on macOS).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "imaging/RawPixelSource.h"
#include "rawr/raw_ingress/Raw10Unpacker.h"
#include "vk_test_common.hpp"

namespace {

bool runCase(vktest::Ctx& c, rawr::raw_ingress::Raw10Unpacker& unpacker, VkCommandBuffer cmd, VkFence fence,
             std::uint32_t slot, std::uint32_t width, std::uint32_t height, std::uint32_t strideBytes) {
    // Odd strides exercise byte addressing that straddles 32-bit words.
    std::vector<std::uint8_t> packed(std::size_t(strideBytes) * height);
    std::uint32_t state = 0x1234567u ^ strideBytes;
    for (auto& b : packed) {
        state = state * 1664525u + 1013904223u;
        b = std::uint8_t(state >> 24);
    }
    std::vector<std::uint16_t> expected(std::size_t(width) * height);
    for (std::uint32_t y = 0; y < height; ++y)
        rawrcam::imaging::unpackRaw10Row(packed.data() + std::size_t(y) * strideBytes, width,
                                         expected.data() + std::size_t(y) * width);

    const VkDeviceSize srcBytes = (packed.size() + 3u) & ~VkDeviceSize(3);
    const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    auto src = vktest::mkBuf(c.pd, c.dev, srcBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host);
    auto dst = vktest::mkBuf(c.pd, c.dev, VkDeviceSize(width) * height * 2u, VK_BUFFER_USAGE_TRANSFER_DST_BIT, host);
    auto img = vktest::mkImg(c.pd, c.dev, width, height, VK_FORMAT_R16_UINT,
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
    unpacker.record(cmd, slot, src.b, img.v, width, height, strideBytes);
    b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {width, height, 1};
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
    std::size_t mismatches = 0, first = 0;
    for (std::size_t i = 0; i < expected.size(); ++i)
        if (got[i] != expected[i] && mismatches++ == 0) first = i;
    vkUnmapMemory(c.dev, dst.m);
    vktest::delImg(c.dev, img);
    vktest::delBuf(c.dev, dst);
    vktest::delBuf(c.dev, src);
    if (mismatches) {
        std::printf("FAIL %ux%u stride=%u mismatches=%zu first=(%zu,%zu)\n", width, height, strideBytes, mismatches,
                    first % width, first / width);
        return false;
    }
    std::printf("PASS %ux%u stride=%u\n", width, height, strideBytes);
    return true;
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
            auto unpacker = rawr::raw_ingress::Raw10Unpacker::create({c.pd, c.dev, c.qf});
            constexpr std::uint32_t w = 4080, h = 64;  // a real sensor width
            ok = runCase(c, *unpacker, cmd, fence, 0, w, h, w / 4 * 5) && ok;         // tight
            ok = runCase(c, *unpacker, cmd, fence, 1, w, h, w / 4 * 5 + 64) && ok;    // aligned padding
            ok = runCase(c, *unpacker, cmd, fence, 2, w, h, w / 4 * 5 + 3) && ok;     // odd padding
            ok = runCase(c, *unpacker, cmd, fence, 0, 36, 7, 48) && ok;               // tiny, slot reuse
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
