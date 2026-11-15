// Standalone scheduling benchmark. No camera/app integration.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>

#include "rawr/raw_ingress/RawRemosaicker.h"
#include "vk_test_common.hpp"
using Clock = std::chrono::steady_clock;
using Remosaic = rawr::raw_ingress::RawRemosaicker;
double ms(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }
int main(int argc, char** argv) {
    try {
        if (argc != 7) throw std::runtime_error("usage: INPUT.raw16 WIDTH HEIGHT CFA_ORDER FIT(0|1) ROUNDS");
        const unsigned w = std::stoul(argv[2]), h = std::stoul(argv[3]), order = std::stoul(argv[4]);
        const bool fit = std::stoul(argv[5]) != 0;
        const unsigned rounds = std::stoul(argv[6]);
        if (w < 64 || h < 64 || w % 8 || h % 8 || order > 3 || rounds < 1 || rounds > 20)
            throw std::runtime_error("invalid geometry/rounds");
        const size_t n = size_t(w) * h, bytes = n * 2, outputBytes = bytes / 4;
        std::array<std::vector<uint16_t>, 3> frames;
        std::ifstream file(argv[1], std::ios::binary | std::ios::ate);
        if (!file || file.tellg() != std::streamoff(bytes)) throw std::runtime_error("input size mismatch");
        frames[0].resize(n);
        file.seekg(0);
        file.read(reinterpret_cast<char*>(frames[0].data()), bytes);
        if (!file) throw std::runtime_error("input read failed");
        // Distinct synthetic burst frames catch stale descriptors and early slot reuse.
        for (auto v : frames[0])
            if (v > 1023) throw std::runtime_error("10-bit input required");
        for (unsigned f = 1; f < 3; ++f) {
            frames[f] = frames[0];
            for (auto& v : frames[f]) v = uint16_t(std::min(1023u, unsigned(v) + f * 7));
        }
        auto c = vktest::ctx();
        if (bytes > c.prop.limits.maxStorageBufferRange || w / 2 > c.prop.limits.maxImageDimension2D ||
            h / 2 > c.prop.limits.maxImageDimension2D)
            throw std::runtime_error("device buffer/image limit exceeded");
        std::printf("GPU=%s fit=%u frames=3 source_bytes=%zu\n", c.prop.deviceName, fit, bytes);
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
        ai.commandBufferCount = 3;
        std::array<VkCommandBuffer, 3> cmd{};
        vktest::ck(vkAllocateCommandBuffers(c.dev, &ai, cmd.data()), "commands");
        std::array<VkFence, 2> fences{};
        std::array<bool, 2> busy{};
        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        for (auto& f : fences) vktest::ck(vkCreateFence(c.dev, &fi, nullptr, &f), "fence");
        const auto host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        std::array<vktest::Buf, 2> src{};
        std::array<void*, 2> mapped{};
        std::array<vktest::Img, 3> dst{};
        for (unsigned i = 0; i < 2; ++i) {
            src[i] = vktest::mkBuf(c.pd, c.dev, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host);
            vktest::ck(vkMapMemory(c.dev, src[i].m, 0, bytes, 0, &mapped[i]), "map");
        }
        for (auto& d : dst)
            d = vktest::mkImg(c.pd, c.dev, w / 2, h / 2, VK_FORMAT_R16_UINT,
                              VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        auto readback = vktest::mkBuf(c.pd, c.dev, outputBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, host);
        std::array<std::vector<uint16_t>, 3> reference;
        auto remosaic = Remosaic::create({c.pd, c.dev, c.qf});
        auto wait = [&](unsigned s) {
            if (busy[s]) {
                vktest::ck(vkWaitForFences(c.dev, 1, &fences[s], VK_TRUE, UINT64_MAX), "wait");
                busy[s] = false;
            }
        };
        auto begin = [&](unsigned s) {
            vktest::ck(vkResetCommandBuffer(cmd[s], 0), "reset");
            VkCommandBufferBeginInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            vktest::ck(vkBeginCommandBuffer(cmd[s], &bi), "begin");
        };
        auto submit = [&](unsigned s, unsigned command) {
            vktest::ck(vkEndCommandBuffer(cmd[command]), "end");
            vktest::ck(vkResetFences(c.dev, 1, &fences[s]), "reset fence");
            VkSubmitInfo si{};
            si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cmd[command];
            vktest::ck(vkQueueSubmit(c.q, 1, &si, fences[s]), "submit");
            busy[s] = true;
        };
        // One warm-up pair, then alternate measurement order to reduce drift bias.
        for (unsigned round = 0; round <= rounds; ++round)
            for (unsigned mode = 0; mode < 2; ++mode) {
                const bool overlap = (mode ^ (round & 1u)) != 0;
                double copies = 0;
                std::array<double, 3> submitted{}, copied{};
                const auto start = Clock::now();
                for (unsigned frame = 0; frame < 3; ++frame) {
                    unsigned slot = frame % 2;
                    wait(slot);  // protects mapped bytes, descriptor and command reuse
                    auto t = Clock::now();
                    std::memcpy(mapped[slot], frames[frame].data(), bytes);
                    copies += ms(t);
                    copied[frame] = ms(start);
                    begin(slot);
                    VkImageMemoryBarrier b{};
                    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                    b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    b.image = dst[frame].i;
                    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                    vkCmdPipelineBarrier(cmd[slot], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
                    Remosaic::Frame f;
                    f.source = src[slot].b;
                    f.destination = dst[frame].v;
                    f.width = w;
                    f.height = h;
                    f.rowStrideBytes = w * 2;
                    f.block = 4;
                    f.order = order;
                    f.quality = Remosaic::Quality::Detail;
                    f.hexadecaToQuad = true;
                    f.phaseFit = fit;
                    // Shared remosaic scratch is safe: compute submissions use one ordered queue.
                    remosaic->record(cmd[slot], slot, f);
                    submit(slot, slot);
                    submitted[frame] = ms(start);
                    if (!overlap) wait(slot);
                }
                wait(0);
                wait(1);
                const double elapsed = ms(start);
                // All results are read only after the timed burst, compared byte-for-byte
                // with the first sequential run. Retain each frame in a separate image.
                for (unsigned frame = 0; frame < 3; ++frame) {
                    begin(2);
                    VkImageMemoryBarrier b{};
                    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                    b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    b.image = dst[frame].i;
                    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                    vkCmdPipelineBarrier(cmd[2], VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         0, 0, nullptr, 0, nullptr, 1, &b);
                    VkBufferImageCopy region{};
                    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    region.imageExtent = {w / 2, h / 2, 1};
                    vkCmdCopyImageToBuffer(cmd[2], dst[frame].i, VK_IMAGE_LAYOUT_GENERAL, readback.b, 1, &region);
                    submit(0, 2);
                    wait(0);
                    void* data = nullptr;
                    vktest::ck(vkMapMemory(c.dev, readback.m, 0, outputBytes, 0, &data), "map readback");
                    if (reference[frame].empty()) {
                        auto p = static_cast<uint16_t*>(data);
                        reference[frame].assign(p, p + outputBytes / 2);
                    } else if (std::memcmp(data, reference[frame].data(), outputBytes) != 0)
                        throw std::runtime_error("burst result differs from sequential reference");
                    vkUnmapMemory(c.dev, readback.m);
                }
                std::printf(
                    "round=%u mode=%s burst_ms=%.3f copy_ms=%.3f copy_done=%.3f,%.3f,%.3f submit=%.3f,%.3f,%.3f "
                    "parity=EXACT\n",
                    round, overlap ? "overlap" : "sequential", elapsed, copies, copied[0], copied[1], copied[2],
                    submitted[0], submitted[1], submitted[2]);
                std::fflush(stdout);
            }
        remosaic.reset();
        vktest::delBuf(c.dev, readback);
        for (auto& d : dst) vktest::delImg(c.dev, d);
        for (unsigned i = 0; i < 2; ++i) {
            vkUnmapMemory(c.dev, src[i].m);
            vktest::delBuf(c.dev, src[i]);
            vkDestroyFence(c.dev, fences[i], nullptr);
        }
        vkDestroyCommandPool(c.dev, pool, nullptr);
        vktest::delCtx(c);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
