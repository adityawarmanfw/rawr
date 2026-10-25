// Offline harness for the WB-pass lens distortion resample: runs the real
// PostDemosaicProcessor on the host GPU for the full-frame (photo) and
// cropped (video) cases of the rational profile, dumps the synthetic source
// and each output as raw RGBA16F and prints the WB-pass GPU time with and
// without distortion. tests/distortion_compare.py scores the dumps against a
// double-precision reference, so the same harness measures any shader build.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "post_demosaic/PostDemosaicProcessor.h"
#include "vk_test_common.hpp"
using namespace vktest;

namespace {
VkImageMemoryBarrier bar(VkImage im, VkAccessFlags s, VkAccessFlags d, VkImageLayout o, VkImageLayout n) {
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
VkCommandBuffer cmdAlloc(const Ctx& c, VkCommandPool p) {
    VkCommandBuffer x{};
    VkCommandBufferAllocateInfo a{};
    a.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    a.commandPool = p;
    a.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    a.commandBufferCount = 1;
    ck(vkAllocateCommandBuffers(c.dev, &a, &x), "cmd alloc");
    return x;
}
void begin(VkCommandBuffer c) {
    VkCommandBufferBeginInfo b{};
    b.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    ck(vkBeginCommandBuffer(c, &b), "cmd begin");
}
void submit(const Ctx& c, VkCommandBuffer x) {
    ck(vkEndCommandBuffer(x), "cmd end");
    VkSubmitInfo s{};
    s.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    s.commandBufferCount = 1;
    s.pCommandBuffers = &x;
    ck(vkQueueSubmit(c.q, 1, &s, VK_NULL_HANDLE), "submit");
    ck(vkQueueWaitIdle(c.q), "wait");
}
Img upload(const Ctx& c, VkCommandPool p, const std::vector<uint8_t>& bytes, uint32_t W, uint32_t H) {
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
    auto b0 = bar(im.i, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
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
std::vector<uint8_t> readback(const Ctx& c, VkCommandPool p, VkImage im, uint32_t W, uint32_t H) {
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
    std::vector<uint8_t> o(static_cast<size_t>(n));
    std::memcpy(o.data(), m, size_t(n));
    vkUnmapMemory(c.dev, st.m);
    vkFreeCommandBuffers(c.dev, p, 1, &cb);
    delBuf(c.dev, st);
    return o;
}
uint16_t floatToHalf(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    int32_t e = int32_t((x >> 23) & 0xff) - 127 + 15;
    uint32_t m = x & 0x7fffffu;
    if (e <= 0) return uint16_t(sign);  // flush denormals: content stays well above them
    if (e >= 31) return uint16_t(sign | 0x7c00u);
    uint32_t h = (uint32_t(e) << 10) | (m >> 13);
    if (m & 0x1000u) ++h;  // round to nearest (ties up)
    return uint16_t(sign | h);
}
void writeFile(const std::string& path, const void* data, size_t size) {
    std::ofstream f(path, std::ios::binary);
    f.write(static_cast<const char*>(data), std::streamsize(size));
}

// Band-limited texture plus sharp grid lines: smooth enough that fp16 does
// not hide a sub-pixel misplacement, with edges that expose it.
std::vector<uint8_t> makeSource(uint32_t w, uint32_t h) {
    std::vector<uint16_t> px(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const float u = float(x), v = float(y);
            float base = 0.5f + 0.18f * std::sin(u * 0.045f) * std::cos(v * 0.037f) +
                         0.12f * std::sin((u + v) * 0.11f) + 0.05f * std::sin(u * 0.31f - v * 0.23f);
            const bool line = (x % 128u) < 3u || (y % 128u) < 3u;
            const float r = line ? 0.95f : base;
            const float g = line ? 0.05f : base * 0.9f;
            const float b = line ? 0.05f : 0.2f + 0.6f * float(x) / float(w);
            uint16_t* p = &px[(size_t(y) * w + x) * 4];
            p[0] = floatToHalf(r);
            p[1] = floatToHalf(g);
            p[2] = floatToHalf(b);
            p[3] = floatToHalf(1.0f);
        }
    }
    std::vector<uint8_t> bytes(px.size() * 2);
    std::memcpy(bytes.data(), px.data(), bytes.size());
    return bytes;
}

struct Case {
    const char* name;
    uint32_t width, height;
    bool crop;
    float outputScale;
    std::array<float, 2> origin;
    float frameScale;
};
}  // namespace

// usage: distortion_validate <out-dir> [iterations]
int main(int argc, char** argv) {
    try {
        if (argc < 2) throw std::runtime_error("usage: distortion_validate <out-dir> [iterations]");
        const std::string dir = argv[1];
        const int iterations = argc > 2 ? std::max(1, std::atoi(argv[2])) : 40;
        Ctx c = ctx();
        std::cout << "device " << c.prop.deviceName << "\n";
        VkCommandPool pool{};
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.queueFamilyIndex = c.qf;
        ck(vkCreateCommandPool(c.dev, &pi, nullptr, &pool), "pool");
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 32;
        VkQueryPool queries{};
        ck(vkCreateQueryPool(c.dev, &qi, nullptr, &queries), "query pool");

        // Calibration (4096x3072 frame, OpenCV rational) as in the profile.
        const std::array<float, 4> intrinsic{1698.182744f, 1696.500137f, 2042.360233f, 1548.313918f};
        const std::array<float, 5> distortion{-0.051307f, 0.015171f, 0.000123f, 0.000357f, 0.0f};
        const std::array<float, 3> denominator{0.0f, -0.023536f, 0.014256f};
        const Case cases[] = {{"photo", 4096, 3072, false, 0.9758f, {0.0f, 0.0f}, 1.0f},
                              {"video", 3840, 2160, true, 0.98474f, {128.0f, 456.0f}, 1.0f},
                              {"samsung_3x", 3648, 2736, false, 1.0f, {0.0f, 0.0f}, 1.0f}};
        for (const Case& cs : cases) {
            const auto bytes = makeSource(cs.width, cs.height);
            writeFile(dir + "/" + cs.name + "_src.f16", bytes.data(), bytes.size());
            Img src = upload(c, pool, bytes, cs.width, cs.height);
            rawr::post::PostDemosaicProcessor processor(c.pd, c.dev, c.qf, cs.width, cs.height, 0);
            for (const bool distort : {false, true}) {
                rawr::post::StillDistortionCorrection correction;
                if (distort) {
                    correction.enabled = true;
                    correction.model = rawr::post::StillDistortionCorrection::Model::Rational;
                    correction.intrinsic = {intrinsic[0], intrinsic[1], intrinsic[2], intrinsic[3], 0.0f};
                    correction.distortion = distortion;
                    correction.rationalDenominator = denominator;
                    correction.outputFocal = {intrinsic[0] * cs.outputScale, intrinsic[1] * cs.outputScale};
                    correction.frameOrigin = cs.origin;
                    correction.frameScale = cs.frameScale;
                    if (std::string(cs.name) == "samsung_3x") {
                        correction.model = rawr::post::StillDistortionCorrection::Model::Camera2;
                        correction.intrinsic = {6961.56f, 6975.01f, 1813.4f, 1445.27f, 0.0f};
                        correction.distortion = {-0.416185f, 8.34895f, -47.3359f, 0.0f, 0.0f};
                    }
                }
                std::vector<double> wbMs;
                for (int i = 0; i < iterations + 3; ++i) {
                    auto cmd = cmdAlloc(c, pool);
                    begin(cmd);
                    vkCmdResetQueryPool(cmd, queries, 0, 32);
                    processor.record(cmd, src.i, src.v, VK_NULL_HANDLE, VK_NULL_HANDLE, {1.0f, 1.0f, 1.0f}, false,
                                     correction, queries);
                    submit(c, cmd);
                    vkFreeCommandBuffers(c.dev, pool, 1, &cmd);
                    uint64_t t[2]{};
                    ck(vkGetQueryPoolResults(c.dev, queries, 0, 2, sizeof(t), t, sizeof(uint64_t),
                                             VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
                       "query results");
                    if (i >= 3) wbMs.push_back(double(t[1] - t[0]) * c.prop.limits.timestampPeriod / 1.0e6);
                }
                std::sort(wbMs.begin(), wbMs.end());
                std::cout << cs.name << " distortion=" << distort << " wb_ms median=" << wbMs[wbMs.size() / 2]
                          << " min=" << wbMs.front() << " p90=" << wbMs[wbMs.size() * 9 / 10] << "\n";
                if (distort) {
                    const auto out = readback(c, pool, processor.outputImage(), cs.width, cs.height);
                    writeFile(dir + "/" + cs.name + "_out.f16", out.data(), out.size());
                }
            }
            delImg(c.dev, src);
        }
        vkDestroyQueryPool(c.dev, queries, nullptr);
        vkDestroyCommandPool(c.dev, pool, nullptr);
        delCtx(c);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
