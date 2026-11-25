// Rexture grain evening (rx_even.comp), its steps as RextureRecorder::recordGrainPass runs them, on a synthetic 768 x 256
// grey scene with per-channel noise of variance alpha * level * m (m: the output's noise relative to one frame):
//  - x < 256: merged (m 0.06) with a 96 x 96 few-frame patch (m 0.6): the patch's grain (luma and colour) must come down
//    towards the merged level around it;
//  - 256 <= x < 512: few-frame (m 0.6) texture well above the noise (a 6 px sinusoid): its amplitude must stay;
//  - x >= 512: merged (m 0.06), far from both: must come out unchanged (within fp16 rounding).
// Run twice: daylight-neutral raw (white balance 1) and the same grey scene under warm light (raw R and B at 1 / gain of
// green, tungsten gains). After white balance the second must come out the same: the texture stays grey (no tint from
// its raw R - G / B - G detail) and the patch's colour grain is lowered without following its luma grain.
#include <rawr/raw_gpu_pipeline/VulkanExecutor.h>

#include <algorithm>
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

uint16_t toHalf(float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    const uint32_t sign = (b >> 16) & 0x8000u;
    const int exp = int((b >> 23) & 0xffu) - 127 + 15;
    if (exp <= 0) return uint16_t(sign);
    if (exp >= 31) return uint16_t(sign | 0x7bffu);
    uint32_t h = sign | (uint32_t(exp) << 10) | ((b >> 13) & 0x3ffu);
    if (b & 0x1000u) ++h;
    return uint16_t(h);
}
float fromHalf(uint16_t h) {
    const uint32_t sign = uint32_t(h & 0x8000u) << 16;
    const int exp = (h >> 10) & 0x1f;
    const uint32_t man = h & 0x3ffu;
    uint32_t b = sign;
    if (exp == 31) b |= 0x7f800000u | man << 13;
    else if (exp != 0) b |= uint32_t(exp - 15 + 127) << 23 | man << 13;
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}

constexpr uint32_t W = 768, H = 256;
constexpr float kAlpha = 1e-3f, kLevel = 0.2f, kMerged = 0.06f, kFew = 0.6f;
struct Pc {
    int32_t op, level, w, h;
    float alpha, beta;
    int32_t rrow;
    int32_t ox[7];
    float wbR, wbB;
};
static_assert(sizeof(Pc) == 64, "Pc must match rx_even's push block");
bool patch(uint32_t x, uint32_t y) { return x >= 80 && x < 176 && y >= 80 && y < 176; }
bool textured(uint32_t x) { return x >= 256 && x < 512; }
float truthAt(uint32_t x, uint32_t y) {
    return textured(x) ? kLevel + 0.05f * std::sin(6.2831853f * float(x) / 6.0f) * std::sin(6.2831853f * float(y) / 9.0f) : kLevel;
}
float mAt(uint32_t x, uint32_t y) { return (patch(x, y) || textured(x)) ? kFew : kMerged; }

// One case: raw channel k = grey scene / gain k with the raw noise model; checks run on the white-balanced values. Returns
// the regression of the patch's colour grain on its luma grain (see main).
double runCase(Gpu& gpu, float wbR, float wbB) {
        using namespace rawr::raw_gpu_pipeline;
        auto& c = gpu.c;
        const float gk[3] = {wbR, 1.0f, wbB};
        uint64_t rng = 0x2545f4914f6cdd1dull;
        const auto gauss = [&rng]() {
            const auto u = [&rng]() { rng = rng * 6364136223846793005ull + 1442695040888963407ull; return (float((rng >> 40) & 0xffffffull) + 0.5f) / 16777216.0f; };
            const float a = u(), b = u();
            return std::sqrt(-2.0f * std::log(a)) * std::cos(6.2831853f * b);
        };
        const uint32_t mw = W / 4, mh = H / 4, rw = (W + 7) / 8, rh = (H + 7) / 8;
        std::vector<uint16_t> img(size_t(W) * H * 4);
        std::vector<float> m(size_t(mw) * mh);
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                const size_t i = size_t(y) * W + x;
                const float t = truthAt(x, y);
                for (int k = 0; k < 3; ++k) {
                    const float raw = t / gk[k];
                    img[i * 4 + size_t(k)] = toHalf(raw + std::sqrt(kAlpha * raw * mAt(x, y)) * gauss());
                }
                img[i * 4 + 3] = toHalf(1.0f);
            }
        for (uint32_t my = 0; my < mh; ++my)
            for (uint32_t mx = 0; mx < mw; ++mx) m[size_t(my) * mw + mx] = mAt(4 * mx + 2, 4 * my + 2);
        const auto usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        auto outI = vktest::mkImg(c.pd, c.dev, W, H, VK_FORMAT_R16G16B16A16_SFLOAT, usage);
        auto atlas = vktest::mkImg(c.pd, c.dev, W, H, VK_FORMAT_R16G16B16A16_SFLOAT, usage);
        auto gain = vktest::mkImg(c.pd, c.dev, W, H, VK_FORMAT_R16G16B16A16_SFLOAT, usage);
        auto mI = vktest::mkImg(c.pd, c.dev, mw, mh, VK_FORMAT_R32_SFLOAT, usage);
        auto refA = vktest::mkImg(c.pd, c.dev, rw, rh, VK_FORMAT_R16_SFLOAT, usage);
        auto refB = vktest::mkImg(c.pd, c.dev, rw, rh, VK_FORMAT_R16_SFLOAT, usage);
        const VkDeviceSize imgB = VkDeviceSize(W) * H * 8, mB = VkDeviceSize(mw) * mh * 4;
        const auto host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        auto up = vktest::mkBuf(c.pd, c.dev, imgB + mB, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, host);
        auto down = vktest::mkBuf(c.pd, c.dev, imgB, VK_BUFFER_USAGE_TRANSFER_DST_BIT, host);
        void* mapped = nullptr;
        vktest::ck(vkMapMemory(c.dev, up.m, 0, VK_WHOLE_SIZE, 0, &mapped), "map up");
        std::memcpy(mapped, img.data(), imgB);
        std::memcpy(static_cast<char*>(mapped) + imgB, m.data(), mB);
        vkUnmapMemory(c.dev, up.m);
        VulkanExecutor exec;
        exec.initialize(c.dev, false);
        exec.beginBatch();
        const auto all = {&outI, &atlas, &gain, &mI, &refA, &refB};
        const auto size = [](int32_t l) { return std::pair<uint32_t, uint32_t>{(W + (1u << l) - 1) >> l, (H + (1u << l) - 1) >> l}; };
        gpu.oneShot([&](VkCommandBuffer cmd) {
            for (auto* im : all) barrier(cmd, im->i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy cp{};
            cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            cp.imageExtent = {W, H, 1};
            vkCmdCopyBufferToImage(cmd, up.b, outI.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
            cp.bufferOffset = imgB;
            cp.imageExtent = {mw, mh, 1};
            vkCmdCopyBufferToImage(cmd, up.b, mI.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
            for (auto* im : all) barrier(cmd, im->i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
            const auto ib = [](uint32_t b, const vktest::Img& i) { return ImageBinding{b, i.v, VK_IMAGE_LAYOUT_GENERAL}; };
            const DispatchBindings bind{{ib(0, outI), ib(1, atlas), ib(2, gain), ib(3, mI), ib(4, refA), ib(5, refB)}, {}};
            Pc p{0, 0, int32_t(W), int32_t(H), kAlpha, 0.0f, int32_t(size(1).second), {}, wbR, wbB};
            for (int32_t l = 1, x = 0; l <= 6; ++l) { p.ox[l] = x; x += int32_t(size(l).first); }
            const auto run = [&](int32_t op, int32_t level, uint32_t gx, uint32_t gy) {
                p.op = op;
                p.level = level;
                exec.record(cmd, ShaderId::RxEven, bind, &p, sizeof(p), gx, gy);
                for (auto* im : all) barrier(cmd, im->i, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
            };
            const auto g16 = [](uint32_t v) { return (v + 15) / 16; };
            for (int32_t op = 0; op <= 4; ++op) run(op, 0, g16(rw), g16(rh));
            for (int32_t l = 1; l <= 6; ++l) run(5, l, g16(size(l).first), (size(l).second + 7) / 8);
            for (int32_t l = 0; l <= 5; ++l) run(6, l, g16(size(l + 1).first), g16(size(l + 1).second));
            for (int32_t l = 5; l >= 0; --l) run(7, l, g16(size(l).first), g16(size(l).second));
            barrier(cmd, outI.i, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy dp{};
            dp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            dp.imageExtent = {W, H, 1};
            vkCmdCopyImageToBuffer(cmd, outI.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, down.b, 1, &dp);
        });
        std::vector<uint16_t> res(size_t(W) * H * 4);
        vktest::ck(vkMapMemory(c.dev, down.m, 0, VK_WHOLE_SIZE, 0, &mapped), "map down");
        std::memcpy(res.data(), mapped, imgB);
        vkUnmapMemory(c.dev, down.m);
        exec.reset();
        vktest::delBuf(c.dev, up);
        vktest::delBuf(c.dev, down);
        for (auto* im : all) vktest::delImg(c.dev, *im);
        // residuals against the truth after white balance: luma (green) and colour (R - G, B - G) in the patch interior,
        // the merged area around it, the texture's luma amplitude and the colour that follows it
        const auto in = [&](size_t i, int k) { return fromHalf(img[i * 4 + size_t(k)]) * gk[k]; };
        const auto out = [&](size_t i, int k) { return fromHalf(res[i * 4 + size_t(k)]) * gk[k]; };
        double pin[3] = {0, 0, 0}, pout[3] = {0, 0, 0}, follow = 0, bin = 0, nP = 0, nB = 0, proj = 0, projIn = 0, tint = 0, norm = 0;
        float far = 0;
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                const size_t i = size_t(y) * W + x;
                const float t = truthAt(x, y);
                const float iG = in(i, 1), oR = out(i, 0), oG = out(i, 1), oB = out(i, 2);
                if (x >= 96 && x < 160 && y >= 96 && y < 160) {   // patch interior
                    pin[0] += (iG - t) * (iG - t); pout[0] += (oG - t) * (oG - t);
                    pin[1] += (in(i, 0) - iG) * (in(i, 0) - iG); pout[1] += (oR - oG) * (oR - oG);
                    pin[2] += (in(i, 2) - iG) * (in(i, 2) - iG); pout[2] += (oB - oG) * (oB - oG);
                    follow += double(oR - oG + oB - oG) * (oG - t);
                    nP += 1;
                }
                if (x >= 16 && x < 240 && y >= 16 && y < H - 16 && !(x >= 48 && x < 208 && y >= 48 && y < 208)) {   // merged, around it
                    bin += (oG - t) * (oG - t);
                    nB += 1;
                }
                if (x >= 300 && x < 468 && y >= 40 && y < H - 40) {   // texture interior
                    const float s = t - kLevel;
                    proj += double(oG - kLevel) * s; projIn += double(iG - kLevel) * s; norm += double(s) * s;
                    tint += double(0.5f * (oR + oB) - oG) * s;
                }
                if (x >= 700 && y >= 8 && y < H - 8)
                    for (int k = 0; k < 3; ++k) far = std::max(far, std::fabs(out(i, k) - in(i, k)) / gk[k]);
            }
        const double lumaIn = std::sqrt(pin[0] / nP), lumaOut = std::sqrt(pout[0] / nP);
        const double colIn = std::sqrt((pin[1] + pin[2]) / (2 * nP)), colOut = std::sqrt((pout[1] + pout[2]) / (2 * nP));
        // regression of the colour grain (R' + B') / 2 - G' on the luma grain: about -1 for channels with independent grain,
        // pulled towards gain - 1 when the luma grain is moved by the same raw amount in every channel (bright grain purple,
        // dark grain yellow under warm light)
        const double colourFollowsLuma = follow / (2 * pout[0]);
        const double around = std::sqrt(bin / nB), keep = proj / norm, keepIn = projIn / norm, tinted = tint / norm;
        std::printf("wb %.2f / %.2f: few-frame patch: luma noise %.4f -> %.4f (merged around it %.4f), colour noise %.4f -> %.4f "
                    "(follows luma %+.2f) | texture amplitude %.3f -> %.3f, tint %+.3f | far merged area max change %.1e\n",
                    wbR, wbB, lumaIn, lumaOut, around, colIn, colOut, colourFollowsLuma, keepIn, keep, tinted, far);
        if (!(lumaOut < 0.7 * lumaIn)) throw std::runtime_error("the few-frame patch's luma grain is not lowered");
        if (!(lumaOut < 2.0 * around)) throw std::runtime_error("the few-frame patch stays far noisier than the merged area around it");
        if (!(colOut < 0.8 * colIn)) throw std::runtime_error("the few-frame patch's colour grain is not lowered");
        if (!(keep > 0.9 * keepIn)) throw std::runtime_error("texture above the noise loses amplitude");
        if (!(std::fabs(tinted) < 0.03)) throw std::runtime_error("grey texture comes out tinted after white balance");
        if (!(far < 2.5e-4f)) throw std::runtime_error("a merged area far from any few-frame area changes");
        return colourFollowsLuma;
}
}  // namespace

int main() {
    try {
        Gpu gpu;
        const double neutral = runCase(gpu, 1.0f, 1.0f);
        const double warm = runCase(gpu, 1.13f, 2.78f);   // tungsten
        // the pass in raw space: -0.95 -> -0.86 and tint +0.11 on this scene
        if (!(std::fabs(warm - neutral) < 0.04)) throw std::runtime_error("under warm light the colour grain follows the luma grain");
        std::puts("rexture_even_vulkan_test passed");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
