#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#include "../../vk_common/testing/vk_test_common.hpp"
#include "raw_preview/RawPreview.hpp"
using namespace vktest;
struct Slot {
    VkCommandPool pool{};
    VkCommandBuffer cmd{};
    VkFence f{};
    VkQueryPool qp{};
    Img raw{}, out{}, state{};
};
static std::vector<uint16_t> pattern(uint32_t W, uint32_t H, bool stress) {
    std::vector<uint16_t> r(size_t(W) * H);
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++) {
            float fx = float(x) / W, fy = float(y) / H;
            float v = stress ? (.55f + 1.15f * fx + .35f * std::sin(fy * 20.f)) : (.15f + .65f * fx);
            if (stress && fx > .60f && fy > .18f && fy < .82f) v += .65f;
            v = std::max(0.f, std::min(1.f, v));
            r[size_t(y) * W + x] = (uint16_t)std::lround(64 + v * (4095 - 64));
        }
    return r;
}
static void upload(Ctx& c, VkCommandBuffer cmd, Img& im, const std::vector<uint16_t>& raw, uint32_t W, uint32_t H) {
    VkDeviceSize n = raw.size() * 2;
    Buf b = mkBuf(c.pd, c.dev, n, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    void* m;
    ck(vkMapMemory(c.dev, b.m, 0, n, 0, &m), "map");
    std::memcpy(m, raw.data(), n);
    vkUnmapMemory(c.dev, b.m);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &bi);
    VkImageMemoryBarrier a{};
    a.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    a.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    a.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    a.srcQueueFamilyIndex = a.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    a.image = im.i;
    a.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    a.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &a);
    VkBufferImageCopy bc{};
    bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bc.imageExtent = {W, H, 1};
    vkCmdCopyBufferToImage(cmd, b.b, im.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
    VkImageMemoryBarrier g{};
    g.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    g.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    g.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    g.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    g.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    g.srcQueueFamilyIndex = g.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    g.image = im.i;
    g.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &g);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(c.q, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(c.q);
    vkResetCommandBuffer(cmd, 0);
    delBuf(c.dev, b);
}
static double pct(const std::vector<double>& v, double p) {
    return v[std::min(v.size() - 1, size_t(p * (v.size() - 1)))];
}
// Optional synthetic Camera2 lens-shading map (bench-only): smooth
// vignetting, center 1.0, corners up to ~2.2 with per-channel variation,
// mimicking a real wide-angle gain grid.
static std::vector<float> g_lsc;
static uint32_t g_lscW = 0, g_lscH = 0;
static std::string g_dump;
static void makeSyntheticLsc(uint32_t gw, uint32_t gh) {
    g_lscW = gw;
    g_lscH = gh;
    g_lsc.resize(size_t(gw) * gh * 4u);
    for (uint32_t y = 0; y < gh; y++)
        for (uint32_t x = 0; x < gw; x++) {
            float nx = (float(x) / float(gw - 1)) * 2.f - 1.f;
            float ny = (float(y) / float(gh - 1)) * 2.f - 1.f;
            float r2 = nx * nx + ny * ny;
            float base = 1.f + 0.62f * r2 + 0.11f * r2 * r2;
            size_t o = (size_t(y) * gw + x) * 4u;
            g_lsc[o + 0] = base * 1.06f;
            g_lsc[o + 1] = base * 0.98f;
            g_lsc[o + 2] = base * 1.00f;
            g_lsc[o + 3] = base * 1.12f;
        }
}
// --method: 0 colour propagation, 1 Inpaint Opposed.
static uint32_t g_highlightMethod = 0;
static void run(Ctx& c, const std::string& shader, const std::string& maskShader, uint32_t W, uint32_t H,
                uint32_t frames, double fps, bool stress) {
    Slot s[3];
    for (int k = 0; k < 3; k++) {
        VkCommandPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pi.queueFamilyIndex = c.qf;
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        ck(vkCreateCommandPool(c.dev, &pi, nullptr, &s[k].pool), "pool");
        VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = s[k].pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        ck(vkAllocateCommandBuffers(c.dev, &ai, &s[k].cmd), "cmd");
        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        ck(vkCreateFence(c.dev, &fi, nullptr, &s[k].f), "fence");
        VkQueryPoolCreateInfo qi{};
        qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 2;
        ck(vkCreateQueryPool(c.dev, &qi, nullptr, &s[k].qp), "qp");
        s[k].raw =
            mkImg(c.pd, c.dev, W, H, VK_FORMAT_R16_UINT, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
        s[k].out = mkImg(c.pd, c.dev, W / 2, H / 2, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT);
        s[k].state = mkImg(c.pd, c.dev, W / 2, H / 2, VK_FORMAT_R16_UINT, VK_IMAGE_USAGE_STORAGE_BIT);
        auto raw = pattern(W, H, stress);
        upload(c, s[k].cmd, s[k].raw, raw, W, H);
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(s[k].cmd, &bi);
        VkImageMemoryBarrier og{};
        og.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        og.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        og.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        og.srcQueueFamilyIndex = og.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        og.image = s[k].out.i;
        og.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        og.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(s[k].cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &og);
        VkImageMemoryBarrier sg = og;
        sg.image = s[k].state.i;
        vkCmdPipelineBarrier(s[k].cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &sg);
        vkEndCommandBuffer(s[k].cmd);
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &s[k].cmd;
        vkQueueSubmit(c.q, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(c.q);
        vkResetCommandBuffer(s[k].cmd, 0);
    }
    raw_preview::RawPreviewCreateInfo ci{};
    ci.physicalDevice = c.pd;
    ci.device = c.dev;
    ci.shaderPath = shader;
    ci.cfaStateShaderPath = maskShader;
    raw_preview::RawPreview pr(ci);
    raw_preview::RawFrameParameters rp{};
    rp.blackLevel[0] = rp.blackLevel[1] = rp.blackLevel[2] = rp.blackLevel[3] = 64;
    rp.whiteLevel = 4095;
    rp.clipThreshold = .995f;
    rp.edgeStrength = 8;
    rp.chromaBlend = .2f;
    rp.highlightMethod = g_highlightMethod;
    const uint32_t warm = 60, total = warm + frames;
    std::vector<uint64_t> pf(3, UINT64_MAX);
    std::vector<double> ms;
    ms.reserve(frames);
    auto harvest = [&](int k) {
        if (pf[k] == UINT64_MAX) return;
        vkWaitForFences(c.dev, 1, &s[k].f, VK_TRUE, UINT64_MAX);
        uint64_t t[2];
        vkGetQueryPoolResults(c.dev, s[k].qp, 0, 2, sizeof(t), t, sizeof(uint64_t),
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        if (pf[k] >= warm) ms.push_back(double(t[1] - t[0]) * c.prop.limits.timestampPeriod / 1e6);
        pf[k] = UINT64_MAX;
    };
    auto start = std::chrono::steady_clock::now();
    auto per = std::chrono::duration<double>(1.0 / fps);
    for (uint64_t f = 0; f < total; f++) {
        int k = f % 3;
        harvest(k);
        vkResetFences(c.dev, 1, &s[k].f);
        vkResetCommandBuffer(s[k].cmd, 0);
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(s[k].cmd, &bi);
        vkCmdResetQueryPool(s[k].cmd, s[k].qp, 0, 2);
        vkCmdWriteTimestamp(s[k].cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, s[k].qp, 0);
        raw_preview::RawPreviewRecordInfo ri{};
        ri.commandBuffer = s[k].cmd;
        ri.inputRawR16UintView = s[k].raw.v;
        ri.outputLinearRgba16fView = s[k].out.v;
        ri.outputCfaStateR16UintView = s[k].state.v;
        ri.width = W;
        ri.height = H;
        ri.parameters = rp;
        if (!g_lsc.empty()) {
            ri.lensShadingMapWidth = g_lscW;
            ri.lensShadingMapHeight = g_lscH;
            ri.lensShadingMap = g_lsc.data();
            ri.lensShadingMapFloatCount = g_lsc.size();
        }
        pr.record(ri);
        vkCmdWriteTimestamp(s[k].cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, s[k].qp, 1);
        vkEndCommandBuffer(s[k].cmd);
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &s[k].cmd;
        vkQueueSubmit(c.q, 1, &si, s[k].f);
        pf[k] = f;
        std::this_thread::sleep_until(
            start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(per * double(f + 1)));
    }
    for (int k = 0; k < 3; k++) harvest(k);
    std::sort(ms.begin(), ms.end());
    double mean = std::accumulate(ms.begin(), ms.end(), 0.0) / ms.size();
    std::cout << (stress ? "HIGHLIGHT-STRESS" : "NORMAL") << " " << W << "x" << H << "  best " << ms.front() << " med "
              << pct(ms, .5) << " mean " << mean << " p95 " << pct(ms, .95) << " p99 " << pct(ms, .99) << " worst "
              << ms.back() << " ms  " << (pct(ms, .5) <= 1.0 ? "PASS" : "FAIL") << "\n";
    std::cout << "RAW_PREVIEW_MS stress=" << (stress ? 1 : 0) << " width=" << W << " height=" << H
              << " best=" << ms.front() << " median=" << pct(ms, .5) << " mean=" << mean << " p95=" << pct(ms, .95)
              << " p99=" << pct(ms, .99) << " worst=" << ms.back() << "\n";
    if (!g_dump.empty()) {
        // Dump the most recent frame's RGB output (bench-only equivalence check).
        int k = int((warm + frames - 1) % 3);
        VkDeviceSize n = VkDeviceSize(size_t(W / 2) * (H / 2) * 8);
        Buf db = mkBuf(c.pd, c.dev, n, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkResetFences(c.dev, 1, &s[k].f);
        vkResetCommandBuffer(s[k].cmd, 0);
        VkCommandBufferBeginInfo dbi{};
        dbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        dbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(s[k].cmd, &dbi);
        VkImageMemoryBarrier db0{};
        db0.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        db0.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        db0.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        db0.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        db0.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        db0.srcQueueFamilyIndex = db0.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        db0.image = s[k].out.i;
        db0.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(s[k].cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &db0);
        VkBufferImageCopy dcp{};
        dcp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        dcp.imageExtent = {W / 2, H / 2, 1};
        vkCmdCopyImageToBuffer(s[k].cmd, s[k].out.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, db.b, 1, &dcp);
        vkEndCommandBuffer(s[k].cmd);
        VkSubmitInfo dsi{};
        dsi.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        dsi.commandBufferCount = 1;
        dsi.pCommandBuffers = &s[k].cmd;
        vkQueueSubmit(c.q, 1, &dsi, s[k].f);
        vkWaitForFences(c.dev, 1, &s[k].f, VK_TRUE, UINT64_MAX);
        void* m = nullptr;
        ck(vkMapMemory(c.dev, db.m, 0, n, 0, &m), "dump map");
        std::ofstream f(g_dump, std::ios::binary | std::ios::trunc);
        f.write(static_cast<const char*>(m), static_cast<std::streamsize>(n));
        f.close();
        vkUnmapMemory(c.dev, db.m);
        delBuf(c.dev, db);
        if (!f) throw std::runtime_error("dump write failed");
        std::cout << "RAW_PREVIEW_DUMP path=" << g_dump << "\n";
    }
    for (int k = 0; k < 3; k++) {
        vkDestroyQueryPool(c.dev, s[k].qp, nullptr);
        vkDestroyFence(c.dev, s[k].f, nullptr);
        delImg(c.dev, s[k].state);
        delImg(c.dev, s[k].out);
        delImg(c.dev, s[k].raw);
        vkDestroyCommandPool(c.dev, s[k].pool, nullptr);
    }
}
int main(int argc, char** argv) {
    std::cout.setf(std::ios::unitbuf);
    std::string shader = "./raw_preview.comp.spv", maskShader = "./raw_preview_cfa_state.comp.spv";
    uint32_t frames = 600, W = 4080, H = 3064;
    double fps = 60;
    int stressMode = -1;
    uint32_t lscW = 0, lscH = 0;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--shader" && i + 1 < argc)
            shader = argv[++i];
        else if (a == "--mask-shader" && i + 1 < argc)
            maskShader = argv[++i];
        else if (a == "--lsc" && i + 1 < argc) {
            std::string v = argv[++i];
            auto x = v.find('x');
            lscW = std::stoul(v.substr(0, x));
            lscH = std::stoul(v.substr(x + 1));
        }
        else if (a == "--method" && i + 1 < argc)
            g_highlightMethod = std::stoul(argv[++i]);
        else if (a == "--dump" && i + 1 < argc)
            g_dump = argv[++i];
        else if (a == "--frames" && i + 1 < argc)
            frames = std::stoul(argv[++i]);
        else if (a == "--fps" && i + 1 < argc)
            fps = std::stod(argv[++i]);
        else if (a == "--width" && i + 1 < argc)
            W = std::stoul(argv[++i]);
        else if (a == "--height" && i + 1 < argc)
            H = std::stoul(argv[++i]);
        else if (a == "--stress-only")
            stressMode = 1;
        else if (a == "--normal-only")
            stressMode = 0;
    }
    try {
        Ctx c = ctx();
        if (lscW >= 2 && lscH >= 2) {
            makeSyntheticLsc(lscW, lscH);
            std::cout << "LSC synthetic map: " << lscW << "x" << lscH << "\n";
        }
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(c.pd, VK_FORMAT_R16_UINT, &fp);
        if ((fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) == 0)
            throw std::runtime_error("VK_FORMAT_R16_UINT storage image unsupported");
        std::cout << "============================================================\nRAW_PREVIEW 1.2.0 CFA STATE "
                     "BENCHMARK\n============================================================\nGPU                  : "
                  << c.prop.deviceName << "\nWorkgroup            : 8 x 8\nFrames               : " << frames
                  << " measured + 60 warmup / case\nCadence              : " << fps << " fps paced\n";
        if (stressMode <= 0) run(c, shader, maskShader, W, H, frames, fps, false);
        if (stressMode != 0) run(c, shader, maskShader, W, H, frames, fps, true);
        std::cout << "THREE_FRAMES_IN_FLIGHT_PASS slots=3\n";
        delCtx(c);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 2;
    }
}
