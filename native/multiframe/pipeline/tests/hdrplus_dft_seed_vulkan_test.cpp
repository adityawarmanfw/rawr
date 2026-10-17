// The two destinations of a seeded DFT must match independent transforms
// bit for bit. Distinct inputs exercise flat, impulse, signed and HDR values.
#include <rawr/raw_gpu_pipeline/VulkanExecutor.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "vk_test_common.hpp"
namespace {
struct Gpu {
    vktest::Ctx c = vktest::ctx();
    VkCommandPool pool{};
    VkCommandBuffer cmd{};
    VkFence fence{};
    Gpu() {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = c.qf;
        vktest::ck(vkCreateCommandPool(c.dev, &pi, nullptr, &pool), "pool");
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        vktest::ck(vkAllocateCommandBuffers(c.dev, &ai, &cmd), "cmd");
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vktest::ck(vkCreateFence(c.dev, &fi, nullptr, &fence), "fence");
    }
    ~Gpu() {
        vkDeviceWaitIdle(c.dev);
        vkDestroyFence(c.dev, fence, nullptr);
        vkDestroyCommandPool(c.dev, pool, nullptr);
        vktest::delCtx(c);
    }
    template <typename F>
    void oneShot(F&& record) {
        vktest::ck(vkResetCommandBuffer(cmd, 0), "reset");
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vktest::ck(vkBeginCommandBuffer(cmd, &bi), "begin");
        record(cmd);
        vktest::ck(vkEndCommandBuffer(cmd), "end");
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        vktest::ck(vkResetFences(c.dev, 1, &fence), "reset fence");
        vktest::ck(vkQueueSubmit(c.q, 1, &si, fence), "submit");
        vktest::ck(vkWaitForFences(c.dev, 1, &fence, VK_TRUE, UINT64_MAX), "wait");
    }
};

void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
}

void checkSeed(Gpu& gpu) {
    using namespace rawr::raw_gpu_pipeline;
    constexpr uint32_t w = 24, h = 16;
    constexpr VkDeviceSize inputBytes = w * h * 4 * sizeof(float), outputBytes = 2 * inputBytes;
    auto& c = gpu.c;
    const auto usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    auto src = vktest::mkImg(c.pd, c.dev, w, h, VK_FORMAT_R32G32B32A32_SFLOAT, usage);
    std::array<vktest::Img, 4> dst;
    for (auto& img : dst) img = vktest::mkImg(c.pd, c.dev, w * 2, h, VK_FORMAT_R32G32B32A32_SFLOAT, usage);
    auto upload = vktest::mkBuf(c.pd, c.dev, inputBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    auto readback = vktest::mkBuf(c.pd, c.dev, outputBytes * dst.size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VulkanExecutor exec;
    exec.initialize(c.dev, false);
    for (int pattern = 0; pattern < 4; ++pattern) {
        void* mapped = nullptr;
        vktest::ck(vkMapMemory(c.dev, upload.m, 0, inputBytes, 0, &mapped), "upload map");
        auto* data = static_cast<float*>(mapped);
        for (unsigned i = 0; i < inputBytes / sizeof(float); ++i)
            data[i] = pattern == 0 ? 64.0f
                      : pattern == 1
                          ? (i % 257 == 0 ? 65535.0f : 0.0f)
                          : float(int((i * 1664525u + 1013904223u) % 131071u) - 65535) * (pattern == 2 ? 1.0f : 0.001f);
        vkUnmapMemory(c.dev, upload.m);
        exec.beginBatch();
        gpu.oneShot([&](VkCommandBuffer cmd) {
            barrier(cmd, src.i, pattern == 0 ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {w, h, 1};
            vkCmdCopyBufferToImage(cmd, upload.b, src.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            barrier(cmd, src.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
            for (auto& img : dst)
                barrier(cmd, img.i, pattern == 0 ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_IMAGE_LAYOUT_GENERAL);
            const int tiles[2] = {w / 8, h / 8};
            const auto ib = [](uint32_t n, const vktest::Img& img) {
                return ImageBinding{n, img.v, VK_IMAGE_LAYOUT_GENERAL};
            };
            for (int i = 0; i < 2; ++i)
                exec.record(cmd, ShaderId::HdrqForwardDft, {{ib(0, src), ib(1, dst[i])}, {}}, tiles, sizeof(tiles),
                            tiles[0], tiles[1]);
            exec.record(cmd, ShaderId::HdrqSeedDft, {{ib(0, src), ib(1, dst[2]), ib(2, dst[3])}, {}}, tiles,
                        sizeof(tiles), tiles[0], tiles[1]);
            for (unsigned i = 0; i < dst.size(); ++i) {
                barrier(cmd, dst[i].i, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                copy.bufferOffset = i * outputBytes;
                copy.imageExtent = {w * 2, h, 1};
                vkCmdCopyImageToBuffer(cmd, dst[i].i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.b, 1, &copy);
            }
        });
        vktest::ck(vkMapMemory(c.dev, readback.m, 0, VK_WHOLE_SIZE, 0, &mapped), "readback map");
        for (unsigned i = 1; i < dst.size(); ++i)
            if (std::memcmp(mapped, static_cast<char*>(mapped) + i * outputBytes, outputBytes) != 0)
                throw std::runtime_error("seeded DFT differs from independent DFT");
        vkUnmapMemory(c.dev, readback.m);
    }
    exec.reset();
    vktest::delBuf(c.dev, upload);
    vktest::delBuf(c.dev, readback);
    for (auto& img : dst) vktest::delImg(c.dev, img);
    vktest::delImg(c.dev, src);
}
}  // namespace
int main() {
    try {
        Gpu gpu;
        checkSeed(gpu);
        std::puts("hdrplus_dft_seed_vulkan_test OK (bit-identical)");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
