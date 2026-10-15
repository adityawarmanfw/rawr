// Host Vulkan validation for local_tone: the GPU passes against a CPU mirror of
// the same algorithm on a synthetic high-contrast scene.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "local_tone/LocalTone.h"
#include "vk_test_common.hpp"

namespace {
using namespace rawr::local_tone;

std::uint16_t toHalf(float f) {
    std::uint32_t x;
    std::memcpy(&x, &f, 4);
    const std::uint32_t sign = (x >> 16) & 0x8000u;
    int e = int((x >> 23) & 0xffu) - 127 + 15;
    std::uint32_t m = x & 0x7fffffu;
    if (e <= 0) return std::uint16_t(sign);
    if (e >= 31) return std::uint16_t(sign | 0x7c00u);
    return std::uint16_t(sign | (std::uint32_t(e) << 10) | ((m + 0x1000u) >> 13));
}

constexpr float kAp1Y[3] = {0.2722287168f, 0.6740817658f, 0.0536895174f};

float logLuma(const float* rgb, float logGain) {
    const float y = kAp1Y[0] * std::max(rgb[0], 0.f) + kAp1Y[1] * std::max(rgb[1], 0.f) + kAp1Y[2] * std::max(rgb[2], 0.f);
    return std::max(std::log2(std::max(y, 1e-12f)) + logGain, -16.0f);
}

// CPU mirror of the GPU passes (identity camera matrix).
std::vector<float> reference(const std::vector<float>& rgba, int w, int h, const Params& p, float gain) {
    const float logGain = std::log2(gain);
    const int block = int(blockSize(w, h));
    const int lw = (w + block - 1) / block, lh = (h + block - 1) / block;
    std::vector<float> i(lw * lh), ii(lw * lh);
    for (int qy = 0; qy < lh; ++qy)
        for (int qx = 0; qx < lw; ++qx) {
            double s = 0;
            int n = 0;
            for (int y = qy * block; y < std::min(h, qy * block + block); ++y)
                for (int x = qx * block; x < std::min(w, qx * block + block); ++x, ++n)
                    s += logLuma(&rgba[(y * w + x) * 4], logGain);
            i[qy * lw + qx] = float(s / n);
            ii[qy * lw + qx] = i[qy * lw + qx] * i[qy * lw + qx];
        }
    const auto box = [&](const std::vector<float>& a) {
        std::vector<float> t(a.size()), o(a.size());
        for (int y = 0; y < lh; ++y)
            for (int x = 0; x < lw; ++x) {
                double s = 0;
                for (int k = -kRadius; k <= kRadius; ++k) s += a[y * lw + std::clamp(x + k, 0, lw - 1)];
                t[y * lw + x] = float(s / (2 * kRadius + 1));
            }
        for (int y = 0; y < lh; ++y)
            for (int x = 0; x < lw; ++x) {
                double s = 0;
                for (int k = -kRadius; k <= kRadius; ++k) s += t[std::clamp(y + k, 0, lh - 1) * lw + x];
                o[y * lw + x] = float(s / (2 * kRadius + 1));
            }
        return o;
    };
    const auto mi = box(i), mii = box(ii);
    std::vector<float> a(i.size()), b(i.size());
    for (std::size_t k = 0; k < i.size(); ++k) {
        const float var = std::max(mii[k] - mi[k] * mi[k], 0.f);
        a[k] = var / (var + kEps);
        b[k] = mi[k] - a[k] * mi[k];
    }
    const auto ma = box(a), mb = box(b);
    std::vector<float> lift(i.size()), base(i.size());
    for (std::size_t k = 0; k < i.size(); ++k) {
        base[k] = ma[k] * i[k] + mb[k];
        lift[k] = regionalLiftEv(base[k], p);
    }
    const auto sample = [&](const std::vector<float>& m, float u, float v) {
        const float x = u * lw - 0.5f, y = v * lh - 0.5f;
        const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
        const float fx = x - x0, fy = y - y0;
        const auto at = [&](int xx, int yy) { return m[std::clamp(yy, 0, lh - 1) * lw + std::clamp(xx, 0, lw - 1)]; };
        return (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) + (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
    };
    std::vector<float> out = rgba;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float u = (x + 0.5f) / (block * lw), v = (y + 0.5f) / (block * lh);
            float* px = &out[(y * w + x) * 4];
            const float ly = logLuma(px, logGain);
            const float ev = sample(lift, u, v) + std::clamp((p.detail - 1.0f) * (ly - sample(base, u, v)), -1.f, 1.f) *
                                                      std::clamp((ly + 12.0f) / 3.0f, 0.f, 1.f);
            for (int c = 0; c < 3; ++c) px[c] *= std::exp2(ev);
        }
    return out;
}
}  // namespace

int main() {
    auto c = vktest::ctx();
    const int w = 1024, h = 768;
    // Dark left half (a shadowed wall) and a bright right half (sky), both
    // with fine texture, plus a gradient strip.
    std::vector<float> scene(std::size_t(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float texture = 1.0f + 0.3f * std::sin(x * 0.9f) * std::sin(y * 0.7f);
            float level = x < w / 2 ? 0.004f : 0.45f;
            if (y > h * 3 / 4) level = 0.001f * std::exp2(10.0f * x / w);
            float* px = &scene[(std::size_t(y) * w + x) * 4];
            px[0] = level * texture * 0.9f;
            px[1] = level * texture;
            px[2] = level * texture * 1.1f;
            px[3] = 1.0f;
        }
    std::vector<std::uint16_t> halfs(scene.size());
    std::vector<float> quantized(scene.size());
    for (std::size_t k = 0; k < scene.size(); ++k) {
        halfs[k] = toHalf(scene[k]);
        quantized[k] = vktest::halfToFloat(halfs[k]);
    }
    const VkDeviceSize bytes = halfs.size() * 2;
    auto staging = vktest::mkBuf(c.pd, c.dev, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    void* mapped = nullptr;
    vktest::ck(vkMapMemory(c.dev, staging.m, 0, bytes, 0, &mapped), "map");
    std::memcpy(mapped, halfs.data(), bytes);
    auto image = vktest::mkImg(c.pd, c.dev, w, h, VK_FORMAT_R16G16B16A16_SFLOAT,
                               VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);

    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.queueFamilyIndex = c.qf;
    VkCommandPool pool;
    vktest::ck(vkCreateCommandPool(c.dev, &pi, nullptr, &pool), "pool");
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vktest::ck(vkAllocateCommandBuffers(c.dev, &ai, &cmd), "cmd");

    const Params params{0.5f, 1.1f};
    const float gain = 2.0f;
    {
        rawr::local_tone::LocalToneProcessor processor({c.pd, c.dev, c.qf}, 1);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cmd, &bi);
        VkImageMemoryBarrier toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toDst.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        toDst.srcQueueFamilyIndex = toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.image = image.i;
        toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &toDst);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {std::uint32_t(w), std::uint32_t(h), 1};
        vkCmdCopyBufferToImage(cmd, staging.b, image.i, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        VkImageMemoryBarrier ready = toDst;
        ready.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        ready.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        ready.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &ready);
        RecordInfo info{};
        info.commandBuffer = cmd;
        info.width = w;
        info.height = h;
        info.measure = {image.i, image.v};
        info.targets[0] = {image.i, image.v};
        info.renderGain = gain;
        info.params = params;
        // Inactive params record nothing.
        RecordInfo off = info;
        off.params = {0.0f, 1.0f};
        assert(!processor.record(off));
        assert(processor.record(info));
        vkCmdCopyImageToBuffer(cmd, image.i, VK_IMAGE_LAYOUT_GENERAL, staging.b, 1, &copy);
        vkEndCommandBuffer(cmd);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        vktest::ck(vkQueueSubmit(c.q, 1, &si, VK_NULL_HANDLE), "submit");
        vktest::ck(vkQueueWaitIdle(c.q), "wait");
    }
    std::vector<std::uint16_t> outHalf(halfs.size());
    std::memcpy(outHalf.data(), mapped, bytes);
    const auto expected = reference(quantized, w, h, params, gain);
    double maxErr = 0, sumErr = 0;
    for (std::size_t k = 0; k < scene.size(); k += 4)
        for (int ch = 0; ch < 3; ++ch) {
            const float gpu = vktest::halfToFloat(outHalf[k + ch]);
            const double err = std::abs(std::log2(std::max(gpu, 1e-9f)) - std::log2(std::max(expected[k + ch], 1e-9f)));
            maxErr = std::max(maxErr, err);
            sumErr += err;
        }
    const auto evAt = [&](const std::vector<float>& img, int x, int y) {
        return std::log2(img[(std::size_t(y) * w + x) * 4 + 1]);
    };
    const auto gpuEvAt = [&](int x, int y) { return std::log2(vktest::halfToFloat(outHalf[(std::size_t(y) * w + x) * 4 + 1])); };
    const double darkLift = gpuEvAt(w / 8, h / 4) - evAt(quantized, w / 8, h / 4);
    const double brightLift = gpuEvAt(w * 7 / 8, h / 4) - evAt(quantized, w * 7 / 8, h / 4);
    std::printf("local_tone: max |dEV| vs CPU %.4f mean %.5f; dark lift %.2f EV, bright lift %.2f EV\n", maxErr,
                sumErr / (scene.size() / 4 * 3), darkLift, brightLift);
    assert(maxErr < 0.05);      // fp16 storage + hardware bilinear
    assert(darkLift > 1.0);     // the shadowed half is lifted
    assert(std::abs(brightLift) < 0.2);  // the sky keeps its level
    vkDestroyCommandPool(c.dev, pool, nullptr);
    vktest::delImg(c.dev, image);
    vktest::delBuf(c.dev, staging);
    vktest::delCtx(c);
    std::puts("local_tone_vulkan_test OK");
    return 0;
}
