// Host Vulkan validation for local_tone: the exposure-fusion passes against a
// CPU mirror of the same algorithm on a synthetic high-contrast scene.
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "local_tone/ExposureFusion.h"
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

struct V3 {
    float r, g, b;
};

V3 aces(V3 c) {
    c = {c.r / 0.6f, c.g / 0.6f, c.b / 0.6f};
    const V3 v{0.59719f * c.r + 0.35458f * c.g + 0.04823f * c.b, 0.07600f * c.r + 0.90834f * c.g + 0.01566f * c.b,
               0.02840f * c.r + 0.13383f * c.g + 0.83777f * c.b};
    const auto fit = [](float x) {
        return (x * (x + 0.0245786f) - 0.000090537f) / (x * (0.983729f * x + 0.4329510f) + 0.238081f);
    };
    const V3 f{fit(v.r), fit(v.g), fit(v.b)};
    const auto sat = [](float x) { return std::clamp(x, 0.0f, 1.0f); };
    return {sat(1.60475f * f.r - 0.53108f * f.g - 0.07367f * f.b), sat(-0.10208f * f.r + 1.10813f * f.g - 0.00605f * f.b),
            sat(-0.00327f * f.r - 0.07276f * f.g + 1.07602f * f.b)};
}
float lumaOf(V3 c) { return 0.1f * c.r + 0.7f * c.g + 0.2f * c.b; }

// The test scene is already Rec.709-like: camera->AP1 is the inverse of the
// shader's AP1->709 so the composed matrix is identity.
constexpr float kAp1To709[3][3] = {{1.70505f, -0.62179f, -0.08326f},
                                   {-0.13026f, 1.14080f, -0.01055f},
                                   {-0.02400f, -0.12897f, 1.15297f}};
std::array<float, 9> inverseAp1To709ColumnMajor() {
    const auto& m = kAp1To709;
    const float det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                      m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                      m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    float inv[3][3];
    inv[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / det;
    inv[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det;
    inv[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det;
    inv[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / det;
    inv[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det;
    inv[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det;
    inv[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / det;
    inv[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det;
    inv[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det;
    std::array<float, 9> out{};
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r) out[std::size_t(c * 3 + r)] = inv[r][c];
    return out;
}

struct Img {
    int w = 0, h = 0, ch = 0;
    std::vector<float> v;
    float& at(int x, int y, int c) { return v[(std::size_t(y) * w + x) * ch + c]; }
    float get(int x, int y, int c) const {
        return v[(std::size_t(std::clamp(y, 0, h - 1)) * w + std::clamp(x, 0, w - 1)) * ch + c];
    }
    // Bilinear at uv with clamp-to-edge, like the shaders' sampler.
    float sample(float u, float vv, int c) const {
        const float x = u * w - 0.5f, y = vv * h - 0.5f;
        const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
        const float fx = x - x0, fy = y - y0;
        return (get(x0, y0, c) * (1 - fx) + get(x0 + 1, y0, c) * fx) * (1 - fy) +
               (get(x0, y0 + 1, c) * (1 - fx) + get(x0 + 1, y0 + 1, c) * fx) * fy;
    }
};
Img make(int w, int h, int ch) { return {w, h, ch, std::vector<float>(std::size_t(w) * h * ch)}; }

// CPU mirror of fus_*.comp (identity colour, renderGain x exposure = gain).
std::vector<float> reference(const std::vector<float>& rgba, int w, int h, const FusionParams& p, float gain) {
    const float s = std::exp2(p.shadowsEv), hl = std::exp2(-p.highlightsEv), sig2 = p.sigma * p.sigma;
    const auto lightness = [&](V3 c) {
        const V3 ch{c.r * hl, c.g * hl, c.b * hl}, cs{c.r * s, c.g * s, c.b * s};
        return V3{std::sqrt(lumaOf(aces(ch))), std::sqrt(lumaOf(aces(c))), std::sqrt(lumaOf(aces(cs)))};
    };
    const auto pixel = [&](int x, int y) {
        const float* px = &rgba[(std::size_t(y) * w + x) * 4];
        return V3{std::max(px[0], 0.f) * gain, std::max(px[1], 0.f) * gain, std::max(px[2], 0.f) * gain};
    };
    const int block = 1 << kFusionBaseMip;
    std::vector<Img> L, W;
    L.push_back(make((w + block - 1) / block, (h + block - 1) / block, 3));
    W.push_back(make(L[0].w, L[0].h, 3));
    for (int qy = 0; qy < L[0].h; ++qy)
        for (int qx = 0; qx < L[0].w; ++qx) {
            double l[3]{}, wt[3]{};
            int n = 0;
            for (int y = qy * block; y < std::min(h, qy * block + block); ++y)
                for (int x = qx * block; x < std::min(w, qx * block + block); ++x, ++n) {
                    const V3 v = lightness(pixel(x, y));
                    const float lv[3] = {v.r, v.g, v.b};
                    float e[3], sum = 0;
                    for (int k = 0; k < 3; ++k) {
                        e[k] = std::exp(-0.5f * (lv[k] - 0.5f) * (lv[k] - 0.5f) * sig2);
                        sum += e[k];
                    }
                    for (int k = 0; k < 3; ++k) {
                        l[k] += lv[k];
                        wt[k] += e[k] / (sum + 0.00001f);
                    }
                }
            for (int k = 0; k < 3; ++k) {
                L[0].at(qx, qy, k) = float(l[k] / n);
                W[0].at(qx, qy, k) = float(wt[k] / n);
            }
        }
    const int n = int(p.coarsestMip - kFusionBaseMip);
    for (int i = 0; i < n; ++i) {
        const auto& src = L[std::size_t(i)];
        Img l = make(std::max(1, src.w / 2), std::max(1, src.h / 2), 3), wt = make(l.w, l.h, 3);
        for (int y = 0; y < l.h; ++y)
            for (int x = 0; x < l.w; ++x)
                for (int k = 0; k < 3; ++k) {
                    float a = 0, b = 0;
                    for (int dy = 0; dy < 2; ++dy)
                        for (int dx = 0; dx < 2; ++dx) {
                            const int sx = std::min(2 * x + dx, src.w - 1), sy = std::min(2 * y + dy, src.h - 1);
                            a += src.get(sx, sy, k);
                            b += W[std::size_t(i)].get(sx, sy, k);
                        }
                    l.at(x, y, k) = a * 0.25f;
                    wt.at(x, y, k) = b * 0.25f;
                }
        L.push_back(l);
        W.push_back(wt);
    }
    std::vector<Img> A(std::size_t(n) + 1);
    A[std::size_t(n)] = make(L[std::size_t(n)].w, L[std::size_t(n)].h, 1);
    for (int y = 0; y < A[std::size_t(n)].h; ++y)
        for (int x = 0; x < A[std::size_t(n)].w; ++x) {
            float wt[3], sum = 0, v = 0;
            for (int k = 0; k < 3; ++k) sum += wt[k] = W[std::size_t(n)].get(x, y, k);
            for (int k = 0; k < 3; ++k) v += L[std::size_t(n)].get(x, y, k) * wt[k] / (sum + 0.0001f);
            A[std::size_t(n)].at(x, y, 0) = v;
        }
    for (int i = n; i > 0; --i) {
        const auto& fine = L[std::size_t(i - 1)];
        Img a = make(fine.w, fine.h, 1);
        for (int y = 0; y < a.h; ++y)
            for (int x = 0; x < a.w; ++x) {
                const float u = (x + 0.5f) / a.w, v = (y + 0.5f) / a.h;
                float lap[3], wt[3], sum = 0, add = 0;
                for (int k = 0; k < 3; ++k) {
                    lap[k] = fine.get(x, y, k) - L[std::size_t(i)].sample(u, v, k);
                    wt[k] = W[std::size_t(i - 1)].get(x, y, k) * (p.boostLocalContrast ? std::abs(lap[k]) + 0.00001f : 1.f);
                    sum += wt[k];
                }
                for (int k = 0; k < 3; ++k) add += lap[k] * wt[k] / (sum + 0.00001f);
                a.at(x, y, 0) = A[std::size_t(i)].sample(u, v, 0) + add;
            }
        A[std::size_t(i - 1)] = a;
    }
    std::vector<float> out = rgba;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float u = (x + 0.5f) / w, v = (y + 0.5f) / h;
            float mX = 0, mY = 0, mX2 = 0, mXY = 0, ws = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const float tu = u + float(dx) / L[0].w, tv = v + float(dy) / L[0].h;
                    const float gx = L[0].sample(tu, tv, 1), gy = A[0].sample(tu, tv, 0);
                    const float g = std::exp(-0.5f * float(dx * dx + dy * dy) / (0.7f * 0.7f));
                    mX += gx * g;
                    mY += gy * g;
                    mX2 += gx * gx * g;
                    mXY += gx * gy * g;
                    ws += g;
                }
            mX /= ws;
            mY /= ws;
            mX2 /= ws;
            mXY /= ws;
            const float a = (mXY - mX * mY) / (std::max(mX2 - mX * mX, 0.f) + 0.00001f), b = mY - a * mX;
            const V3 t = aces(pixel(x, y));
            const float lum = 0.1f * std::sqrt(t.r) + 0.7f * std::sqrt(t.g) + 0.2f * std::sqrt(t.b) + 0.00001f;
            float mult = std::max(a * lum + b, 0.f) / lum;
            if (lum <= 0.007f) mult = 1.f + (mult - 1.f) * (lum / 0.007f) * (lum / 0.007f);
            for (int c = 0; c < 3; ++c) out[(std::size_t(y) * w + x) * 4 + c] *= mult;
        }
    return out;
}
}  // namespace

int main() {
    assert(noiseLimitedShadowsEv(2.5f, 400.0f) == 2.5f);
    assert(std::abs(noiseLimitedShadowsEv(2.5f, 6400.0f) - 1.0f) < 1e-5f);
    assert(noiseLimitedShadowsEv(2.5f, 1e6f) == kMinShadowsEv);
    assert(noiseLimitedShadowsEv(0.0f, 6400.0f) == 0.0f);

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

    FusionParams params{};
    params.shadowsEv = kDefaultShadowsEv;
    params.coarsestMip = 6;
    const float gain = 4.0f;
    {
        ExposureFusionProcessor processor({c.pd, c.dev, c.qf}, 1);
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
        FusionRecordInfo info{};
        info.commandBuffer = cmd;
        info.width = w;
        info.height = h;
        info.measure = {image.i, image.v};
        info.targets[0] = {image.i, image.v};
        info.cameraToWorking = inverseAp1To709ColumnMajor();
        info.renderGain = gain;
        info.params = params;
        FusionRecordInfo bad = info;
        bad.params.coarsestMip = kFusionBaseMip;  // nothing to blend
        assert(!processor.record(bad));
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
    std::printf("exposure_fusion: max |dEV| vs CPU %.4f mean %.5f; dark lift %.2f EV, bright lift %.2f EV\n", maxErr,
                sumErr / (double(scene.size()) / 4 * 3), darkLift, brightLift);
    assert(maxErr < 0.05);               // fp16 pyramid + hardware bilinear
    assert(darkLift > 1.0);              // the shadowed half is lifted
    assert(brightLift < darkLift - 1.0); // far more than the sky
    vkDestroyCommandPool(c.dev, pool, nullptr);
    vktest::delImg(c.dev, image);
    vktest::delBuf(c.dev, staging);
    vktest::delCtx(c);
    std::puts("local_tone_vulkan_test OK");
    return 0;
}
