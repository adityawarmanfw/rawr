#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

#include "post_demosaic/PostDemosaicProcessor.h"
#include "vk_test_common.hpp"
using namespace vktest;

static VkImageMemoryBarrier bar(VkImage im, VkAccessFlags s, VkAccessFlags d, VkImageLayout o, VkImageLayout n) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = s;
    b.dstAccessMask = d;
    b.oldLayout = o;
    b.newLayout = n;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return b;
}
static VkCommandBuffer cmdAlloc(const Ctx& c, VkCommandPool p) {
    VkCommandBuffer x{};
    VkCommandBufferAllocateInfo a{};
    a.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    a.commandPool = p;
    a.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    a.commandBufferCount = 1;
    ck(vkAllocateCommandBuffers(c.dev, &a, &x), "cmd alloc");
    return x;
}
static void begin(VkCommandBuffer c) {
    VkCommandBufferBeginInfo b{};
    b.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    ck(vkBeginCommandBuffer(c, &b), "cmd begin");
}
static void submit(const Ctx& c, VkCommandBuffer x) {
    ck(vkEndCommandBuffer(x), "cmd end");
    VkSubmitInfo s{};
    s.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    s.commandBufferCount = 1;
    s.pCommandBuffers = &x;
    ck(vkQueueSubmit(c.q, 1, &s, VK_NULL_HANDLE), "submit");
    ck(vkQueueWaitIdle(c.q), "wait");
}
static Img upload(const Ctx& c, VkCommandPool p, const std::vector<uint8_t>& bytes, uint32_t W, uint32_t H) {
    Buf st = mkBuf(c.pd, c.dev, bytes.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    void* m = nullptr;
    ck(vkMapMemory(c.dev, st.m, 0, bytes.size(), 0, &m), "map");
    std::memcpy(m, bytes.data(), bytes.size());
    vkUnmapMemory(c.dev, st.m);
    Img im = mkImg(c.pd, c.dev, W, H, VK_FORMAT_R16G16B16A16_SFLOAT,
                   VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    auto cb = cmdAlloc(c, p);
    begin(cb);
    auto b0 =
        bar(im.i, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b0);
    VkBufferImageCopy cp{};
    cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.imageExtent = {W, H, 1};
    vkCmdCopyBufferToImage(cb, st.b, im.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    auto b1 = bar(im.i, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_GENERAL);
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b1);
    submit(c, cb);
    vkFreeCommandBuffers(c.dev, p, 1, &cb);
    delBuf(c.dev, st);
    return im;
}
static std::vector<uint8_t> readback(const Ctx& c, VkCommandPool p, VkImage im, uint32_t W, uint32_t H) {
    const VkDeviceSize n = VkDeviceSize(size_t(W) * H * 8);
    Buf st = mkBuf(c.pd, c.dev, n, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    auto cb = cmdAlloc(c, p);
    begin(cb);
    auto b = bar(im, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    VkBufferImageCopy cp{};
    cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.imageExtent = {W, H, 1};
    vkCmdCopyImageToBuffer(cb, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, st.b, 1, &cp);
    submit(c, cb);
    void* m = nullptr;
    ck(vkMapMemory(c.dev, st.m, 0, n, 0, &m), "map out");
    std::vector<uint8_t> o;
    o.resize(static_cast<size_t>(n));
    std::memcpy(o.data(), m, size_t(n));
    vkUnmapMemory(c.dev, st.m);
    vkFreeCommandBuffers(c.dev, p, 1, &cb);
    delBuf(c.dev, st);
    return o;
}

// Compare actual GPU output against the unmirrored result, including odd axes,
// workgroup edges and the FCC output path. Reuse the processor across mirror changes.
int main() {
    try {
        Ctx c = ctx();
        VkCommandPool pool{};
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.queueFamilyIndex = c.qf;
        ck(vkCreateCommandPool(c.dev, &pi, nullptr, &pool), "pool");
        for (const auto size : {std::array<uint32_t, 2>{3, 5}, {17, 19}, {32, 18}}) {
            const auto w = size[0], h = size[1];
            std::vector<uint16_t> pixels(size_t(w) * h * 4);
            for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = uint16_t(0x3000 + i % 1024);
            std::vector<uint8_t> bytes(pixels.size() * 2);
            std::memcpy(bytes.data(), pixels.data(), bytes.size());
            for (uint32_t fcc : {0u, 1u}) {
                rawr::post::PostDemosaicProcessor processor(c.pd, c.dev, c.qf, w, h, fcc);
                std::vector<uint8_t> baseline;
                for (uint32_t mirror : {0u, 1u, 2u, 3u, 0u}) {
                    Img src = upload(c, pool, bytes, w, h);
                    auto cmd = cmdAlloc(c, pool);
                    begin(cmd);
                    rawr::post::StillDistortionCorrection correction;
                    correction.mirror = mirror;
                    processor.record(cmd, src.i, src.v, VK_NULL_HANDLE, VK_NULL_HANDLE,
                                     {1.0f, 1.0f, 1.0f}, false, correction);
                    submit(c, cmd);
                    vkFreeCommandBuffers(c.dev, pool, 1, &cmd);
                    const auto result = readback(c, pool, processor.outputImage(), w, h);
                    // Restore the layout expected by the next record.
                    cmd = cmdAlloc(c, pool);
                    begin(cmd);
                    auto restore = bar(processor.outputImage(), VK_ACCESS_TRANSFER_READ_BIT,
                                       VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
                    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                         0, 0, nullptr, 0, nullptr, 1, &restore);
                    submit(c, cmd);
                    vkFreeCommandBuffers(c.dev, pool, 1, &cmd);
                    if (baseline.empty()) baseline = result;
                    for (uint32_t y = 0; y < h; ++y) {
                        for (uint32_t x = 0; x < w; ++x) {
                            const uint32_t sx = mirror & 1u ? w - 1 - x : x;
                            const uint32_t sy = mirror & 2u ? h - 1 - y : y;
                            if (std::memcmp(result.data() + (size_t(y) * w + x) * 8,
                                            baseline.data() + (size_t(sy) * w + sx) * 8, 8) != 0)
                                throw std::runtime_error("mirror output mismatch: " + std::to_string(w) + "x" +
                                    std::to_string(h) + " fcc=" + std::to_string(fcc) + " mirror=" + std::to_string(mirror));
                        }
                    }
                    delImg(c.dev, src);
                }
            }
        }
        vkDestroyCommandPool(c.dev, pool, nullptr);
        delCtx(c);
        std::cout << "mirror_validate passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
