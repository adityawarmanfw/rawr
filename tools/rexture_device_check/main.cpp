// On-device (and desktop) check of the Rexture merge: runs a burst in the replay tool's `rawburst:` layout through the
// production coordinator, optionally dumps every stage for the NumPy comparison (tmp/indep/m7/compare.py), or times the merge
// against HDR+ Quality. No camera, no app: an NDK executable pushed with adb.
//   rexture_device_check DIR [--merge rexture|hdrplus-freq] [--frames N] [--noise-ab A,B] [--dump-stages OUT]
//                        [--inject-flow DUMPDIR] [--repeat K] [--scores]
// --frames N cycles the burst's frames to N (timing only: repeated frames align trivially but cost the same).
#include <rawr/raw_gpu_pipeline/AndroidBurstCoordinator.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "RextureStageObserver.h"
#include "StageDump.h"
#include "vk_test_common.hpp"

namespace {
using Clock = std::chrono::steady_clock;
using namespace rawr::raw_gpu_pipeline;

struct Commands {
    VkDevice device{};
    VkCommandPool pool{};
    VkCommandBuffer command{};
    VkFence fence{};
    explicit Commands(const vktest::Ctx& c) : device(c.dev) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = c.qf;
        vktest::ck(vkCreateCommandPool(device, &pi, nullptr, &pool), "command pool");
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        vktest::ck(vkAllocateCommandBuffers(device, &ai, &command), "command buffer");
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vktest::ck(vkCreateFence(device, &fi, nullptr, &fence), "fence");
    }
    ~Commands() {
        if (fence) vkDestroyFence(device, fence, nullptr);
        if (pool) vkDestroyCommandPool(device, pool, nullptr);
    }
    template <typename F> void submit(VkQueue queue, F&& record) {
        vktest::ck(vkResetCommandBuffer(command, 0), "reset command");
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vktest::ck(vkBeginCommandBuffer(command, &bi), "begin command");
        record(command);
        vktest::ck(vkEndCommandBuffer(command), "end command");
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &command;
        vktest::ck(vkResetFences(device, 1, &fence), "reset fence");
        vktest::ck(vkQueueSubmit(queue, 1, &si, fence), "queue submit");
        vktest::ck(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "fence wait");
    }
};

std::map<std::string, std::string> keyValues(const std::string& path, char sep) {
    std::map<std::string, std::string> out;
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::string line;
    while (std::getline(in, line)) {
        const auto at = line.find(sep);
        if (at != std::string::npos) out[line.substr(0, at)] = line.substr(at + 1);
    }
    return out;
}

std::vector<float> csvFloats(const std::string& text, std::size_t n) {
    std::vector<float> v;
    std::istringstream in(text);
    std::string item;
    while (std::getline(in, item, ',') && v.size() < n) v.push_back(std::stof(item));
    if (v.size() != n) throw std::runtime_error("bad list: " + text);
    return v;
}

long peakRssKb() {  // VmHWM of this process (Linux / Android); 0 elsewhere
    std::ifstream in("/proc/self/status");
    std::string line;
    while (std::getline(in, line))
        if (line.rfind("VmHWM:", 0) == 0) return std::stol(line.substr(6));
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    try {
        std::string dir, merge = "rexture", dumpDir, inject, noiseAb;
        int frames = 0, repeat = 1;
        bool scores = false;
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            auto need = [&]() { if (i + 1 >= argc) throw std::invalid_argument("missing value for " + a); return std::string(argv[++i]); };
            if (a == "--merge") merge = need();
            else if (a == "--frames") frames = std::stoi(need());
            else if (a == "--noise-ab") noiseAb = need();
            else if (a == "--dump-stages") dumpDir = need();
            else if (a == "--inject-flow") inject = need();
            else if (a == "--repeat") repeat = std::stoi(need());
            else if (a == "--scores") scores = true;
            else if (a.rfind("--", 0) == 0) throw std::invalid_argument("unknown flag " + a);
            else dir = a;
        }
        if (dir.empty()) throw std::invalid_argument("usage: rexture_device_check DIR [options]");
        const auto info = keyValues(dir + "/bundle.txt", '=');
        const std::uint32_t width = std::stoul(info.at("width")), height = std::stoul(info.at("height"));
        const std::uint32_t cfa = std::stoul(info.at("cfa"));
        const int stored = std::stoi(info.at("frames"));
        const int count = frames > 0 ? frames : stored;

        auto ctx = vktest::ctx();
        std::cout << "device=" << ctx.prop.deviceName << " shared_kb=" << ctx.prop.limits.maxComputeSharedMemorySize / 1024u
                  << " geometry=" << width << 'x' << height << " frames=" << count << " merge=" << merge << '\n';
        Commands commands(ctx);
        std::vector<vktest::Img> images;
        std::vector<BurstFrame> burst;
        std::vector<std::vector<std::uint8_t>> raws(stored);
        MultiframeFrameParameters params{};
        std::array<float, 4> blackSeen{};
        float whiteSeen = 0;
        std::uint32_t ref = 0;
        auto staging = vktest::mkBuf(ctx.pd, ctx.dev, VkDeviceSize(width) * height * 2u, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        for (int i = 0; i < count; ++i) {
            const int src = i % stored;
            char name[32];
            std::snprintf(name, sizeof(name), "/frame_%02d", src);
            if (raws[src].empty()) {
                std::ifstream f(dir + name + ".raw16", std::ios::binary | std::ios::ate);
                if (!f) throw std::runtime_error(std::string("cannot open ") + name + ".raw16");
                const auto bytes = static_cast<std::size_t>(f.tellg());
                if (bytes != std::size_t(width) * height * 2u) throw std::runtime_error("raw16 size mismatch");
                f.seekg(0);
                raws[src].resize(bytes);
                f.read(reinterpret_cast<char*>(raws[src].data()), static_cast<std::streamsize>(bytes));
                const auto meta = keyValues(dir + name + ".meta", '\t');
                const auto black = csvFloats(meta.at("blackLevelPhysicalRggb"), 4);
                const auto gains = csvFloats(meta.at("colorCorrectionGainsRggb"), 4);
                if (src == 0) {
                    std::copy(black.begin(), black.end(), blackSeen.begin());
                    whiteSeen = std::stof(meta.at("effectiveWhiteLevel"));
                    params.whiteBalance = {gains[0], .5f * (gains[1] + gains[2]), gains[3]};
                    ref = static_cast<std::uint32_t>(std::stoul(meta.at("rawrReferenceIndex")));
                }
            }
            images.push_back(vktest::mkImg(ctx.pd, ctx.dev, width, height, VK_FORMAT_R16_UINT,
                                           VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT));
            void* mapped = nullptr;
            vktest::ck(vkMapMemory(ctx.dev, staging.m, 0, raws[src].size(), 0, &mapped), "map staging");
            std::memcpy(mapped, raws[src].data(), raws[src].size());
            vkUnmapMemory(ctx.dev, staging.m);
            auto& im = images.back();
            commands.submit(ctx.q, [&](VkCommandBuffer c) {
                VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.image = im.i;
                b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                                     nullptr, 1, &b);
                VkBufferImageCopy copy{};
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.imageExtent = {width, height, 1};
                vkCmdCopyBufferToImage(c, staging.b, im.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
                b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                                     nullptr, 1, &b);
            });
            MultiframeFrameParameters fp = params;
            fp.normalization.blackByPhase = blackSeen;
            fp.normalization.whiteLevel = whiteSeen;
            rawr::zsl_ring::GpuRawImageView raw{{std::uint64_t(i + 1), 1000000000ull + std::uint64_t(i) * 33333333ull, width, height},
                                                im.i, im.v, VK_FORMAT_R16_UINT, VkDeviceSize(width) * height * 2u};
            burst.push_back({raw, fp});
        }
        vktest::delBuf(ctx.dev, staging);
        if (ref >= burst.size()) ref = std::uint32_t(burst.size() / 2);

        rawr::raw_merge_wronski_gpu::Config cfg{};
        cfg.cfa = static_cast<rawr::raw_merge_wronski_gpu::CfaPattern>(cfa);
        if (!noiseAb.empty()) {
            const auto comma = noiseAb.find(',');
            cfg.noiseAlpha = std::stof(noiseAb.substr(0, comma));
            cfg.noiseBeta = std::stof(noiseAb.substr(comma + 1));
        }
        const MergeAlgorithm algorithm = merge == "hdrplus-freq" ? MergeAlgorithm::HdrPlusFrequency : MergeAlgorithm::Rexture;
        AndroidBurstCoordinator coordinator;
        std::unique_ptr<stagedump::StageDump> dump;
        if (!dumpDir.empty()) {
            dump = std::make_unique<stagedump::StageDump>(ctx, dumpDir, [&](const stagedump::StageDump::Record& r) {
                commands.submit(ctx.q, r);
            });
            coordinator.setRextureObserver(stagedump::makeRextureObserver(*dump, inject));
        }
        coordinator.initialize(ctx.pd, ctx.dev, ctx.qf,
                               [&](const VkSubmitInfo& si, VkFence f) { vktest::ck(vkQueueSubmit(ctx.q, 1, &si, f), "burst submit"); },
                               width, height, 1.0f, {}, {}, cfg, algorithm, {});
        if (scores && algorithm == MergeAlgorithm::Rexture) {
            const auto s = coordinator.scoreRextureFrames(burst);
            for (std::size_t i = 0; i < s.size(); ++i)
                std::printf("score frame=%zu sharp=%.9g sat=%.6f\n", i, s[i].sharp, s[i].sat);
        }
        BurstRunResult result;
        for (int it = 0; it < repeat; ++it) {
            result = coordinator.run(burst, ref);
            const auto& t = result.timings;
            std::printf("run=%d frames=%d total_ms=%.2f base_prepare_ms=%.2f base_kernel_rcd_ms=%.2f companion_prepare_ms=%.2f "
                        "alignment_ms=%.2f robustness_ms=%.2f accumulation_ms=%.2f finalize_ms=%.2f init_ms=%.2f "
                        "arena_image_mib=%.1f arena_buffer_mib=%.1f peak_rss_mib=%.1f\n",
                        it, count, t.totalMs, t.referencePrepareMs, t.referenceStatsMs, t.companionPrepareMs, t.alignmentMs,
                        t.robustnessMs, t.accumulationMs, t.finalizeMs, t.initializeMs,
                        double(result.physicalImageBytes) / 1048576.0, double(result.physicalBufferBytes) / 1048576.0,
                        double(peakRssKb()) / 1024.0);
        }
        if (dump) dump->finish();
        coordinator.reset();
        for (auto& im : images) vktest::delImg(ctx.dev, im);
        vktest::delCtx(ctx);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "ERROR " << e.what() << '\n';
        return 1;
    }
}
