#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

#include "post_demosaic/LabDefringe.h"
#include "vk_test_common.hpp"
using namespace vktest;

static std::vector<uint8_t> readBytes(const std::string& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("Cannot open " + p);
    auto n = f.tellg();
    if (n < 0) throw std::runtime_error("tellg failed");
    f.seekg(0);
    std::vector<uint8_t> b(static_cast<size_t>(n));
    if (n > 0) f.read(reinterpret_cast<char*>(b.data()), n);
    return b;
}
static VkImageMemoryBarrier bar(VkImage im, VkAccessFlags s, VkAccessFlags d, VkImageLayout o, VkImageLayout n) {
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
static VkCommandBuffer cmdAlloc(const Ctx& c, VkCommandPool p) {
    VkCommandBuffer x{};
    VkCommandBufferAllocateInfo a{};
    a.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    a.commandPool = p;
    a.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    a.commandBufferCount = 1;
    ck(vkAllocateCommandBuffers(c.dev, &a, &x), "cmd alloc");
    return x;
}
static void begin(VkCommandBuffer c) {
    VkCommandBufferBeginInfo b{};
    b.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    ck(vkBeginCommandBuffer(c, &b), "cmd begin");
}
static void submit(const Ctx& c, VkCommandBuffer x) {
    ck(vkEndCommandBuffer(x), "cmd end");
    VkSubmitInfo s{};
    s.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    s.commandBufferCount = 1;
    s.pCommandBuffers = &x;
    ck(vkQueueSubmit(c.q, 1, &s, VK_NULL_HANDLE), "submit");
    ck(vkQueueWaitIdle(c.q), "wait");
}
static Img upload(const Ctx& c, VkCommandPool p, const std::vector<uint8_t>& bytes, uint32_t W, uint32_t H) {
    Buf st = mkBuf(c.pd, c.dev, bytes.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    void* m = nullptr;
    ck(vkMapMemory(c.dev, st.m, 0, bytes.size(), 0, &m), "map");
    std::memcpy(m, bytes.data(), bytes.size());
    vkUnmapMemory(c.dev, st.m);
    Img im = mkImg(c.pd, c.dev, W, H, VK_FORMAT_R16G16B16A16_SFLOAT,
                   VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    auto cb = cmdAlloc(c, p);
    begin(cb);
    auto b0 =
        bar(im.i, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
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
static void initOut(VkCommandBuffer cb, VkImage im) {
    auto b = bar(im, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
}
static std::vector<uint8_t> readback(const Ctx& c, VkCommandPool p, VkImage im, uint32_t W, uint32_t H) {
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
    std::vector<uint8_t> o;
    o.resize(static_cast<size_t>(n));
    std::memcpy(o.data(), m, size_t(n));
    vkUnmapMemory(c.dev, st.m);
    vkFreeCommandBuffers(c.dev, p, 1, &cb);
    delBuf(c.dev, st);
    return o;
}

// File harness for reference parity, odd/tiny/bypass and per-device timing.
int main(int argc, char** argv) {
    try {
        if (argc < 8)
            throw std::runtime_error(
                "usage: lab_defringe_validate INPUT OUTPUT WIDTH HEIGHT MATRIX.txt STRENGTH ITERATIONS");
        uint32_t w = std::stoul(argv[3]), h = std::stoul(argv[4]);
        float strength = std::stof(argv[6]);
        int count = std::stoi(argv[7]);
        if (!w || !h || count < 1) throw std::runtime_error("invalid geometry/iterations");
        std::array<float, 9> matrix{};
        std::ifstream f(argv[5]);
        for (auto& v : matrix)
            if (!(f >> v)) throw std::runtime_error("invalid matrix file");
        auto bytes = readBytes(argv[1]);
        if (bytes.size() != size_t(w) * h * 8) throw std::runtime_error("input size");
        Ctx c = ctx();
        VkCommandPool pool{};
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.queueFamilyIndex = c.qf;
        ck(vkCreateCommandPool(c.dev, &pi, nullptr, &pool), "pool");
        Img src = upload(c, pool, bytes, w, h);
        Img dst = mkImg(c.pd, c.dev, w, h, VK_FORMAT_R16G16B16A16_SFLOAT,
                        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        auto cmd = cmdAlloc(c, pool);
        begin(cmd);
        initOut(cmd, dst.i);
        submit(c, cmd);
        vkFreeCommandBuffers(c.dev, pool, 1, &cmd);
        VkQueryPool queries{};
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 2;
        ck(vkCreateQueryPool(c.dev, &qi, nullptr, &queries), "queries");
        std::vector<double> ms;
        {
            rawr::post::LabDefringe lab(c.pd, c.dev, w, h);
            for (int i = 0; i < count + (count > 1 ? 20 : 0); ++i) {
                auto cb = cmdAlloc(c, pool);
                begin(cb);
                vkCmdResetQueryPool(cb, queries, 0, 2);
                vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, queries, 0);
                lab.record(cb, src.v, dst.v, matrix.data(), strength);
                vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, queries, 1);
                submit(c, cb);
                uint64_t t[2]{};
                ck(vkGetQueryPoolResults(c.dev, queries, 0, 2, sizeof(t), t, sizeof(uint64_t),
                                         VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
                   "timestamps");
                if (i >= (count > 1 ? 20 : 0)) ms.push_back(double(t[1] - t[0]) * c.prop.limits.timestampPeriod * 1e-6);
                vkFreeCommandBuffers(c.dev, pool, 1, &cb);
            }
            std::sort(ms.begin(), ms.end());
            std::cout << "LAB_PASS gpu=" << c.prop.deviceName << " median_ms=" << ms[ms.size() / 2]
                      << " scratch_bytes=" << lab.allocatedBytes() << "\n";
        }
        auto result = readback(c, pool, dst.i, w, h);
        std::ofstream output(argv[2], std::ios::binary);
        output.write(reinterpret_cast<const char*>(result.data()), result.size());
        if (!output) throw std::runtime_error("output write");
        vkDestroyQueryPool(c.dev, queries, nullptr);
        delImg(c.dev, src);
        delImg(c.dev, dst);
        vkDestroyCommandPool(c.dev, pool, nullptr);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
