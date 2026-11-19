// Rexture detail fit (rx_fit.comp), the three steps RextureRecorder::recordFit runs, on a synthetic scene whose companion sums
// rx_accumulate would have produced (bs = ds * (K * truth green): companions consistent with the truth):
//  - op 0: the base green plane and its 3 x 3 standard deviation match a CPU reference,
//  - op 1: where the merge is soft (fine stripes), the fitted green moves towards the truth,
//  - op 1: where the merge already agrees with the data (flat), it does not change,
//  - op 1: where no companion sample arrived, non-green pixels keep the merge exactly,
//  - op 1: a non-finite sum keeps the merge,
//  - op 2: only grey detail is added (colour differences after white balance unchanged), green becomes the fitted green,
//  - op 3: a companion that matches the base gets motion weight 1, a patch that changed (moved) gets 0,
//  - the stencil is exact: companions at arbitrary sub-pixel offsets whose samples agree with the merge (sums built the way
//    rx_accumulate builds them) leave it unchanged (an assumed stencil sharpened every other pixel: a checkerboard).
#include <rawr/raw_gpu_pipeline/RextureFit.h>
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
namespace rf = rawr::raw_gpu_pipeline::rexture_fit;

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

constexpr uint32_t W = 128, H = 96;
constexpr float kBlack = 64.0f, kWhite = 1023.0f;
constexpr float kAlpha = 1.6e-4f, kBeta = 4.3e-7f;   // a daylight phone noise model (normalised units)
constexpr float kWb[3] = {2.2f, 1.0f, 1.6f};
bool isGreen(uint32_t x, uint32_t y) { return ((x + y) & 1u) == 1u; }   // RGGB
int channel(uint32_t x, uint32_t y) { return isGreen(x, y) ? 1 : ((y & 1u) == 0u ? 0 : 2); }

struct Result {
    std::vector<float> bg, fit, out, motion;   // bg: W x H x 2 (after op 0), fit: W x H (op 1), out: W x H x 4, motion: W / 2 x H / 2
};

// acc: W x H x 4 halves (bs, c0, cE, cS), acc2: W x H x 2 halves (cSE, cSW), z0: W x H x 4 halves.
Result run(Gpu& gpu, const std::vector<uint16_t>& raw, const std::vector<uint16_t>& acc, const std::vector<uint16_t>& acc2,
           const std::vector<uint16_t>& z0, const std::vector<uint16_t>& companion) {
    using namespace rawr::raw_gpu_pipeline;
    auto& c = gpu.c;
    const auto usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    auto rawI = vktest::mkImg(c.pd, c.dev, W, H, VK_FORMAT_R16_UINT, usage);
    auto bgI = vktest::mkImg(c.pd, c.dev, W, H, VK_FORMAT_R16G16_SFLOAT, usage);
    auto accI = vktest::mkImg(c.pd, c.dev, W, H, VK_FORMAT_R16G16B16A16_SFLOAT, usage);
    auto acc2I = vktest::mkImg(c.pd, c.dev, W, H, VK_FORMAT_R16G16_SFLOAT, usage);
    auto outI = vktest::mkImg(c.pd, c.dev, W, H, VK_FORMAT_R16G16B16A16_SFLOAT, usage);
    auto compI = vktest::mkImg(c.pd, c.dev, W, H, VK_FORMAT_R16_UINT, usage);
    auto flowI = vktest::mkImg(c.pd, c.dev, 1, 1, VK_FORMAT_R32G32_SFLOAT, usage);   // one tile, zero flow
    auto wI = vktest::mkImg(c.pd, c.dev, W / 2, H / 2, VK_FORMAT_R16_SFLOAT, usage);
    const VkDeviceSize n = VkDeviceSize(W) * H, rawB = n * 2, accB = n * 8, acc2B = n * 4, outB = n * 8, bgB = n * 4, wB = n / 2;
    const auto host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    auto up = vktest::mkBuf(c.pd, c.dev, rawB + accB + acc2B + outB + rawB, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, host);
    auto down = vktest::mkBuf(c.pd, c.dev, bgB + bgB + outB + wB, VK_BUFFER_USAGE_TRANSFER_DST_BIT, host);
    void* mapped = nullptr;
    vktest::ck(vkMapMemory(c.dev, up.m, 0, VK_WHOLE_SIZE, 0, &mapped), "map up");
    char* m = static_cast<char*>(mapped);
    std::memcpy(m, raw.data(), rawB);
    std::memcpy(m + rawB, acc.data(), accB);
    std::memcpy(m + rawB + accB, acc2.data(), acc2B);
    std::memcpy(m + rawB + accB + acc2B, z0.data(), outB);
    std::memcpy(m + rawB + accB + acc2B + outB, companion.data(), rawB);
    vkUnmapMemory(c.dev, up.m);
    VulkanExecutor exec;
    exec.initialize(c.dev, false);
    exec.beginBatch();
    const auto all = {&rawI, &bgI, &accI, &acc2I, &outI, &compI, &flowI, &wI};
    gpu.oneShot([&](VkCommandBuffer cmd) {
        for (auto* im : all) barrier(cmd, im->i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {W, H, 1};
        VkDeviceSize off = 0;
        for (auto* im : {&rawI, &accI, &acc2I, &outI, &compI}) {
            copy.bufferOffset = off;
            vkCmdCopyBufferToImage(cmd, up.b, im->i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            off += im == &rawI || im == &compI ? rawB : im == &accI ? accB : im == &acc2I ? acc2B : outB;
        }
        const VkClearColorValue zero{};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, flowI.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
        for (auto* im : all) barrier(cmd, im->i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        const auto ib = [](uint32_t b, const vktest::Img& i) { return ImageBinding{b, i.v, VK_IMAGE_LAYOUT_GENERAL}; };
        const auto bgOut = [&](VkDeviceSize at) {   // copy the base green plane (later the fitted green) out
            barrier(cmd, bgI.i, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy o{};
            o.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            o.imageExtent = {W, H, 1};
            o.bufferOffset = at;
            vkCmdCopyImageToBuffer(cmd, bgI.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, down.b, 1, &o);
            barrier(cmd, bgI.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        };
        for (int32_t op : {0, 3, 1, 2}) {   // as recordFit (the grain pass runs between op 1 and op 2; not here)
            rf::PushConstants p{};
            for (int i = 0; i < 4; ++i) p.black[i] = kBlack;
            p.white = kWhite;
            p.w = int32_t(W);
            p.h = int32_t(H);
            p.cfa = 0;
            p.op = op;
            p.omega = rf::kOmega;
            p.delta = rf::kDelta;
            for (int i = 0; i < 3; ++i) p.iwb[i] = 1.0f / kWb[i];
            p.alpha = kAlpha;
            p.beta = kBeta;
            p.gain = 1.0f;
            const uint32_t gw = op == 3 ? W / 2 : W, gh = op == 3 ? H / 2 : H;
            exec.record(cmd, ShaderId::RxFit,
                        {{ib(0, op == 3 ? compI : rawI), ib(1, bgI), ib(2, accI), ib(3, outI), ib(4, flowI), ib(5, wI), ib(6, acc2I)}, {}},
                        &p, sizeof(p), (gw + 15) / 16, (gh + 15) / 16);
            for (auto* im : {&bgI, &outI, &wI}) barrier(cmd, im->i, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
            if (op == 3) bgOut(0);
        }
        bgOut(bgB);
        for (auto* im : {&outI, &wI}) barrier(cmd, im->i, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        copy.bufferOffset = bgB + bgB;
        vkCmdCopyImageToBuffer(cmd, outI.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, down.b, 1, &copy);
        copy.bufferOffset = bgB + bgB + outB;
        copy.imageExtent = {W / 2, H / 2, 1};
        vkCmdCopyImageToBuffer(cmd, wI.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, down.b, 1, &copy);
    });
    std::vector<uint16_t> h16(size_t(bgB + bgB + outB + wB) / 2);
    vktest::ck(vkMapMemory(c.dev, down.m, 0, VK_WHOLE_SIZE, 0, &mapped), "map down");
    std::memcpy(h16.data(), mapped, bgB + bgB + outB + wB);
    vkUnmapMemory(c.dev, down.m);
    exec.reset();
    vktest::delBuf(c.dev, up);
    vktest::delBuf(c.dev, down);
    for (auto* im : all) vktest::delImg(c.dev, *im);
    Result r;
    r.bg.resize(size_t(n) * 2);
    r.fit.resize(size_t(n));
    r.out.resize(size_t(n) * 4);
    r.motion.resize(size_t(n) / 4);
    for (size_t i = 0; i < size_t(n) * 2; ++i) r.bg[i] = fromHalf(h16[i]);
    for (size_t i = 0; i < size_t(n); ++i) r.fit[i] = fromHalf(h16[size_t(n) * 2 + i * 2]);
    for (size_t i = 0; i < size_t(n) * 4; ++i) r.out[i] = fromHalf(h16[size_t(n) * 4 + i]);
    for (size_t i = 0; i < size_t(n) / 4; ++i) r.motion[i] = fromHalf(h16[size_t(n) * 8 + i]);
    return r;
}
}  // namespace

int main() {
    try {
        Gpu gpu;
        // Truth (camera RGB): rows < 48: [0, 64) grey vertical stripes, period 4 px; [64, 128) flat grey. Rows >= 48: flat warm.
        // Merge z0: the truth softened by [1 2 1] / 4 in both directions. Base raw: the truth's mosaic (no noise). Companion sums:
        // ds = 6 and bs = ds (K * truth green) everywhere except rows >= 48, columns >= 64 (no companion sample there).
        std::vector<float> truth(size_t(W) * H * 3), z0(size_t(W) * H * 3);
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                float rgb[3];
                if (y < 48) {
                    const float v = x < 64 ? 0.2f + 0.05f * std::sin(6.2831853f * float(x) / 4.0f + 0.7f) : 0.2f;
                    for (int ch = 0; ch < 3; ++ch) rgb[ch] = v / kWb[ch];
                } else {
                    const float warm[3] = {0.14f, 0.10f, 0.06f};
                    for (int ch = 0; ch < 3; ++ch) rgb[ch] = warm[ch] / kWb[ch];
                }
                for (int ch = 0; ch < 3; ++ch) truth[(size_t(y) * W + x) * 3 + ch] = rgb[ch];
            }
        const auto at = [&](const std::vector<float>& a, int x, int y, int ch) {
            x = std::clamp(x, 0, int(W) - 1), y = std::clamp(y, 0, int(H) - 1);
            return a[(size_t(y) * W + size_t(x)) * 3 + size_t(ch)];
        };
        const float k121[3] = {0.25f, 0.5f, 0.25f}, k161[3] = {1.0f / 6.0f, 2.0f / 3.0f, 1.0f / 6.0f};
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x)
                for (int ch = 0; ch < 3; ++ch) {
                    float s = 0;
                    for (int j = -1; j <= 1; ++j)
                        for (int k = -1; k <= 1; ++k) s += k121[j + 1] * k121[k + 1] * at(truth, int(x) + k, int(y) + j, ch);
                    z0[(size_t(y) * W + x) * 3 + size_t(ch)] = q16(s);
                }
        std::vector<uint16_t> raw(size_t(W) * H), acc(size_t(W) * H * 4), acc2(size_t(W) * H * 2), z0h(size_t(W) * H * 4);
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) raw[size_t(y) * W + x] = uint16_t(std::lround(kBlack + at(truth, int(x), int(y), channel(x, y)) * (kWhite - kBlack)));
        const auto norm = [&](uint32_t x, uint32_t y) {
            return (float(raw[size_t(y) * W + x]) - kBlack) / (kWhite - kBlack);
        };
        const auto green = [&](int x, int y) {   // the base green plane (op 0)
            x = std::clamp(x, 0, int(W) - 1), y = std::clamp(y, 0, int(H) - 1);
            if (isGreen(uint32_t(x), uint32_t(y))) return norm(uint32_t(x), uint32_t(y));
            const auto n = [&](int a, int b) { return norm(uint32_t(std::clamp(a, 0, int(W) - 1)), uint32_t(std::clamp(b, 0, int(H) - 1))); };
            return 0.25f * (n(x + 1, y) + n(x - 1, y) + n(x, y + 1) + n(x, y - 1));
        };
        // Companion sums of 6 frames' worth of samples with evenly spread offsets: A's stencil is then ds times the
        // bilinear-squared stencil K (centre 4/9, edges 1/9, corners 1/36), and samples consistent with the truth give
        // bs = ds (K * truth green - base green).
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                const size_t i = size_t(y) * W + x;
                for (int ch = 0; ch < 3; ++ch) z0h[i * 4 + size_t(ch)] = toHalf(z0[i * 3 + size_t(ch)]);
                z0h[i * 4 + 3] = toHalf(1.0f);
                float kt = 0;
                for (int j = -1; j <= 1; ++j)
                    for (int k = -1; k <= 1; ++k) kt += k161[j + 1] * k161[k + 1] * at(truth, int(x) + k, int(y) + j, 1);
                const float ds = (y >= 48 && x >= 64) ? 0.0f : 6.0f;
                acc[i * 4] = toHalf(ds * (kt - green(int(x), int(y))));
                acc[i * 4 + 1] = toHalf(ds * 4.0f / 9.0f);
                acc[i * 4 + 2] = toHalf(ds / 9.0f);
                acc[i * 4 + 3] = toHalf(ds / 9.0f);
                acc2[i * 2] = toHalf(ds / 36.0f);
                acc2[i * 2 + 1] = toHalf(ds / 36.0f);
            }
        constexpr uint32_t kNanX = 20, kNanY = 70;   // a non-finite companion sum
        acc[(size_t(kNanY) * W + kNanX) * 4] = 0x7e00u;
        // Companion: the base mosaic, except a 16 x 16 patch (columns 72..87, rows 8..23, in the flat area) that changed
        // (something moved in): +0.15.
        std::vector<uint16_t> companion = raw;
        for (uint32_t y = 8; y < 24; ++y)
            for (uint32_t x = 72; x < 88; ++x) companion[size_t(y) * W + x] = uint16_t(raw[size_t(y) * W + x] + std::lround(0.15f * (kWhite - kBlack)));
        const Result r = run(gpu, raw, acc, acc2, z0h, companion);
        // 1) base green plane and its 3 x 3 variance
        float bgErr = 0, varErr = 0, varGot = 0, varRef = 0;
        uint32_t varAt[2] = {0, 0};
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                float s = 0, s2 = 0;
                for (int j = -1; j <= 1; ++j)
                    for (int k = -1; k <= 1; ++k) {
                        const float v = green(int(x) + k, int(y) + j);
                        s += v, s2 += v * v;
                    }
                const float var = std::sqrt(std::max(s2 / 9 - (s / 9) * (s / 9), 0.0f));   // standard deviation
                bgErr = std::max(bgErr, std::fabs(r.bg[(size_t(y) * W + x) * 2] - green(int(x), int(y))));
                const float ve = std::fabs(r.bg[(size_t(y) * W + x) * 2 + 1] - var) / (var + 1e-3f);
                if (ve > varErr) varErr = ve, varAt[0] = x, varAt[1] = y, varGot = r.bg[(size_t(y) * W + x) * 2 + 1], varRef = var;
            }
        // 2) stripes: the fitted green is closer to the truth than the merge
        double eMerge = 0, eFit = 0;
        for (uint32_t y = 8; y < 40; ++y)
            for (uint32_t x = 8; x < 56; ++x) {
                const size_t i = size_t(y) * W + x;
                const double t = truth[i * 3 + 1];
                eMerge += (z0[i * 3 + 1] - t) * (z0[i * 3 + 1] - t);
                eFit += (r.fit[i] - t) * (r.fit[i] - t);
            }
        eMerge = std::sqrt(eMerge), eFit = std::sqrt(eFit);
        // 3) flat, consistent data: no change; 4) no companion data: non-green pixels keep the merge exactly
        float flatChange = 0, noDataChange = 0;
        for (uint32_t y = 8; y < 40; ++y)
            for (uint32_t x = 72; x < 120; ++x) flatChange = std::max(flatChange, std::fabs(r.fit[size_t(y) * W + x] - z0[(size_t(y) * W + x) * 3 + 1]));
        for (uint32_t y = 50; y < H; ++y)
            for (uint32_t x = 66; x < W; ++x)
                if (!isGreen(x, y)) noDataChange = std::max(noDataChange, std::fabs(r.fit[size_t(y) * W + x] - z0[(size_t(y) * W + x) * 3 + 1]));
        // 5) non-finite sum: the merge
        const size_t ni = size_t(kNanY) * W + kNanX;
        const bool nanKept = std::isfinite(r.out[ni * 4 + 1]) && std::fabs(r.fit[ni] - z0[ni * 3 + 1]) == 0.0f;
        // 6) op 2: colour differences after white balance unchanged, green = fitted green
        float chroma = 0, greenErr = 0;
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                const size_t i = size_t(y) * W + x;
                const float* o = &r.out[i * 4];
                const float* a = &z0[i * 3];
                chroma = std::max(chroma, std::fabs((o[0] * kWb[0] - o[1]) - (a[0] * kWb[0] - a[1])));
                chroma = std::max(chroma, std::fabs((o[2] * kWb[2] - o[1]) - (a[2] * kWb[2] - a[1])));
                greenErr = std::max(greenErr, std::fabs(o[1] - r.fit[i]));
            }
        // 7) motion weight: 1 on matching cells (away from the patch by 2 px), 0 inside the patch
        float staticMin = 1.0f, movedMax = 0.0f;
        for (uint32_t cy = 1; cy < H / 2 - 1; ++cy)
            for (uint32_t cx = 1; cx < W / 2 - 1; ++cx) {
                const float wgt = r.motion[size_t(cy) * (W / 2) + cx];
                const bool inside = cx >= 37 && cx < 43 && cy >= 5 && cy < 11, near = cx >= 34 && cx < 46 && cy >= 2 && cy < 14;
                if (inside) movedMax = std::max(movedMax, wgt);
                else if (!near) staticMin = std::min(staticMin, wgt);
            }
        std::printf("motion weight: static min %.2f, moved patch max %.2f\n", staticMin, movedMax);
        // 8) exact stencil: the merge is the truth (diagonal stripes, period 3.7 px), 6 companions at random sub-pixel offsets
        // sample it exactly; sums as rx_accumulate keeps them. Nothing may change, and no checkerboard may appear.
        float consistentChange = 0, checker = 0;
        {
            std::vector<float> t2(size_t(W) * H);
            for (uint32_t y = 0; y < H; ++y)
                for (uint32_t x = 0; x < W; ++x) t2[size_t(y) * W + x] = 0.2f + 0.06f * std::sin(6.2831853f * float(x + y) / 3.7f);
            const auto tz = [&](int x, int y) { return t2[size_t(std::clamp(y, 0, int(H) - 1)) * W + size_t(std::clamp(x, 0, int(W) - 1))]; };
            std::vector<uint16_t> raw2(size_t(W) * H), z2(size_t(W) * H * 4);
            for (uint32_t y = 0; y < H; ++y)
                for (uint32_t x = 0; x < W; ++x) {
                    const size_t i = size_t(y) * W + x;
                    float rgb[3];
                    for (int ch = 0; ch < 3; ++ch) rgb[ch] = q16(t2[i] / kWb[ch]);
                    raw2[i] = uint16_t(std::lround(kBlack + rgb[channel(x, y)] * (kWhite - kBlack)));
                    for (int ch = 0; ch < 3; ++ch) z2[i * 4 + size_t(ch)] = toHalf(rgb[ch]);
                    z2[i * 4 + 3] = toHalf(1.0f);
                }
            const auto norm2 = [&](int x, int y) {
                x = std::clamp(x, 0, int(W) - 1), y = std::clamp(y, 0, int(H) - 1);
                return (float(raw2[size_t(y) * W + size_t(x)]) - kBlack) / (kWhite - kBlack);
            };
            const auto green2 = [&](int x, int y) {
                if (isGreen(uint32_t(std::clamp(x, 0, int(W) - 1)), uint32_t(std::clamp(y, 0, int(H) - 1)))) return norm2(x, y);
                return 0.25f * (norm2(x + 1, y) + norm2(x - 1, y) + norm2(x, y + 1) + norm2(x, y - 1));
            };
            std::vector<double> bs(size_t(W) * H), c0(size_t(W) * H), cE(size_t(W) * H), cS(size_t(W) * H), cSE(size_t(W) * H),
                cSW(size_t(W) * H);
            uint32_t seed = 7u;
            const auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return float(seed >> 8) / 16777216.0f; };
            for (int f = 0; f < 6; ++f) {
                const float ox = 3.0f * rnd() - 1.5f, oy = 3.0f * rnd() - 1.5f;   // the frame's offset (flow), any sub-pixel phase
                for (int qy = 0; qy < int(H); ++qy)
                    for (int qx = 0; qx < int(W); ++qx) {
                        if (!isGreen(uint32_t(qx), uint32_t(qy))) continue;
                        const float px = float(qx) - ox, py = float(qy) - oy;   // the sample's scene point on the output grid
                        const int x0 = int(std::floor(px)), y0 = int(std::floor(py));
                        if (x0 < 0 || y0 < 0 || x0 + 1 >= int(W) || y0 + 1 >= int(H)) continue;
                        const float fx = px - float(x0), fy = py - float(y0);
                        const float y = (1 - fx) * (1 - fy) * tz(x0, y0) + fx * (1 - fy) * tz(x0 + 1, y0) + (1 - fx) * fy * tz(x0, y0 + 1) +
                                        fx * fy * tz(x0 + 1, y0 + 1);
                        const int cx[4] = {x0, x0 + 1, x0, x0 + 1}, cy[4] = {y0, y0, y0 + 1, y0 + 1};
                        const float cb[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
                        for (int a = 0; a < 4; ++a) {   // the output pixels the sample covers
                            const size_t i = size_t(cy[a]) * W + size_t(cx[a]);
                            bs[i] += cb[a] * (y - green2(cx[a], cy[a]));
                            for (int b = 0; b < 4; ++b) {
                                const int kx = cx[b] - cx[a], ky = cy[b] - cy[a];
                                const double v = double(cb[a]) * cb[b];
                                if (kx == 0 && ky == 0) c0[i] += v;
                                else if (kx == 1 && ky == 0) cE[i] += v;
                                else if (kx == 0 && ky == 1) cS[i] += v;
                                else if (kx == 1 && ky == 1) cSE[i] += v;
                                else if (kx == -1 && ky == 1) cSW[i] += v;
                            }
                        }
                    }
            }
            std::vector<uint16_t> a1(size_t(W) * H * 4), a2(size_t(W) * H * 2);
            for (size_t i = 0; i < size_t(W) * H; ++i) {
                a1[i * 4] = toHalf(float(bs[i])), a1[i * 4 + 1] = toHalf(float(c0[i])), a1[i * 4 + 2] = toHalf(float(cE[i]));
                a1[i * 4 + 3] = toHalf(float(cS[i])), a2[i * 2] = toHalf(float(cSE[i])), a2[i * 2 + 1] = toHalf(float(cSW[i]));
            }
            const Result r2 = run(gpu, raw2, a1, a2, z2, raw2);
            double chk = 0;
            int nchk = 0;
            for (uint32_t y = 4; y < H - 4; ++y)
                for (uint32_t x = 4; x < W - 4; ++x) {
                    const size_t i = size_t(y) * W + x;
                    const float d = r2.fit[i] - fromHalf(z2[i * 4 + 1]);
                    consistentChange = std::max(consistentChange, std::fabs(d));
                    chk += ((x + y) & 1u ? -1.0 : 1.0) * d;
                    ++nchk;
                }
            checker = float(std::fabs(chk / nchk));
        }
        std::printf("exact stencil: max change %.1e, checkerboard component %.1e\n", consistentChange, checker);
        std::printf("base green max err %.1e, std max rel err %.1e (at %u, %u: %.3e vs %.3e) | stripes: green rms error merge %.4f -> fit %.4f | flat change %.1e | "
                    "no-data change %.1e | non-finite sum kept %d | chroma change %.1e, green vs fit %.1e\n",
                    bgErr, varErr, varAt[0], varAt[1], varGot, varRef, eMerge, eFit, flatChange, noDataChange, int(nanKept), chroma, greenErr);
        if (!(bgErr < 2e-3f)) throw std::runtime_error("base green plane differs from the reference");
        if (!(varErr < 0.05f)) throw std::runtime_error("base green standard deviation differs from the reference");
        if (!(eFit < 0.5 * eMerge)) throw std::runtime_error("the fit does not restore the stripes");
        if (!(flatChange < 2e-3f)) throw std::runtime_error("consistent flat data changes the merge");
        if (!(noDataChange == 0.0f)) throw std::runtime_error("pixels without companion data change");
        if (!nanKept) throw std::runtime_error("a non-finite sum reaches the output");
        if (!(chroma < 2e-3f)) throw std::runtime_error("colour differences change");
        if (!(greenErr < 1e-3f)) throw std::runtime_error("the output green is not the fitted green");
        if (!(staticMin > 0.99f)) throw std::runtime_error("a matching companion is down-weighted");
        if (!(movedMax == 0.0f)) throw std::runtime_error("a moved patch is not rejected");
        if (!(consistentChange < 2e-3f)) throw std::runtime_error("companions that agree with the merge change it");
        if (!(checker < 1e-4f)) throw std::runtime_error("the fit leaves a checkerboard");
        std::puts("rexture_fit_vulkan_test passed");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
