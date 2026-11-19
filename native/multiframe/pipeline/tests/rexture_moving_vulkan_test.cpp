// Rexture moving-texture restore (rx_moving.comp), its five steps as RextureRecorder::recordMovingRestore runs them, on a
// synthetic scene: truth = fine texture everywhere, base frame = its mosaic, merge = the truth softened. Companion statistics:
//  - left part (x < 224): most covering companions rejected (0.9), merge partly blended (m 0.3): a broad moving area.
//    Its top rows (y < 64): m 1, one frame (nothing blended);
//  - right part: companions accepted (rejected share 0.1), except a 16 x 16 px island rejected like the left part.
// Expected: the broad moving area moves towards the truth (base detail back); static areas, the island and the one-frame
// rows keep the merge exactly; colour differences after white balance never change.
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

// IEEE half <-> float (round to nearest; finite inputs only).
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
float q16(float f) { return fromHalf(toHalf(f)); }

constexpr uint32_t W = 512, H = 256;
constexpr float kBlack = 64.0f, kWhite = 1023.0f;
constexpr float kWb[3] = {2.2f, 1.0f, 1.6f};
bool isGreen(uint32_t x, uint32_t y) { return ((x + y) & 1u) == 1u; }   // RGGB
int channel(uint32_t x, uint32_t y) { return isGreen(x, y) ? 1 : ((y & 1u) == 0u ? 0 : 2); }
struct Pc { float black[4]; float white; int32_t w, h, cfa, op; float iwb[3]; };
}  // namespace

int main() {
    try {
        using namespace rawr::raw_gpu_pipeline;
        Gpu gpu;
        auto& c = gpu.c;
        // truth (white-balanced grey level) and its softened merge
        std::vector<float> t(size_t(W) * H), z(size_t(W) * H);
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x)
                t[size_t(y) * W + x] = 0.25f + 0.05f * std::sin(6.2831853f * float(x) / 5.0f) * std::sin(6.2831853f * float(y) / 7.0f + 0.4f);
        const auto tz = [&](int x, int y) { return t[size_t(std::clamp(y, 0, int(H) - 1)) * W + size_t(std::clamp(x, 0, int(W) - 1))]; };
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                float s = 0;
                for (int j = -2; j <= 2; ++j)
                    for (int k = -2; k <= 2; ++k) s += tz(int(x) + k, int(y) + j);
                z[size_t(y) * W + x] = s / 25.0f;
            }
        const uint32_t gw = W / 2, gh = H / 2, mw = W / 4, mh = H / 4, cw = (W + 7) / 8, ch = (H + 7) / 8;
        std::vector<uint16_t> raw(size_t(W) * H), out(size_t(W) * H * 4), mot(size_t(gw) * gh * 2);
        std::vector<float> m(size_t(mw) * mh);
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                const size_t i = size_t(y) * W + x;
                raw[i] = uint16_t(std::lround(kBlack + t[i] / kWb[channel(x, y)] * (kWhite - kBlack)));
                for (int k = 0; k < 3; ++k) out[i * 4 + size_t(k)] = toHalf(z[i] / kWb[k]);
                out[i * 4 + 3] = toHalf(1.0f);
            }
        const auto island = [](uint32_t x, uint32_t y) { return x >= 400 && x < 416 && y >= 160 && y < 176; };
        for (uint32_t gy = 0; gy < gh; ++gy)
            for (uint32_t gx = 0; gx < gw; ++gx) {
                const bool moving = 2 * gx < 224 || island(2 * gx, 2 * gy);
                mot[(size_t(gy) * gw + gx) * 2] = toHalf(8.0f * (moving ? 0.9f : 0.1f));
                mot[(size_t(gy) * gw + gx) * 2 + 1] = toHalf(8.0f);
            }
        for (uint32_t my = 0; my < mh; ++my)
            for (uint32_t mx = 0; mx < mw; ++mx) m[size_t(my) * mw + mx] = (4 * my < 64 && 4 * mx < 224) ? 1.0f : 0.3f;
        const auto usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        auto rawI = vktest::mkImg(c.pd, c.dev, W, H, VK_FORMAT_R16_UINT, usage);
        auto outI = vktest::mkImg(c.pd, c.dev, W, H, VK_FORMAT_R16G16B16A16_SFLOAT, usage);
        auto motI = vktest::mkImg(c.pd, c.dev, gw, gh, VK_FORMAT_R16G16_SFLOAT, usage);
        auto mI = vktest::mkImg(c.pd, c.dev, mw, mh, VK_FORMAT_R32_SFLOAT, usage);
        auto gA = vktest::mkImg(c.pd, c.dev, cw, ch, VK_FORMAT_R16_SFLOAT, usage);
        auto gB = vktest::mkImg(c.pd, c.dev, cw, ch, VK_FORMAT_R16_SFLOAT, usage);
        auto dI = vktest::mkImg(c.pd, c.dev, W, H, VK_FORMAT_R16G16B16A16_SFLOAT, usage);
        const VkDeviceSize rawB = VkDeviceSize(W) * H * 2, outB = VkDeviceSize(W) * H * 8, motB = VkDeviceSize(gw) * gh * 4, mB = VkDeviceSize(mw) * mh * 4;
        const auto host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        auto up = vktest::mkBuf(c.pd, c.dev, rawB + outB + motB + mB, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, host);
        auto down = vktest::mkBuf(c.pd, c.dev, outB, VK_BUFFER_USAGE_TRANSFER_DST_BIT, host);
        void* mapped = nullptr;
        vktest::ck(vkMapMemory(c.dev, up.m, 0, VK_WHOLE_SIZE, 0, &mapped), "map up");
        char* mp = static_cast<char*>(mapped);
        std::memcpy(mp, raw.data(), rawB);
        std::memcpy(mp + rawB, out.data(), outB);
        std::memcpy(mp + rawB + outB, mot.data(), motB);
        std::memcpy(mp + rawB + outB + motB, m.data(), mB);
        vkUnmapMemory(c.dev, up.m);
        VulkanExecutor exec;
        exec.initialize(c.dev, false);
        exec.beginBatch();
        const auto all = {&rawI, &outI, &motI, &mI, &gA, &gB, &dI};
        gpu.oneShot([&](VkCommandBuffer cmd) {
            for (auto* im : all) barrier(cmd, im->i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkDeviceSize off = 0;
            for (auto [im, w, h, bytes] : {std::tuple{&rawI, W, H, rawB}, std::tuple{&outI, W, H, outB}, std::tuple{&motI, gw, gh, motB}, std::tuple{&mI, mw, mh, mB}}) {
                VkBufferImageCopy cp{};
                cp.bufferOffset = off;
                cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                cp.imageExtent = {w, h, 1};
                vkCmdCopyBufferToImage(cmd, up.b, im->i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
                off += bytes;
            }
            for (auto* im : all) barrier(cmd, im->i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
            const auto ib = [](uint32_t b, const vktest::Img& i) { return ImageBinding{b, i.v, VK_IMAGE_LAYOUT_GENERAL}; };
            Pc p{};
            for (int i = 0; i < 4; ++i) p.black[i] = kBlack;
            p.white = kWhite;
            p.w = int32_t(W);
            p.h = int32_t(H);
            p.cfa = 0;
            for (int i = 0; i < 3; ++i) p.iwb[i] = 1.0f / kWb[i];
            for (int32_t op = 0; op < 5; ++op) {
                p.op = op;
                const uint32_t ww = op < 3 ? cw : W, hh = op < 3 ? ch : H;
                exec.record(cmd, ShaderId::RxMoving, {{ib(0, rawI), ib(1, outI), ib(2, motI), ib(3, mI), ib(4, gA), ib(5, gB), ib(6, dI)}, {}}, &p,
                            sizeof(p), (ww + 15) / 16, (hh + 15) / 16);
                for (auto* im : all) barrier(cmd, im->i, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
            }
            barrier(cmd, outI.i, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy cp{};
            cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            cp.imageExtent = {W, H, 1};
            vkCmdCopyImageToBuffer(cmd, outI.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, down.b, 1, &cp);
        });
        std::vector<uint16_t> res(size_t(W) * H * 4);
        vktest::ck(vkMapMemory(c.dev, down.m, 0, VK_WHOLE_SIZE, 0, &mapped), "map down");
        std::memcpy(res.data(), mapped, outB);
        vkUnmapMemory(c.dev, down.m);
        exec.reset();
        vktest::delBuf(c.dev, up);
        vktest::delBuf(c.dev, down);
        for (auto* im : all) vktest::delImg(c.dev, *im);
        double eMerge = 0, eOut = 0;
        float staticChange = 0, oneFrameChange = 0, chroma = 0;
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                const size_t i = size_t(y) * W + x;
                float o[3], a[3];
                for (int k = 0; k < 3; ++k) o[k] = fromHalf(res[i * 4 + size_t(k)]), a[k] = fromHalf(out[i * 4 + size_t(k)]);
                chroma = std::max({chroma, std::fabs((o[0] * kWb[0] - o[1]) - (a[0] * kWb[0] - a[1])), std::fabs((o[2] * kWb[2] - o[1]) - (a[2] * kWb[2] - a[1]))});
                if (x >= 352) staticChange = std::max(staticChange, std::fabs(o[1] - a[1]));   // static part, the island included
                if (y < 24 && x < 192) oneFrameChange = std::max(oneFrameChange, std::fabs(o[1] - a[1]));
                if (x >= 24 && x < 160 && y >= 128 && y < H - 24) {   // inside the broad moving area
                    eMerge += (a[1] - t[i]) * (a[1] - t[i]);
                    eOut += (o[1] - t[i]) * (o[1] - t[i]);
                }
            }
        eMerge = std::sqrt(eMerge), eOut = std::sqrt(eOut);
        std::printf("moving area: green rms error vs truth merge %.4f -> restored %.4f | static change %.1e | one-frame change %.1e | chroma change %.1e\n",
                    eMerge, eOut, staticChange, oneFrameChange, chroma);
        if (!(eOut < 0.5 * eMerge)) throw std::runtime_error("the broad moving area does not get the base detail back");
        if (!(staticChange == 0.0f)) throw std::runtime_error("a static area (or a small rejected island) changes");
        if (!(oneFrameChange == 0.0f)) throw std::runtime_error("a one-frame area changes");
        if (!(chroma < 2e-3f)) throw std::runtime_error("colour differences change");
        std::puts("rexture_moving_vulkan_test passed");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
