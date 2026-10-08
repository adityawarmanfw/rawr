#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <rcd/Rcd.hpp>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../../../vk_common/testing/vk_test_common.hpp"
using namespace vktest;

static std::vector<uint8_t> readBytes(const std::string& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("Cannot open " + p);
    const auto n = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> b(static_cast<size_t>(n), uint8_t{});
    if (n > 0) f.read(reinterpret_cast<char*>(b.data()), n);
    return b;
}
static rcd::ShaderProvider provider(std::string dir) {
    return [dir = std::move(dir)](std::string_view n) {
        auto b = readBytes(dir + "/" + std::string(n) + ".spv");
        if (b.size() % 4) throw std::runtime_error("Bad SPIR-V size");
        std::vector<uint32_t> w(b.size() / 4);
        std::memcpy(w.data(), b.data(), b.size());
        return w;
    };
}
static void transition(VkCommandBuffer cmd, VkImage image) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
}
struct Stats {
    double best{}, median{}, p95{}, p99{}, mean{}, worst{};
};
static double percentile(const std::vector<double>& s, double q) {
    if (s.empty()) return 0.0;
    const double pos = q * double(s.size() - 1);
    const size_t i = size_t(pos);
    const double f = pos - double(i);
    return i + 1 < s.size() ? s[i] * (1.0 - f) + s[i + 1] * f : s[i];
}
static Stats stats(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    Stats s{};
    s.best = v.front();
    s.median = percentile(v, .5);
    s.p95 = percentile(v, .95);
    s.p99 = percentile(v, .99);
    s.mean = std::accumulate(v.begin(), v.end(), 0.0) / double(v.size());
    s.worst = v.back();
    return s;
}
static void printStats(const char* name, const Stats& s) {
    std::cout << "RCD_BENCH_STAGE name=" << name << " best_ms=" << s.best << " median_ms=" << s.median
              << " p95_ms=" << s.p95 << " p99_ms=" << s.p99 << " mean_ms=" << s.mean << " worst_ms=" << s.worst << "\n";
}
static std::pair<uint32_t, uint32_t> parsePair(const std::string& s) {
    const auto pos = s.find('x');
    if (pos == std::string::npos) throw std::runtime_error("workgroup must be XxY");
    const uint32_t x = uint32_t(std::stoul(s.substr(0, pos))), y = uint32_t(std::stoul(s.substr(pos + 1)));
    if (!x || !y) throw std::runtime_error("workgroup dimensions must be non-zero");
    return {x, y};
}
int main(int argc, char** argv) {
    try {
        std::string sd, in;
        bool autoBalance=false;
        uint32_t W = 0, H = 0, warmup = 20, iters = 100, wgx = 16, wgy = 16;
        std::pair<uint32_t, uint32_t> wgDir{0, 0}, wgGreen{0, 0}, wgDiag{0, 0}, wgSites{0, 0}, wgExport{0, 0};
        for (int i = 1; i < argc; i++) {
            std::string a = argv[i];
            if (a == "--auto-balance") autoBalance=true;
            else if (a == "--shader-dir")
                sd = argv[++i];
            else if (a == "--input-f32")
                in = argv[++i];
            else if (a == "--width")
                W = uint32_t(std::stoul(argv[++i]));
            else if (a == "--height")
                H = uint32_t(std::stoul(argv[++i]));
            else if (a == "--warmup")
                warmup = uint32_t(std::stoul(argv[++i]));
            else if (a == "--iterations")
                iters = uint32_t(std::stoul(argv[++i]));
            else if (a == "--wg-x")
                wgx = uint32_t(std::stoul(argv[++i]));
            else if (a == "--wg-y")
                wgy = uint32_t(std::stoul(argv[++i]));
            else if (a == "--wg-direction")
                wgDir = parsePair(argv[++i]);
            else if (a == "--wg-green")
                wgGreen = parsePair(argv[++i]);
            else if (a == "--wg-diagonal")
                wgDiag = parsePair(argv[++i]);
            else if (a == "--wg-green-sites")
                wgSites = parsePair(argv[++i]);
            else if (a == "--wg-export")
                wgExport = parsePair(argv[++i]);
        }
        if (sd.empty() || in.empty() || !W || !H || !iters)
            throw std::runtime_error(
                "usage: rcd_benchmark --shader-dir DIR --input-f32 FILE --width W --height H [--warmup N] "
                "[--iterations N] [--auto-balance] [--wg-x X --wg-y Y] [--wg-direction XxY --wg-green XxY --wg-diagonal XxY "
                "--wg-green-sites XxY --wg-export XxY]");
        auto bytes = readBytes(in);
        if (bytes.size() != size_t(W) * H * 4) throw std::runtime_error("input size mismatch");
        Ctx c = ctx();
        std::cout << "Using GPU: " << c.prop.deviceName << "\n";
        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.queueFamilyIndex = c.qf;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VkCommandPool pool{};
        ck(vkCreateCommandPool(c.dev, &pci, nullptr, &pool), "pool");
        Buf ib = mkBuf(c.pd, c.dev, bytes.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        void* m = nullptr;
        ck(vkMapMemory(c.dev, ib.m, 0, bytes.size(), 0, &m), "map in");
        std::memcpy(m, bytes.data(), bytes.size());
        vkUnmapMemory(c.dev, ib.m);
        Img oi = mkImg(c.pd, c.dev, W, H, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT);
        rcd::PipelineConfig cfg{};
        cfg.autoBalance=autoBalance;
        cfg.width = W;
        cfg.height = H;
        cfg.pattern = rcd::BayerPattern::RGGB;
        cfg.inputMode = rcd::InputMode::NormalizedFloatBuffer;
        cfg.telemetry = true;
        cfg.workgroupX = wgx;
        cfg.workgroupY = wgy;
        if (wgDir.first) {
            cfg.directionWorkgroupX = wgDir.first;
            cfg.directionWorkgroupY = wgDir.second;
        }
        if (wgGreen.first) {
            cfg.greenWorkgroupX = wgGreen.first;
            cfg.greenWorkgroupY = wgGreen.second;
        }
        if (wgDiag.first) {
            cfg.diagonalWorkgroupX = wgDiag.first;
            cfg.diagonalWorkgroupY = wgDiag.second;
        }
        if (wgSites.first) {
            cfg.greenSitesWorkgroupX = wgSites.first;
            cfg.greenSitesWorkgroupY = wgSites.second;
        }
        if (wgExport.first) {
            cfg.exportWorkgroupX = wgExport.first;
            cfg.exportWorkgroupY = wgExport.second;
        }
        auto pipe = std::make_unique<rcd::RcdPipeline>(rcd::VulkanContext{c.pd, c.dev, c.qf, nullptr}, provider(sd),
                                                       cfg, rcd::PipelineAssets{});
        VkCommandBuffer cmd{};
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        ck(vkAllocateCommandBuffers(c.dev, &cai, &cmd), "cmd");
        // One-time output transition outside benchmark.
        {
            VkCommandBufferBeginInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            ck(vkBeginCommandBuffer(cmd, &bi), "begin init");
            transition(cmd, oi.i);
            ck(vkEndCommandBuffer(cmd), "end init");
            VkSubmitInfo si{};
            si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cmd;
            ck(vkQueueSubmit(c.q, 1, &si, VK_NULL_HANDLE), "submit init");
            ck(vkQueueWaitIdle(c.q), "wait init");
            ck(vkResetCommandBuffer(cmd, 0), "reset init");
        }
        rcd::NormalizedBayerBufferView iv{{ib.b, 0, bytes.size()}, W, H, rcd::BayerPattern::RGGB};
        rcd::LinearRgbImage ov{oi.i, oi.v, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_LAYOUT_GENERAL, W, H};
        std::array<std::vector<double>, 6> samples;
        std::vector<double> cpuSubmitMs;
        for (auto& v : samples) v.reserve(iters);
        cpuSubmitMs.reserve(iters);
        const char* names[6] = {"rcd.direction",   "rcd.green",  "rcd.diagonal",
                                "rcd.green_sites", "rcd.export", "rcd.total"};
        const uint32_t total = warmup + iters;
        for (uint32_t n = 0; n < total; ++n) {
            VkCommandBufferBeginInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            ck(vkBeginCommandBuffer(cmd, &bi), "begin bench");
            pipe->record(cmd, iv, ov);
            ck(vkEndCommandBuffer(cmd), "end bench");
            VkSubmitInfo si{};
            si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cmd;
            const auto c0 = std::chrono::steady_clock::now();
            ck(vkQueueSubmit(c.q, 1, &si, VK_NULL_HANDLE), "submit bench");
            ck(vkQueueWaitIdle(c.q), "wait bench");
            const auto c1 = std::chrono::steady_clock::now();
            rcd::FrameTelemetry tel{};
            if (!pipe->collectTelemetry(tel)) throw std::runtime_error("timestamp query collection failed");
            if (n >= warmup) {
                for (size_t k = 0; k < 6; ++k) {
                    auto it = std::find_if(tel.gpuEvents.begin(), tel.gpuEvents.end(),
                                           [&](const auto& e) { return e.name == names[k]; });
                    if (it == tel.gpuEvents.end()) throw std::runtime_error("missing telemetry event");
                    samples[k].push_back(it->milliseconds);
                }
                cpuSubmitMs.push_back(std::chrono::duration<double, std::milli>(c1 - c0).count());
            }
            ck(vkResetCommandBuffer(cmd, 0), "reset bench");
        }
        std::cout << std::fixed << std::setprecision(6);
        const std::pair<uint32_t, uint32_t> rd{cfg.directionWorkgroupX, cfg.directionWorkgroupY};
        const std::pair<uint32_t, uint32_t> rg{cfg.greenWorkgroupX, cfg.greenWorkgroupY};
        const std::pair<uint32_t, uint32_t> rq{cfg.diagonalWorkgroupX, cfg.diagonalWorkgroupY};
        const std::pair<uint32_t, uint32_t> rs{cfg.greenSitesWorkgroupX, cfg.greenSitesWorkgroupY};
        const std::pair<uint32_t, uint32_t> re{cfg.exportWorkgroupX, cfg.exportWorkgroupY};
        std::cout << "RCD_BENCHMARK_BEGIN width=" << W << " height=" << H << " warmup=" << warmup
                  << " iterations=" << iters << " input=NormalizedFloatBuffer_LINEAR255"
                  << " wg_direction=" << rd.first << "x" << rd.second << " wg_green=" << rg.first << "x" << rg.second
                  << " wg_diagonal=" << rq.first << "x" << rq.second << " wg_green_sites=" << rs.first << "x"
                  << rs.second << " wg_export=" << re.first << "x" << re.second << "\n";
        for (size_t k = 0; k < 6; ++k) printStats(names[k], stats(samples[k]));
        printStats("cpu_submit_wait", stats(cpuSubmitMs));
        std::cout << "RCD_BENCHMARK_PASS width=" << W << " height=" << H << " iterations=" << iters << "\n";
        pipe.reset();
        vkFreeCommandBuffers(c.dev, pool, 1, &cmd);
        delImg(c.dev, oi);
        delBuf(c.dev, ib);
        vkDestroyCommandPool(c.dev, pool, nullptr);
        delCtx(c);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "RCD_BENCHMARK_FAIL: " << e.what() << "\n";
        return 1;
    }
}
