// Host still-pipeline benchmark (MoltenVK): the production native libraries in
// the app's order, timed per stage, with device-memory accounting.
//
//   clip pack + LSC -> AMaZE -> post-demosaic (WB/HL/distortion, defringe, FCC)
//   -> gain map -> readback -> UltraHDR fit + strip JPEG (base || map)
//
// Like the app, every shot builds its engines and images fresh, so "build" and
// the memory peak include allocation. The tone stage is not run: the SDR base
// fed to the gain map and the base JPEG is a cheap sRGB encode of the
// post-demosaic image (tone is not under test). See README.md.
#include <amaze/Amaze.hpp>
#include <gainmap/GainmapCompute.h>
#include <post_demosaic/PostDemosaicProcessor.h>
#include <turbojpeg.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "encoding/jpeg/StripJpegEncoder.h"
#include "encoding/jpeg/UltraHdrMapFit.h"
#include "gainmap.h"
#include "gainmap_blur.h"
#include "sensor_clip_pack.h"
#include "vk_test_common.hpp"

using namespace vktest;
using Clock = std::chrono::steady_clock;

// ---- Device-memory accounting: the libraries' vkAllocateMemory/vkFreeMemory
// calls resolve to these definitions at static link time.
namespace mem {
std::mutex mu;
std::map<VkDeviceMemory, VkDeviceSize> live;
VkDeviceSize current = 0, peak = 0;
void resetPeak() {
    std::lock_guard<std::mutex> l(mu);
    peak = current;
}
}  // namespace mem
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo* info,
                                                          const VkAllocationCallbacks* cb, VkDeviceMemory* out) {
    static auto real = reinterpret_cast<PFN_vkAllocateMemory>(vkGetDeviceProcAddr(device, "vkAllocateMemory"));
    const VkResult r = real(device, info, cb, out);
    if (r == VK_SUCCESS) {
        std::lock_guard<std::mutex> l(mem::mu);
        mem::live[*out] = info->allocationSize;
        mem::current += info->allocationSize;
        mem::peak = std::max(mem::peak, mem::current);
    }
    return r;
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice device, VkDeviceMemory m, const VkAllocationCallbacks* cb) {
    static auto real = reinterpret_cast<PFN_vkFreeMemory>(vkGetDeviceProcAddr(device, "vkFreeMemory"));
    if (m) {
        std::lock_guard<std::mutex> l(mem::mu);
        auto it = mem::live.find(m);
        if (it != mem::live.end()) {
            mem::current -= it->second;
            mem::live.erase(it);
        }
    }
    real(device, m, cb);
}

namespace {
double ms(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }
double mib(VkDeviceSize b) { return double(b) / (1024.0 * 1024.0); }
std::vector<uint8_t> readBytes(const std::string& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("Cannot open " + p);
    const auto n = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> b(static_cast<size_t>(n));
    f.read(reinterpret_cast<char*>(b.data()), n);
    return b;
}
amaze::ShaderProvider amazeShaders() {
    return [](std::string_view name) {
        const std::string n(name);
        const bool glsl = n.size() > 5 && n.compare(n.size() - 5, 5, ".comp") == 0;
        const std::string stem = n.substr(0, n.rfind('.'));
        // AMAZE_SPV_DIR (both kinds in one folder) for on-device runs.
        const char* dir = getenv("AMAZE_SPV_DIR");
        auto b = readBytes(std::string(dir ? dir : glsl ? AMAZE_GLSL_SPV_DIR : AMAZE_WGSL_SPV_DIR) + "/" + stem + ".spv");
        std::vector<uint32_t> w(b.size() / 4);
        std::memcpy(w.data(), b.data(), w.size() * 4);
        return w;
    };
}
VkImageMemoryBarrier imageBarrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src,
                                  VkAccessFlags dst) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return b;
}
void barrier(VkCommandBuffer cmd, const VkImageMemoryBarrier& b,
             VkPipelineStageFlags src = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
             VkPipelineStageFlags dst = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT) {
    vkCmdPipelineBarrier(cmd, src, dst, 0, 0, nullptr, 0, nullptr, 1, &b);
}

struct Options {
    std::string cfa;
    uint32_t width = 0, height = 0, pattern = 0;
    float black = 0, white = 0;
    std::array<float, 3> wb{2.0f, 1.0f, 1.6f};
    int reps = 3;
    uint32_t fccSteps = 2;
    float defringe = 1.0f;
    uint32_t hl = 0;  // 0 off, 1 colour propagation, 2 Inpaint Opposed
    bool distortion = false;
    bool lsc = true;
    bool uhdr = true;
    bool jpeg = true;
    bool fullResMap = true;
    bool captureColor = true;
    uint32_t amazeSlots = 16;
    bool persistent = false;  // keep engines across reps (app "Persistent Engine")
    std::string dumpPrefix;   // write final RGBA8 base + RGBA8 map for bit-identity checks
};

// Minimal compute pipeline for the clip-pack stage (owned by the app in production).
struct PackStage {
    VkDevice dev{};
    VkDescriptorSetLayout dsl{};
    VkPipelineLayout layout{};
    VkPipeline pipe{};
    VkDescriptorPool pool{};
    VkDescriptorSet set{};
    PackStage(VkDevice d) : dev(d) {
        VkDescriptorSetLayoutBinding b[4]{};
        for (uint32_t i = 0; i < 4; ++i) {
            b[i].binding = i;
            b[i].descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b[i].descriptorCount = 1;
            b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo dci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 4, b};
        ck(vkCreateDescriptorSetLayout(dev, &dci, nullptr, &dsl), "pack dsl");
        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 80};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &dsl, 1, &pr};
        ck(vkCreatePipelineLayout(dev, &pl, nullptr, &layout), "pack layout");
        VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, sensor_clip_pack_spv_size,
                                    reinterpret_cast<const uint32_t*>(sensor_clip_pack_spv)};
        VkShaderModule mod{};
        ck(vkCreateShaderModule(dev, &sm, nullptr, &mod), "pack module");
        VkComputePipelineCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod,
                    "main", nullptr};
        ci.layout = layout;
        ck(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, nullptr, &pipe), "pack pipeline");
        vkDestroyShaderModule(dev, mod, nullptr);
        VkDescriptorPoolSize ps[2]{{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 1, 2, ps};
        ck(vkCreateDescriptorPool(dev, &pci, nullptr, &pool), "pack pool");
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, pool, 1, &dsl};
        ck(vkAllocateDescriptorSets(dev, &ai, &set), "pack set");
    }
    ~PackStage() {
        vkDestroyDescriptorPool(dev, pool, nullptr);
        vkDestroyPipeline(dev, pipe, nullptr);
        vkDestroyPipelineLayout(dev, layout, nullptr);
        vkDestroyDescriptorSetLayout(dev, dsl, nullptr);
    }
};

struct Gpu {
    Ctx c;
    VkCommandPool pool{};
    VkQueryPool query{};
    VkQueryPool stageQuery{};
    float tsPeriodMs = 0;
    Gpu() {
        c = ctx();
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
                                    VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, c.qf};
        ck(vkCreateCommandPool(c.dev, &pci, nullptr, &pool), "pool");
        VkQueryPoolCreateInfo q{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, nullptr, 0, VK_QUERY_TYPE_TIMESTAMP, 18, 0};
        ck(vkCreateQueryPool(c.dev, &q, nullptr, &query), "post-demosaic query pool");
        q.queryCount = 2;
        ck(vkCreateQueryPool(c.dev, &q, nullptr, &stageQuery), "stage query pool");
        tsPeriodMs = c.prop.limits.timestampPeriod * 1e-6f;
    }
    ~Gpu() {
        vkDestroyQueryPool(c.dev, stageQuery, nullptr);
        vkDestroyQueryPool(c.dev, query, nullptr);
        vkDestroyCommandPool(c.dev, pool, nullptr);
        delCtx(c);
    }
    VkCommandBuffer begin() {
        VkCommandBuffer cmd{};
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool,
                                       VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        ck(vkAllocateCommandBuffers(c.dev, &ai, &cmd), "alloc cmd");
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                    VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
        ck(vkBeginCommandBuffer(cmd, &bi), "begin");
        return cmd;
    }
    void submit(VkCommandBuffer cmd) {
        ck(vkEndCommandBuffer(cmd), "end");
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        ck(vkQueueSubmit(c.q, 1, &si, VK_NULL_HANDLE), "submit");
        ck(vkQueueWaitIdle(c.q), "wait");
        vkFreeCommandBuffers(c.dev, pool, 1, &cmd);
    }
    // One timed GPU stage in its own submission: returns GPU ms (timestamps).
    template <class F>
    double timed(F&& record) {
        VkCommandBuffer cmd = begin();
        vkCmdResetQueryPool(cmd, stageQuery, 0, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, stageQuery, 0);
        cmd = record(cmd);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, stageQuery, 1);
        submit(cmd);
        uint64_t t[2]{};
        ck(vkGetQueryPoolResults(c.dev, stageQuery, 0, 2, sizeof(t), t, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
           "stage query");
        return double(t[1] - t[0]) * tsPeriodMs;
    }
};

struct Row {
    std::map<std::string, double> v;
};

uint16_t toHalf(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    int32_t e = int32_t((x >> 23) & 0xffu) - 112;
    uint32_t m = x & 0x7fffffu;
    if (e <= 0) return uint16_t(sign);
    if (e >= 31) return uint16_t(sign | 0x7c00u);
    return uint16_t(sign | (uint32_t(e) << 10) | (m >> 13));
}

Row runShot(Gpu& g, const Options& o, const std::vector<uint16_t>& raw, bool dump) {
    Row row;
    auto& R = row.v;
    VkDevice dev = g.c.dev;
    VkPhysicalDevice pd = g.c.pd;
    const uint32_t W = o.width, H = o.height;
    mem::resetPeak();
    const VkDeviceSize memBase = mem::current;
    auto t0 = Clock::now();

    // ---------------- Demosaic phase (SharedHighlightRuntime + AMaZE) ----------------
    auto tb = Clock::now();
    Img input = mkImg(pd, dev, W, H, VK_FORMAT_R16_UINT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    Img packed = mkImg(pd, dev, W / 2, H / 2, VK_FORMAT_R16G16B16A16_SFLOAT,
                       VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    Img clip = mkImg(pd, dev, W / 2, H / 2, VK_FORMAT_R16_UINT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    Img demosaiced = mkImg(pd, dev, W, H, VK_FORMAT_R16G16B16A16_SFLOAT,
                           VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    const VkDeviceSize rawBytes = VkDeviceSize(W) * H * 2;
    Buf upload = mkBuf(pd, dev, rawBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    // Camera2-style LSC grid (17x13, vec4 per point): smooth radial vignette.
    const uint32_t lw = 17, lh = 13;
    std::vector<float> lsc(size_t(lw) * lh * 4);
    for (uint32_t y = 0; y < lh; ++y)
        for (uint32_t x = 0; x < lw; ++x) {
            const float dx = (x / float(lw - 1) - 0.5f) * 2, dy = (y / float(lh - 1) - 0.5f) * 2;
            for (int c = 0; c < 4; ++c) lsc[(size_t(y) * lw + x) * 4 + c] = 1.0f + 0.9f * (dx * dx + dy * dy) / 2;
        }
    Buf lscBuf = mkBuf(pd, dev, lsc.size() * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    void* m = nullptr;
    ck(vkMapMemory(dev, lscBuf.m, 0, VK_WHOLE_SIZE, 0, &m), "map lsc");
    std::memcpy(m, lsc.data(), lsc.size() * 4);
    vkUnmapMemory(dev, lscBuf.m);
    PackStage pack(dev);
    amaze::PipelineConfig acfg{};
    acfg.width = W;
    acfg.height = H;
    acfg.pattern = amaze::BayerPattern(o.pattern);
    acfg.inputMode = amaze::InputMode::PackedCfaRgba16fImage;
    acfg.tileSlots = o.amazeSlots;
    amaze::VulkanContext avc{};
    avc.physicalDevice = pd;
    avc.device = dev;
    avc.queueFamilyIndex = g.c.qf;
    auto amazePipe = std::make_unique<amaze::AmazePipeline>(avc, amazeShaders(), acfg);
    R["demosaic.build"] = ms(tb, Clock::now());

    auto tu = Clock::now();
    ck(vkMapMemory(dev, upload.m, 0, rawBytes, 0, &m), "map upload");
    std::memcpy(m, raw.data(), rawBytes);
    vkUnmapMemory(dev, upload.m);
    R["upload.cpu"] = ms(tu, Clock::now());

    R["pack_lsc.gpu"] = g.timed([&](VkCommandBuffer cmd) {
        barrier(cmd, imageBarrier(input.i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                                  VK_ACCESS_TRANSFER_WRITE_BIT));
        VkBufferImageCopy bc{};
        bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bc.imageExtent = {W, H, 1};
        vkCmdCopyBufferToImage(cmd, upload.b, input.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
        barrier(cmd, imageBarrier(input.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT));
        for (VkImage i : {packed.i, clip.i, demosaiced.i})
            barrier(cmd, imageBarrier(i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
                                      VK_ACCESS_SHADER_WRITE_BIT));
        VkDescriptorImageInfo ii[3]{{VK_NULL_HANDLE, input.v, VK_IMAGE_LAYOUT_GENERAL},
                                    {VK_NULL_HANDLE, packed.v, VK_IMAGE_LAYOUT_GENERAL},
                                    {VK_NULL_HANDLE, clip.v, VK_IMAGE_LAYOUT_GENERAL}};
        VkDescriptorBufferInfo bi{lscBuf.b, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w[4]{};
        for (uint32_t i = 0; i < 4; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = pack.set;
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            if (i < 3) w[i].pImageInfo = &ii[i];
            else w[i].pBufferInfo = &bi;
        }
        vkUpdateDescriptorSets(dev, 4, w, 0, nullptr);
        struct {
            float black[4], invRange[4], wb[4];
            float clipThreshold;
            uint32_t width, height, pattern, lscEnabled, lscWidth, lscHeight, pad;
        } pc{};
        for (int c = 0; c < 4; ++c) {
            pc.black[c] = o.black;
            pc.invRange[c] = 1.0f / (o.white - o.black);
            pc.wb[c] = 1.0f;
        }
        pc.clipThreshold = 0.995f;
        pc.width = W;
        pc.height = H;
        pc.pattern = o.pattern;
        pc.lscEnabled = o.lsc ? 1u : 0u;
        pc.lscWidth = lw;
        pc.lscHeight = lh;
        static_assert(sizeof(pc) == 80);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pack.pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pack.layout, 0, 1, &pack.set, 0, nullptr);
        vkCmdPushConstants(cmd, pack.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cmd, (W / 2 + 7) / 8, (H / 2 + 7) / 8, 1);
        barrier(cmd, imageBarrier(packed.i, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT,
                                  VK_ACCESS_SHADER_READ_BIT));
        return cmd;
    });

    R["amaze.gpu"] = g.timed([&](VkCommandBuffer cmd) {
        amaze::PackedCfaImageView pin{packed.i, packed.v, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_LAYOUT_GENERAL,
                                      W / 2, H / 2, W, H, acfg.pattern};
        amaze::LinearRgbImage pout{demosaiced.i, demosaiced.v, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_LAYOUT_GENERAL,
                                   W, H};
        amazePipe->record(cmd, pin, pout, o.wb);
        return cmd;
    });
    R["mem.demosaic_peak"] = mib(mem::peak - memBase);
    // releaseWorkspaceKeepOutput: everything but the demosaiced output and clip state.
    amazePipe.reset();
    delImg(dev, input);
    delImg(dev, packed);
    delBuf(dev, upload);

    // ---------------- Render phase ----------------
    auto tr = Clock::now();
    static std::unique_ptr<rawr::post::PostDemosaicProcessor> persistentPost;
    std::unique_ptr<rawr::post::PostDemosaicProcessor> freshPost;
    rawr::post::PostDemosaicProcessor* post = nullptr;
    if (o.persistent && persistentPost) {
        post = persistentPost.get();
    } else {
        freshPost = std::make_unique<rawr::post::PostDemosaicProcessor>(pd, dev, g.c.qf, W, H, o.fccSteps, true, 0.0f,
                                                                        0.0f, o.defringe, 0.02f, 0.08f);
        post = freshPost.get();
        if (o.persistent) {
            persistentPost = std::move(freshPost);
            post = persistentPost.get();
        }
    }
    R["render.build"] = ms(tr, Clock::now());

    // Post-demosaic with the app's timestamp layout (0-1 colour, 1-3 HL, 4-5 FCC, 16-17 defringe+FCC tail).
    const float camToSrgb[9] = {1.7f, -0.55f, -0.15f, -0.25f, 1.45f, -0.2f, 0.05f, -0.5f, 1.45f};
    rawr::post::StillDistortionCorrection dist{};
    if (o.distortion) {
        dist.enabled = true;
        const float f = 0.8f * float(std::max(W, H));
        dist.intrinsic = {f, f, W / 2.0f, H / 2.0f, 0.0f};
        dist.distortion = {-0.08f, 0.02f, 0.0f, 0.0005f, -0.0003f};
    }
    auto tp = Clock::now();
    {
        VkCommandBuffer cmd = g.begin();
        vkCmdResetQueryPool(cmd, g.query, 0, 18);
        post->record(cmd, demosaiced.i, demosaiced.v, clip.i, clip.v, o.wb, o.hl == 1, dist, g.query, false,
                     o.hl == 2 ? 1u : 0u, 0.99f, 0.5f, 1.0f, rawr::shading::LensShadingMapView{}, o.pattern,
                     rawr::post::DenoiseRequest{}, {}, camToSrgb);
        for (uint32_t q : {6u, 7u, 8u, 9u, 12u, 13u, 14u, 15u})
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g.query, q);
        g.submit(cmd);
        uint64_t t[18]{};
        ck(vkGetQueryPoolResults(dev, g.query, 0, 18, sizeof(t), t, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
           "post query");
        auto d = [&](int a, int b) { return double(t[b] - t[a]) * g.tsPeriodMs; };
        R["wb_hl_dist.gpu"] = d(0, 1);
        R["hl_prop.gpu"] = d(1, 3);
        R["fcc.gpu"] = d(4, 5);
        R["defringe.gpu"] = d(16, 17) - d(4, 5);
    }
    R["post.wall"] = ms(tp, Clock::now());

    // App order: post-demosaic's spare images go before the output-side buffers exist.
    auto tx = Clock::now();
#ifdef BENCH_RELEASE_SCRATCH
    if (!o.persistent && !getenv("BENCH_KEEP_SCRATCH")) post->releaseScratch();
#endif
    Img sdr = mkImg(pd, dev, W, H, VK_FORMAT_R8G8B8A8_UNORM,
                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    const VkDeviceSize sdrBytes = VkDeviceSize(W) * H * 4;
    Buf sdrRead = mkBuf(pd, dev, sdrBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    const uint32_t mapW = o.fullResMap ? W : W / 2, mapH = o.fullResMap ? H : H / 2;
    const uint32_t mapStoreH = mapH * (o.captureColor ? 2u : 1u);
    Img map{};
    Buf mapRead{};
    std::unique_ptr<gainmap::GainmapCompute> gm;
    if (o.uhdr) {
        map = mkImg(pd, dev, mapW, mapStoreH, VK_FORMAT_R8G8B8A8_UNORM,
                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        mapRead = mkBuf(pd, dev, VkDeviceSize(mapW) * mapStoreH * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        gainmap::GainmapCreateInfo gci{};
        gci.context = {pd, dev, nullptr};
        gci.shaderSpirv = reinterpret_cast<const uint32_t*>(gainmap_spv);
        gci.shaderSpirvBytes = gainmap_spv_size;
        gci.blurShaderSpirv = reinterpret_cast<const uint32_t*>(gainmap_blur_spv);
        gci.blurShaderSpirvBytes = gainmap_blur_spv_size;
        gm = std::make_unique<gainmap::GainmapCompute>(gci);
    }
    R["pixels.build"] = ms(tx, Clock::now());

    // SDR base stand-in: format-converting blit of the post-demosaic output, so
    // the JPEG encoders see real scene texture (tone is not under test).
    R["sdr_standin.gpu"] = g.timed([&](VkCommandBuffer cmd) {
        barrier(cmd, imageBarrier(sdr.i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                                  VK_ACCESS_TRANSFER_WRITE_BIT));
        barrier(cmd, imageBarrier(post->outputImage(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                                  VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT));
        VkImageBlit blit{};
        blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = blit.dstOffsets[1] = {int32_t(W), int32_t(H), 1};
        vkCmdBlitImage(cmd, post->outputImage(), VK_IMAGE_LAYOUT_GENERAL, sdr.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit, VK_FILTER_NEAREST);
        barrier(cmd, imageBarrier(sdr.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT));
        return cmd;
    });

    if (o.uhdr) {
        R["gainmap.gpu"] = g.timed([&](VkCommandBuffer cmd) {
            barrier(cmd, imageBarrier(map.i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
                                      VK_ACCESS_SHADER_WRITE_BIT));
            gainmap::GainmapRecordInfo gri{};
            gri.commandBuffer = cmd;
            gri.hdr = {post->outputView(), VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_LAYOUT_GENERAL, W, H};
            gri.sdr = {sdr.v, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_GENERAL, W, H};
            gri.map = {map.v, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_GENERAL, mapW, mapH};
            gri.clip = {clip.v, VK_FORMAT_R16_UINT, VK_IMAGE_LAYOUT_GENERAL, mapW, mapH};
            gri.params.fullResMap = o.fullResMap;
            gri.params.writeHdrChromaticity = o.captureColor;
            gri.params.hdrExposure = 2.0f;
            gm->record(gri);
            return cmd;
        });
    }
    R["readback.gpu"] = g.timed([&](VkCommandBuffer cmd) {
        barrier(cmd, imageBarrier(sdr.i, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT));
        VkBufferImageCopy bic{};
        bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bic.imageExtent = {W, H, 1};
        vkCmdCopyImageToBuffer(cmd, sdr.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sdrRead.b, 1, &bic);
        if (o.uhdr) {
            barrier(cmd, imageBarrier(map.i, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                      VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT));
            bic.imageExtent = {mapW, mapStoreH, 1};
            vkCmdCopyImageToBuffer(cmd, map.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mapRead.b, 1, &bic);
        }
        return cmd;
    });
    R["mem.render_peak"] = mib(mem::peak - memBase);

    // ---------------- CPU: readback telemetry + UltraHDR JPEG ----------------
    void* sdrPix = nullptr;
    void* mapPix = nullptr;
    ck(vkMapMemory(dev, sdrRead.m, 0, VK_WHOLE_SIZE, 0, &sdrPix), "map sdr");
    VkMappedMemoryRange ranges[2]{{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, sdrRead.m, 0, VK_WHOLE_SIZE}};
    uint32_t nRanges = 1;
    if (o.uhdr) {
        ck(vkMapMemory(dev, mapRead.m, 0, VK_WHOLE_SIZE, 0, &mapPix), "map map");
        ranges[nRanges++] = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, mapRead.m, 0, VK_WHOLE_SIZE};
    }
    auto ti = Clock::now();
    vkInvalidateMappedMemoryRanges(dev, nRanges, ranges);
    R["invalidate.cpu"] = ms(ti, Clock::now());
    if (o.uhdr) {
        // RenderReadback's gain-map telemetry loop (single thread, R channel of every texel).
        auto ts = Clock::now();
        const auto* mb = static_cast<const uint8_t*>(mapPix);
        const uint64_t n = uint64_t(mapW) * mapH;
        uint8_t peakV = 0;
        uint64_t boosted = 0, sum = 0;
        const float boostNorm = 0.5f;
        for (uint64_t i = 0; i < n; ++i) {
            const uint8_t v = mb[i * 4];
            peakV = std::max<uint8_t>(peakV, v);
            sum += v;
            if (static_cast<float>(v) / 255.0f >= boostNorm * 0.9f) ++boosted;
        }
        R["map_stats.cpu"] = ms(ts, Clock::now());
        if (peakV == 255 && sum == 0) std::printf("#");  // keep the loop
    }
    if (o.jpeg) {
        using namespace rawrcam::encoding::jpeg;
        const auto* pixels = static_cast<const unsigned char*>(sdrPix);
        auto tj = Clock::now();
        unsigned char* baseBuf = nullptr;
        size_t baseSize = 0;
        double baseMs = 0;
        std::thread baseWorker([&] {
            int strips = 0;
            std::string err;
            if (!encodeStripsRgba8(pixels, int(W), int(H), 98, TJSAMP_420, &baseBuf, &baseSize, &strips, &err))
                throw std::runtime_error("base encode: " + err);
            baseMs = ms(tj, Clock::now());
        });
        if (o.uhdr) {
            rawrcam::color::UltraHdrParams uh{};
            uh.enabled = true;
            std::vector<unsigned char> fitted;
            float fittedMax = uh.gainMapMaxLog2;
            const auto* mp = static_cast<const unsigned char*>(mapPix);
            if (dump) {  // the writer's exact fit inputs: GPU map (+ chromaticity plane) and SDR base
                std::ofstream(o.dumpPrefix + "_fitin_map.rgba", std::ios::binary)
                    .write(reinterpret_cast<const char*>(mp), std::streamsize(size_t(mapW) * mapStoreH * 4));
                std::ofstream(o.dumpPrefix + "_fitin_base.rgba", std::ios::binary)
                    .write(reinterpret_cast<const char*>(pixels), std::streamsize(size_t(W) * H * 4));
            }
            auto tf = Clock::now();
            const unsigned char* enc = mp;
            if (fitAndRemapMap(mp, mapW, mapH, uh, fitted, fittedMax, pixels, W, H)) {
                enc = fitted.data();
                R["map_fit_only.cpu"] = ms(tf, Clock::now());
                if (o.captureColor) {
                    float lo = uh.gainMapMinLog2, hi = uh.gainMapMaxLog2;
                    restoreHighlightColor(fitted, mp + size_t(mapW) * mapH * 4, mapW, mapH, pixels, W, H, uh,
                                          fittedMax, lo, hi);
                }
            }
            R["map_fit.cpu"] = ms(tf, Clock::now());
            auto te = Clock::now();
            unsigned char* mapBuf = nullptr;
            size_t mapSize = 0;
            int strips = 0;
            std::string err;
            if (!encodeStripsRgba8(enc, int(mapW), int(mapH), uh.gainmapQuality, TJSAMP_420, &mapBuf, &mapSize,
                                   &strips, &err))
                throw std::runtime_error("map encode: " + err);
            R["map_encode.cpu"] = ms(te, Clock::now());
            R["map_jpeg.MB"] = double(mapSize) / 1e6;
            if (dump) {
                std::ofstream(o.dumpPrefix + "_map.rgba", std::ios::binary)
                    .write(reinterpret_cast<const char*>(enc), std::streamsize(size_t(mapW) * mapH * 4));
            }
            tj3Free(mapBuf);
        }
        baseWorker.join();
        R["base_encode.cpu"] = baseMs;
        R["jpeg.wall"] = ms(tj, Clock::now());
        R["base_jpeg.MB"] = double(baseSize) / 1e6;
        tj3Free(baseBuf);
    }
    if (dump) {
        // Post-demosaic output (RGBA16F) for bit-identity checks across changes.
        Buf dl = mkBuf(pd, dev, VkDeviceSize(W) * H * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkCommandBuffer cmd = g.begin();
        barrier(cmd, imageBarrier(post->outputImage(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                                  VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT));
        VkBufferImageCopy bic{};
        bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bic.imageExtent = {W, H, 1};
        vkCmdCopyImageToBuffer(cmd, post->outputImage(), VK_IMAGE_LAYOUT_GENERAL, dl.b, 1, &bic);
        g.submit(cmd);
        void* p = nullptr;
        ck(vkMapMemory(dev, dl.m, 0, VK_WHOLE_SIZE, 0, &p), "map dump");
        std::ofstream(o.dumpPrefix + "_post.rgba16f", std::ios::binary)
            .write(static_cast<const char*>(p), std::streamsize(size_t(W) * H * 8));
        vkUnmapMemory(dev, dl.m);
        delBuf(dev, dl);
    }
    vkUnmapMemory(dev, sdrRead.m);
    if (o.uhdr) vkUnmapMemory(dev, mapRead.m);
    R["mem.final_peak"] = mib(mem::peak - memBase);

    gm.reset();
    freshPost.reset();
    delImg(dev, sdr);
    delBuf(dev, sdrRead);
    if (o.uhdr) {
        delImg(dev, map);
        delBuf(dev, mapRead);
    }
    delImg(dev, clip);
    delImg(dev, demosaiced);
    delBuf(dev, lscBuf);
    R["total.wall"] = ms(t0, Clock::now());
    return row;
}
}  // namespace

int main(int argc, char** argv) {
    try {
        Options o;
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            auto next = [&] {
                if (i + 1 >= argc) throw std::runtime_error("missing value for " + a);
                return std::string(argv[++i]);
            };
            if (a == "--cfa") o.cfa = next();
            else if (a == "--width") o.width = uint32_t(std::stoul(next()));
            else if (a == "--height") o.height = uint32_t(std::stoul(next()));
            else if (a == "--pattern") o.pattern = uint32_t(std::stoul(next()));
            else if (a == "--black") o.black = std::stof(next());
            else if (a == "--white") o.white = std::stof(next());
            else if (a == "--wb") for (auto& g : o.wb) g = std::stof(next());
            else if (a == "--reps") o.reps = std::stoi(next());
            else if (a == "--fcc") o.fccSteps = uint32_t(std::stoul(next()));
            else if (a == "--defringe") o.defringe = std::stof(next());
            else if (a == "--hl") o.hl = uint32_t(std::stoul(next()));
            else if (a == "--distortion") o.distortion = std::stoi(next()) != 0;
            else if (a == "--lsc") o.lsc = std::stoi(next()) != 0;
            else if (a == "--uhdr") o.uhdr = std::stoi(next()) != 0;
            else if (a == "--jpeg") o.jpeg = std::stoi(next()) != 0;
            else if (a == "--full-res-map") o.fullResMap = std::stoi(next()) != 0;
            else if (a == "--capture-color") o.captureColor = std::stoi(next()) != 0;
            else if (a == "--slots") o.amazeSlots = uint32_t(std::stoul(next()));
            else if (a == "--persistent") o.persistent = std::stoi(next()) != 0;
            else if (a == "--dump") o.dumpPrefix = next();
            else throw std::runtime_error("unknown argument " + a);
        }
        if (o.cfa.empty() || !o.width || !o.height || !(o.white > o.black))
            throw std::runtime_error("usage: still_perf_bench --cfa raw.u16 --width W --height H --black B --white W "
                                     "[--wb r g b] [--reps N] [--fcc N] [--defringe S] [--hl 0|1|2] [--distortion 0|1] "
                                     "[--uhdr 0|1] [--jpeg 0|1] [--full-res-map 0|1] [--capture-color 0|1] "
                                     "[--slots N] [--persistent 0|1] [--dump PREFIX]");
        auto bytes = readBytes(o.cfa);
        if (bytes.size() != size_t(o.width) * o.height * 2) throw std::runtime_error("CFA size mismatch");
        std::vector<uint16_t> raw(bytes.size() / 2);
        std::memcpy(raw.data(), bytes.data(), bytes.size());
        bytes.clear();
        bytes.shrink_to_fit();
        Gpu g;
        std::printf("device=%s %ux%u reps=%d\n", g.c.prop.deviceName, o.width, o.height, o.reps);
        std::vector<Row> rows;
        for (int r = 0; r < o.reps + 1; ++r) {  // rep 0 warms pipelines/driver caches and is dropped
            rows.push_back(runShot(g, o, raw, !o.dumpPrefix.empty() && r == o.reps));
        }
        std::map<std::string, std::vector<double>> agg;
        for (size_t r = 1; r < rows.size(); ++r)
            for (auto& [k, v] : rows[r].v) agg[k].push_back(v);
        for (auto& [k, v] : agg) {
            std::sort(v.begin(), v.end());
            std::printf("%-20s %9.2f  (min %.2f max %.2f)\n", k.c_str(), v[v.size() / 2], v.front(), v.back());
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "still_perf_bench: %s\n", e.what());
        return 1;
    }
}
