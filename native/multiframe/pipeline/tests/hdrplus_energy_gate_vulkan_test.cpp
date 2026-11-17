// The energy test of the displacement statistic (hdrq_mismatch): a noise-only pair must read below the gate, and a displaced fine
// periodic pattern (ripples), whose block mean difference nearly cancels, must be caught by the energy test but not by the mean test.
#include <rawr/raw_gpu_pipeline/VulkanExecutor.h>

#include <array>
#include <cmath>
#include <cstdint>
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

// IEEE half from float (round toward zero; the test only needs finite, repeatable inputs).
uint16_t toHalf(float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    const uint32_t sign = (b >> 16) & 0x8000u;
    const int exp = int((b >> 23) & 0xffu) - 127 + 15;
    if (exp <= 0) return uint16_t(sign);
    if (exp >= 31) return uint16_t(sign | 0x7bffu);
    return uint16_t(sign | (uint32_t(exp) << 10) | ((b >> 13) & 0x3ffu));
}


struct MismatchPc {  // as hdrq_mismatch.comp declares it
    int32_t tilesX, tilesY, width, height;
    float noiseSlope, noiseOffset, black, frameInv;
    float energyGain, energyDead, energyScale;
};
constexpr float kGateLow = 1.6f, kGateHigh = 2.6f;  // HdrPlusRecorder.cpp

// Deterministic approximately Gaussian noise of variance 1.
struct Noise {
    uint32_t s = 12345u;
    float next() {
        float v = -6.0f;
        for (int i = 0; i < 12; ++i) {
            s = s * 1664525u + 1013904223u;
            v += float(s >> 8) / 16777216.0f;
        }
        return v;
    }
};

std::vector<float> run(Gpu& gpu, const std::vector<uint16_t>& ref, const std::vector<uint16_t>& aligned, uint32_t w, uint32_t h, const MismatchPc& pc) {
    using namespace rawr::raw_gpu_pipeline;
    auto& c = gpu.c;
    const auto usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const uint32_t tx = w / 8, ty = h / 8;
    auto imgRef = vktest::mkImg(c.pd, c.dev, w, h, VK_FORMAT_R16G16B16A16_SFLOAT, usage);
    auto imgAl = vktest::mkImg(c.pd, c.dev, w, h, VK_FORMAT_R16G16B16A16_SFLOAT, usage);
    auto imgRms = vktest::mkImg(c.pd, c.dev, tx, ty, VK_FORMAT_R32G32B32A32_SFLOAT, usage);
    auto imgMis = vktest::mkImg(c.pd, c.dev, tx, ty, VK_FORMAT_R32_SFLOAT, usage);
    auto imgZ = vktest::mkImg(c.pd, c.dev, tx, ty, VK_FORMAT_R32_SFLOAT, usage);
    const VkDeviceSize frameBytes = VkDeviceSize(w) * h * 8, rmsBytes = VkDeviceSize(tx) * ty * 16, outBytes = VkDeviceSize(tx) * ty * 4;
    const auto host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    auto up = vktest::mkBuf(c.pd, c.dev, 2 * frameBytes + rmsBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, host);
    auto down = vktest::mkBuf(c.pd, c.dev, outBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, host);
    void* mapped = nullptr;
    vktest::ck(vkMapMemory(c.dev, up.m, 0, VK_WHOLE_SIZE, 0, &mapped), "map up");
    std::memcpy(mapped, ref.data(), frameBytes);
    std::memcpy(static_cast<char*>(mapped) + frameBytes, aligned.data(), frameBytes);
    auto* rms = reinterpret_cast<float*>(static_cast<char*>(mapped) + 2 * frameBytes);
    for (uint32_t i = 0; i < tx * ty * 4; ++i) rms[i] = 1.0f;
    vkUnmapMemory(c.dev, up.m);
    VulkanExecutor exec;
    exec.initialize(c.dev, false);
    exec.beginBatch();
    gpu.oneShot([&](VkCommandBuffer cmd) {
        for (auto* im : {&imgRef, &imgAl, &imgRms, &imgMis, &imgZ}) barrier(cmd, im->i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {w, h, 1};
        vkCmdCopyBufferToImage(cmd, up.b, imgRef.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        copy.bufferOffset = frameBytes;
        vkCmdCopyBufferToImage(cmd, up.b, imgAl.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        copy.bufferOffset = 2 * frameBytes;
        copy.imageExtent = {tx, ty, 1};
        vkCmdCopyBufferToImage(cmd, up.b, imgRms.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        for (auto* im : {&imgRef, &imgAl, &imgRms, &imgMis, &imgZ}) barrier(cmd, im->i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        const auto ib = [](uint32_t n, const vktest::Img& img) { return ImageBinding{n, img.v, VK_IMAGE_LAYOUT_GENERAL}; };
        exec.record(cmd, ShaderId::HdrqMismatch, {{ib(0, imgRef), ib(1, imgAl), ib(2, imgRms), ib(3, imgMis), ib(4, imgZ)}, {}}, &pc, sizeof(pc), tx, ty);
        barrier(cmd, imgZ.i, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        copy.bufferOffset = 0;
        vkCmdCopyImageToBuffer(cmd, imgZ.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, down.b, 1, &copy);
    });
    std::vector<float> z(tx * ty);
    vktest::ck(vkMapMemory(c.dev, down.m, 0, VK_WHOLE_SIZE, 0, &mapped), "map down");
    std::memcpy(z.data(), mapped, outBytes);
    vkUnmapMemory(c.dev, down.m);
    exec.reset();
    vktest::delBuf(c.dev, up);
    vktest::delBuf(c.dev, down);
    for (auto* im : {&imgRef, &imgAl, &imgRms, &imgMis, &imgZ}) vktest::delImg(c.dev, *im);
    return z;
}

float meanRegion(const std::vector<float>& z, uint32_t tx, uint32_t x0, uint32_t x1, uint32_t y0, uint32_t y1) {
    double sum = 0;
    int n = 0;
    for (uint32_t y = y0; y < y1; ++y)
        for (uint32_t x = x0; x < x1; ++x) sum += z[y * tx + x], ++n;
    return float(sum / n);
}
}  // namespace

int main() {
    try {
        Gpu gpu;
        constexpr uint32_t w = 128, h = 64, tx = w / 8, ty = h / 8;
        constexpr float level = 200.0f, sigma = 3.0f, ripple = 8.0f;
        // Left half: the same flat level with independent noise in both frames. Right half: a period-4 ripple, shifted by one pixel
        // (a quarter period) between the frames, plus the same noise. Four channels share the geometry.
        std::vector<uint16_t> ref(w * h * 4), al(w * h * 4);
        Noise nz;
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
                for (uint32_t ch = 0; ch < 4; ++ch) {
                    const bool rip = x >= w / 2;
                    const float r = rip ? ripple * std::sin(6.2831853f * float(x) / 4.0f) : 0.0f;
                    const float a = rip ? ripple * std::sin(6.2831853f * float(x + 1) / 4.0f) : 0.0f;
                    ref[(y * w + x) * 4 + ch] = toHalf(level + r + sigma * nz.next());
                    al[(y * w + x) * 4 + ch] = toHalf(level + a + sigma * nz.next());
                }
        MismatchPc pc{int32_t(tx), int32_t(ty), int32_t(w), int32_t(h), 0.0f, sigma * sigma, 0.0f, 1.0f, 0.0f, 1.0f, 0.5f};
        const auto meanOnly = run(gpu, ref, al, w, h, pc);
        pc.energyGain = 1.0f;
        const auto withEnergy = run(gpu, ref, al, w, h, pc);
        // Interior tiles only (the window of an edge tile is clipped).
        const float noiseMean = meanRegion(meanOnly, tx, 1, 6, 2, 6), noiseEnergy = meanRegion(withEnergy, tx, 1, 6, 2, 6);
        const float rippleMean = meanRegion(meanOnly, tx, 10, 15, 2, 6), rippleEnergy = meanRegion(withEnergy, tx, 10, 15, 2, 6);
        std::printf("noise only: mean test %.2f, with energy test %.2f | displaced ripples: mean test %.2f, with energy test %.2f (gate %.1f..%.1f)\n",
                    noiseMean, noiseEnergy, rippleMean, rippleEnergy, kGateLow, kGateHigh);
        if (!(noiseMean < kGateLow && noiseEnergy < kGateLow)) throw std::runtime_error("noise-only tiles read above the gate");
        if (std::fabs(noiseEnergy - noiseMean) > 0.1f) throw std::runtime_error("the energy test responds to noise");
        if (!(rippleEnergy >= kGateHigh)) throw std::runtime_error("the energy test does not gate displaced ripples");
        if (!(rippleEnergy > rippleMean + 1.0f)) throw std::runtime_error("the energy test adds nothing over the mean test on ripples");
        std::puts("hdrplus_energy_gate_vulkan_test passed");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
