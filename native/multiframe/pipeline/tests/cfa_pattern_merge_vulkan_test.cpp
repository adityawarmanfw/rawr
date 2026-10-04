// GPU gate for non-RGGB merges: a GRBG mosaic of a scene equals the RGGB
// mosaic of the same scene shifted left by one column. Merging both bursts
// must therefore give the same RGB, offset by that column. A channel mix-up
// anywhere in the stack (accumulate phase table, robustness guide, noise
// site order) shows up as a gross colour error. Requires a Vulkan device
// (MoltenVK on macOS).
#include <rawr/raw_gpu_pipeline/AndroidBurstCoordinator.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "vk_test_common.hpp"

namespace {

using rawr::raw_merge_wronski_gpu::CfaPattern;

constexpr std::uint32_t kW = 768, kH = 512, kFrames = 4;
constexpr float kBlack = 64.0f, kWhite = 1023.0f;

// Smooth, strongly coloured scene so channel swaps are unmistakable.
float scene(int c, int x, int y) {
    const float fx = float(x) / kW, fy = float(y) / kH;
    const float base[3] = {0.55f, 0.30f, 0.12f};
    return base[c] + 0.08f * std::sin(6.0f * fx + 2.0f * c) * std::cos(4.0f * fy);
}

float noise(std::uint32_t frame, int x, int y) {
    std::uint32_t h = frame * 0x9E3779B9u ^ std::uint32_t(x) * 0x85EBCA6Bu ^ std::uint32_t(y) * 0xC2B2AE35u;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    return (float(h & 0xFFFFu) / 65535.0f - 0.5f) * 0.01f;
}

// RGGB-layout mosaic: channel at (x, y).
int rggbChannel(int x, int y) { return (x & 1) == 0 && (y & 1) == 0 ? 0 : ((x & 1) == 1 && (y & 1) == 1 ? 2 : 1); }

std::vector<std::uint16_t> rggbFrame(std::uint32_t frame) {
    std::vector<std::uint16_t> out(std::size_t(kW) * kH);
    for (int y = 0; y < int(kH); ++y)
        for (int x = 0; x < int(kW); ++x) {
            const float v = scene(rggbChannel(x, y), x, y) + noise(frame, x, y);
            out[std::size_t(y) * kW + x] = std::uint16_t(std::lround(kBlack + v * (kWhite - kBlack)));
        }
    return out;
}

// GRBG(x) == RGGB(x + 1); the last column reuses the same-colour column two
// to the left.
std::vector<std::uint16_t> shiftToGrbg(const std::vector<std::uint16_t>& rggb) {
    std::vector<std::uint16_t> out(rggb.size());
    for (std::uint32_t y = 0; y < kH; ++y)
        for (std::uint32_t x = 0; x < kW; ++x) {
            const std::uint32_t src = x + 1 < kW ? x + 1 : kW - 2;
            out[std::size_t(y) * kW + x] = rggb[std::size_t(y) * kW + src];
        }
    return out;
}

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
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &b);
}

// Returns RGBA float of the merged output, kW x kH.
std::vector<float> merge(Gpu& gpu, CfaPattern cfa, const std::vector<std::vector<std::uint16_t>>& frames) {
    auto& c = gpu.c;
    const VkDeviceSize rawBytes = VkDeviceSize(kW) * kH * 2u;
    std::vector<vktest::Img> images;
    std::vector<rawr::raw_gpu_pipeline::BurstFrame> burst;
    vktest::Buf staging = vktest::mkBuf(c.pd, c.dev, rawBytes * frames.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    void* mapped = nullptr;
    vktest::ck(vkMapMemory(c.dev, staging.m, 0, VK_WHOLE_SIZE, 0, &mapped), "map");
    for (std::size_t i = 0; i < frames.size(); ++i)
        std::memcpy(static_cast<char*>(mapped) + i * rawBytes, frames[i].data(), rawBytes);
    vkUnmapMemory(c.dev, staging.m);
    for (std::size_t i = 0; i < frames.size(); ++i)
        images.push_back(vktest::mkImg(c.pd, c.dev, kW, kH, VK_FORMAT_R16_UINT,
                                       VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT));
    gpu.oneShot([&](VkCommandBuffer cmd) {
        for (std::size_t i = 0; i < images.size(); ++i) {
            barrier(cmd, images[i].i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy r{};
            r.bufferOffset = i * rawBytes;
            r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            r.imageExtent = {kW, kH, 1};
            vkCmdCopyBufferToImage(cmd, staging.b, images[i].i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
            barrier(cmd, images[i].i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        }
    });
    vktest::delBuf(c.dev, staging);
    for (std::size_t i = 0; i < images.size(); ++i) {
        rawr::raw_gpu_pipeline::BurstFrame f{};
        f.raw.ref = {i + 1, (i + 1) * 33'000'000ull, kW, kH};
        f.raw.image = images[i].i;
        f.raw.view = images[i].v;
        f.raw.format = VK_FORMAT_R16_UINT;
        f.parameters.normalization.blackByPhase = {kBlack, kBlack, kBlack, kBlack};
        f.parameters.normalization.whiteLevel = kWhite;
        burst.push_back(f);
    }

    std::vector<float> rgba(std::size_t(kW) * kH * 4u);
    {
        rawr::raw_gpu_pipeline::AndroidBurstCoordinator coordinator;
        rawr::raw_merge_wronski_gpu::Config mergeConfig{};
        mergeConfig.cfa = cfa;
        mergeConfig.estimateNoiseFromBurst = true;
        coordinator.initialize(
            c.pd, c.dev, c.qf,
            [&](const VkSubmitInfo& si, VkFence fence) { vktest::ck(vkQueueSubmit(c.q, 1, &si, fence), "submit"); },
            kW, kH, 1.0f, {}, {}, mergeConfig);
        const auto result = coordinator.run(burst, 1u);
        if (result.outputExtent.width != kW || result.outputExtent.height != kH)
            throw std::runtime_error("unexpected output extent");
        const VkDeviceSize outBytes = VkDeviceSize(kW) * kH * 8u;
        vktest::Buf readback = vktest::mkBuf(c.pd, c.dev, outBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        gpu.oneShot([&](VkCommandBuffer cmd) {
            barrier(cmd, result.output, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
            VkBufferImageCopy r{};
            r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            r.imageExtent = {kW, kH, 1};
            vkCmdCopyImageToBuffer(cmd, result.output, VK_IMAGE_LAYOUT_GENERAL, readback.b, 1, &r);
        });
        vktest::ck(vkMapMemory(c.dev, readback.m, 0, VK_WHOLE_SIZE, 0, &mapped), "map readback");
        const auto* half = static_cast<const std::uint16_t*>(mapped);
        for (std::size_t i = 0; i < rgba.size(); ++i) rgba[i] = vktest::halfToFloat(half[i]);
        vkUnmapMemory(c.dev, readback.m);
        vktest::delBuf(c.dev, readback);
    }
    for (auto& image : images) vktest::delImg(c.dev, image);
    return rgba;
}

}  // namespace

int main() {
    try {
        Gpu gpu;
        std::vector<std::vector<std::uint16_t>> rggb, grbg;
        for (std::uint32_t f = 0; f < kFrames; ++f) {
            rggb.push_back(rggbFrame(f));
            grbg.push_back(shiftToGrbg(rggb.back()));
        }
        const auto a = merge(gpu, CfaPattern::RGGB, rggb);
        const auto b = merge(gpu, CfaPattern::GRBG, grbg);

        // Interior only: borders and the replicated last column differ by design.
        constexpr int kMargin = 32;
        double sumDiff[3] = {}, maxDiff[3] = {}, sumTruth[3] = {}, sumA[3] = {};
        std::size_t n = 0;
        for (int y = kMargin; y < int(kH) - kMargin; ++y)
            for (int x = kMargin; x < int(kW) - kMargin; ++x, ++n)
                for (int ch = 0; ch < 3; ++ch) {
                    const float vb = b[(std::size_t(y) * kW + x) * 4u + ch];
                    const float va = a[(std::size_t(y) * kW + x + 1) * 4u + ch];
                    const double d = std::fabs(double(vb) - double(va));
                    sumDiff[ch] += d;
                    maxDiff[ch] = std::max(maxDiff[ch], d);
                    sumA[ch] += va;
                    sumTruth[ch] += scene(ch, x + 1, y);
                }
        bool ok = true;
        for (int ch = 0; ch < 3; ++ch) {
            const double meanDiff = sumDiff[ch] / double(n);
            const double meanA = sumA[ch] / double(n), meanTruth = sumTruth[ch] / double(n);
            std::printf("channel %d: mean|GRBG-RGGB|=%.5f max=%.5f meanRGGB=%.4f truth=%.4f\n", ch, meanDiff,
                        maxDiff[ch], meanA, meanTruth);
            // The RGGB merge must reproduce the scene (sanity of the harness)...
            ok = ok && std::fabs(meanA - meanTruth) < 0.01;
            // ...and the GRBG merge must agree with it. A swapped channel is
            // off by >= 0.1; residual differences come from the half-quad shift
            // of the alignment/robustness grids.
            ok = ok && meanDiff < 0.004 && maxDiff[ch] < 0.05;
        }
        if (!ok) {
            std::printf("CFA_PATTERN_MERGE_FAIL\n");
            return 1;
        }
        std::printf("PASS GRBG merge matches RGGB merge shifted by one column\n");
        return 0;
    } catch (const std::exception& e) {
        std::printf("CFA_PATTERN_MERGE_FAIL %s\n", e.what());
        return 1;
    }
}
