#include "RendererEngine.h"
#include <rcd/Balance.hpp>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <dual/Pipeline.hpp>
#include <filesystem>
#include <map>
#include <quadfix/Pipeline.hpp>
#include <raw_denoise/DenoisePipeline.hpp>
#include <rcd/Pipeline.hpp>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <vng4/Pipeline.hpp>

#include "LinearResample.h"
#include "RendererSurface.h"
#include "color/ColorMath.h"
#include "raw_demosaic/EmbeddedShaders.hpp"
#include "vulkan/VulkanDispatch.h"

namespace rawrcam::renderer {
namespace {
void vkOk(VkResult result) {
    if (result != VK_SUCCESS) throw std::runtime_error("Renderer GPU failure: " + std::to_string(result));
}
struct MappedFile {
    int fd = -1;
    size_t size = 0;
    void* data = MAP_FAILED;
    MappedFile(const std::string& path, size_t length, bool create) : size(length) {
        fd = ::open(path.c_str(), create ? O_RDWR | O_CREAT | O_TRUNC : O_RDONLY, 0600);
        if (fd < 0) throw std::runtime_error("Cannot open renderer cache");
        if (create && (posix_fallocate(fd, 0, off_t(size)) != 0 || ftruncate(fd, off_t(size)) != 0)) {
            close(fd);
            throw std::runtime_error("Not enough storage for renderer cache");
        }
        struct stat st{};
        if (fstat(fd, &st) || uint64_t(st.st_size) < size) {
            close(fd);
            throw std::runtime_error("Incomplete renderer cache");
        }
        data = mmap(nullptr, size, create ? PROT_READ | PROT_WRITE : PROT_READ, MAP_SHARED, fd, 0);
        if (data == MAP_FAILED) {
            close(fd);
            throw std::runtime_error("Cannot map renderer cache");
        }
    }
    ~MappedFile() {
        if (data != MAP_FAILED) munmap(data, size);
        if (fd >= 0) close(fd);
    }
};
struct Commands {
    VkDevice device;
    VkQueue queue;
    VkCommandPool pool{};
    VkCommandBuffer command{};
    VkFence fence{};
    explicit Commands(const vulkan::VulkanContext& v) : device(v.device()), queue(v.queue()) {
        try {
            VkCommandPoolCreateInfo p{};
            p.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            p.queueFamilyIndex = v.queueFamily();
            p.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            vkOk(vkCreateCommandPool(device, &p, nullptr, &pool));
            VkCommandBufferAllocateInfo a{};
            a.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            a.commandPool = pool;
            a.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            a.commandBufferCount = 1;
            vkOk(vkAllocateCommandBuffers(device, &a, &command));
            VkFenceCreateInfo f{};
            f.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            vkOk(vkCreateFence(device, &f, nullptr, &fence));
        } catch (...) {
            vkDestroyCommandPool(device, pool, nullptr);
            throw;
        }
    }
    ~Commands() {
        vkDestroyFence(device, fence, nullptr);
        vkDestroyCommandPool(device, pool, nullptr);
    }
    void begin() {
        vkOk(vkResetCommandBuffer(command, 0));
        VkCommandBufferBeginInfo b{};
        b.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkOk(vkBeginCommandBuffer(command, &b));
    }
    void submit() {
        vkOk(vkEndCommandBuffer(command));
        vkOk(vkResetFences(device, 1, &fence));
        VkSubmitInfo s{};
        s.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        s.commandBufferCount = 1;
        s.pCommandBuffers = &command;
        vkOk(vkQueueSubmit(queue, 1, &s, fence));
        vkOk(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
    }
};
struct Buffer {
    VkDevice device;
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    void* data = nullptr;
    Buffer(const vulkan::VulkanContext& v, size_t size) : device(v.device()) {
        try {
            VkBufferCreateInfo b{};
            b.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            b.size = size;
            b.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                      VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            vkOk(vkCreateBuffer(device, &b, nullptr, &buffer));
            VkMemoryRequirements r{};
            vkGetBufferMemoryRequirements(device, buffer, &r);
            VkPhysicalDeviceMemoryProperties props{};
            vkGetPhysicalDeviceMemoryProperties(v.physicalDevice(), &props);
            uint32_t index = props.memoryTypeCount;
            for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
                if ((r.memoryTypeBits & (1u << i)) &&
                    (props.memoryTypes[i].propertyFlags &
                     (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                        (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                    index = i;
                    break;
                }
            if (index == props.memoryTypeCount) throw std::runtime_error("No coherent renderer memory");
            VkMemoryAllocateInfo a{};
            a.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            a.allocationSize = r.size;
            a.memoryTypeIndex = index;
            vkOk(vkAllocateMemory(device, &a, nullptr, &memory));
            vkOk(vkBindBufferMemory(device, buffer, memory, 0));
            vkOk(vkMapMemory(device, memory, 0, size, 0, &data));
        } catch (...) {
            vkDestroyBuffer(device, buffer, nullptr);
            vkFreeMemory(device, memory, nullptr);
            throw;
        }
    }
    ~Buffer() {
        if (data) vkUnmapMemory(device, memory);
        vkDestroyBuffer(device, buffer, nullptr);
        vkFreeMemory(device, memory, nullptr);
    }
};
struct Image {
    VkDevice device;
    vulkan::OwnedImage value;
    Image(const vulkan::VulkanContext& v, uint32_t w, uint32_t h)
        : device(v.device()),
          value(vulkan::createOwnedImage(
              v.physicalDevice(), device, w, h, VK_FORMAT_R16G16B16A16_SFLOAT,
              VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {}
    ~Image() { vulkan::destroyOwnedImage(device, value); }
};
void barrier(VkCommandBuffer c, VkImage image, VkImageLayout oldLayout, VkImageLayout layout) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = oldLayout;
    b.newLayout = layout;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcAccessMask = oldLayout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
}
float lanczos(float x) {
    x = std::abs(x);
    if (x < 1e-6f) return 1;
    if (x >= 3) return 0;
    float p = 3.14159265358979323846f * x;
    return std::sin(p) * std::sin(p / 3) / (p * p / 3);
}
}  // namespace
RendererEngine::RendererEngine(const std::string& dir, const std::string& lib, AAssetManager* assets)
    : filesDir_(dir), processor_(dir) {
    if (!vkCreateInstance) vulkan::dispatch::configure(lib, "");
    vulkan_.createInstance();
    vulkan_.createDeviceForSurface(VK_NULL_HANDLE);
    processor_.setAssetManager(assets);
    processor_.setEnginePersistenceEnabled(true);
}
RendererEngine::~RendererEngine() {
    surface_.reset();
    processor_.reset();
    processor_.shutdownFilm();
    vulkan::destroyOwnedImage(vulkan_.device(), overviewInput_);
}
void RendererEngine::setSurface(ANativeWindow* window) {
    if (!surface_) surface_ = std::make_unique<RendererSurface>(vulkan_);
    surface_->attach(window);
}
bool RendererEngine::hasSurface() const noexcept { return surface_ && surface_->attached(); }
void RendererEngine::check() {
    if (cancelled.load()) throw std::runtime_error("Cancelled");
}
void RendererEngine::prepare(DngSource& source, const std::string& path, const RenderOptions& options) {
    const bool shading = options.shading;
    const uint32_t w = source.info().width / (options.bayerBin2x ? 2u : 1u);
    const uint32_t h = source.info().height / (options.bayerBin2x ? 2u : 1u);
    auto normalized = [&](uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
        return options.bayerBin2x ? source.binnedNormalized(x, y, width, height, shading)
                                  : source.normalized(x, y, width, height, shading);
    };
    lastDemosaicMs_ = 0.0;
    if (std::filesystem::exists(path) && std::filesystem::file_size(path) == uint64_t(w) * h * 8) return;
    const auto demosaicBegin = std::chrono::steady_clock::now();
    const auto temporary = path + ".tmp";
    try {
        MappedFile file(temporary, size_t(w) * h * 8, true);
        auto* dst = static_cast<uint16_t*>(file.data);
        // Estimate one frame balance with a bounded row buffer, then reuse it
        // across every tile. Independent tile estimates leave chroma seams.
        rcd::BalanceAccumulator balance;
        if (options.demosaic != "DualRcdVng4") {
            for (uint32_t y = 0; y < h; y += 64) {
                check();
                uint32_t rows = std::min(64u, h - y);
                auto raw = normalized(0, y, w, rows);
                for (uint32_t by = 0; by < rows; by += 8)
                    for (uint32_t x = 0; x < w; x += 8)
                        balance.addBlock(raw.data() + size_t(by) * w + x, w, std::min(8u, w - x),
                                         std::min(8u, rows - by), static_cast<rcd::BayerPattern>(source.cfa));
            }
        }
        const auto frameBalance = balance.gains();
        // Dual auto contrast uses image-wide statistics: keep it a single frame.
        const uint32_t tile = options.demosaic == "DualRcdVng4" ? std::max(w, h) : 1024, halo = 16;
        // quadfix adapt004 pre-filter halo, frozen after host measurement
        // (tiled-vs-full sky delta: H=96 max 0.076 DN, H=128 max 0.021 DN,
        // interior tiles ~0 with real neighbor data). Must stay % 16 == 0 so
        // the expanded tile keeps quadfix plane-block alignment.
        const uint32_t quadHalo = 128;
        Commands commands(vulkan_);
        std::map<std::pair<uint32_t, uint32_t>, std::unique_ptr<rcd::RcdPipeline>> pipelines;
        std::map<std::tuple<uint32_t, uint32_t, bool>, std::unique_ptr<quadfix::QuadfixPipeline>> quadPipes;
        // Filters the expanded quadfix tile covering core [cx0,cx0+cw) x
        // [cy0,cy0+ch) and copies the core rows into dst (row pitch dstStride,
        // placed at (dstX,dstY)). Records into the already-begun command
        // buffer; the caller submits. Returns false when the expanded tile
        // violates quadfix alignment (caller falls back to unfiltered).
        auto quadfixCore = [&](uint32_t cx0, uint32_t cy0, uint32_t cw, uint32_t ch, VkBuffer dst, uint32_t dstStride,
                               uint32_t dstX, uint32_t dstY) {
            uint32_t qleft = cx0 > quadHalo ? cx0 - quadHalo : 0, qtop = cy0 > quadHalo ? cy0 - quadHalo : 0,
                     qright = std::min(cx0 + cw + quadHalo, w), qbottom = std::min(cy0 + ch + quadHalo, h);
            uint32_t qw = qright - qleft, qh = qbottom - qtop;
            if ((qw & 15u) != 0 || (qh & 15u) != 0 || qw < 64 || qh < 64) return false;
            auto qraw = normalized(qleft, qtop, qw, qh);
            const VkDeviceSize qbytes = VkDeviceSize(size_t(qw) * qh * 4);
            Buffer qUpload(vulkan_, size_t(qbytes)), qFiltered(vulkan_, size_t(qbytes));
            std::memcpy(qUpload.data, qraw.data(), size_t(qbytes));
            quadfix::PipelineConfig qcfg{};
            qcfg.width = qw;
            qcfg.height = qh;
            qcfg.fastMedian = options.quadfixFastMedian;
            auto& qpipe = quadPipes[{qw, qh, qcfg.fastMedian}];
            if (!qpipe)
                qpipe = std::make_unique<quadfix::QuadfixPipeline>(
                    quadfix::VulkanContext{vulkan_.physicalDevice(), vulkan_.device(), vulkan_.queueFamily(), nullptr},
                    raw_demosaic::embeddedQuadfixShaders(), qcfg);
            qpipe->record(commands.command, quadfix::BayerBufferView{{qUpload.buffer, 0, qbytes}, qw, qh},
                          quadfix::BayerBufferView{{qFiltered.buffer, 0, qbytes}, qw, qh});
            std::vector<VkBufferCopy> regions;
            regions.reserve(ch);
            for (uint32_t yy = 0; yy < ch; ++yy)
                regions.push_back({VkDeviceSize(size_t(cy0 - qtop + yy) * qw + (cx0 - qleft)) * 4,
                                   VkDeviceSize(size_t(dstY + yy) * dstStride + dstX) * 4,
                                   VkDeviceSize(size_t(cw)) * 4});
            vkCmdCopyBuffer(commands.command, qFiltered.buffer, dst, uint32_t(regions.size()), regions.data());
            VkBufferMemoryBarrier bb{};
            bb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            bb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bb.buffer = dst;
            bb.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(commands.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 0, nullptr, 1, &bb, 0, nullptr);
            return true;
        };
        // Dual demosaics a single full-frame tile, but quadfix scratch for a
        // 200MP frame (~2.7GB) exceeds device limits. Pre-pass: tile the
        // filter over the frame into a full-frame filtered buffer, then run
        // dual once from it. Interior tiles see real neighbor data, so the
        // result matches the tiled RCD/VNG4 path up to frame-border clamping.
        std::unique_ptr<Buffer> dualFiltered;
        if (options.quadfix && options.demosaic == "DualRcdVng4" && (w & 15u) == 0 && (h & 15u) == 0) {
            dualFiltered = std::make_unique<Buffer>(vulkan_, size_t(w) * h * 4);
            const uint32_t qtile = 1024;
            bool ok = true;
            for (uint32_t y = 0; y < h && ok; y += qtile)
                for (uint32_t x = 0; x < w && ok; x += qtile) {
                    check();
                    commands.begin();
                    ok = quadfixCore(x, y, std::min(qtile, w - x), std::min(qtile, h - y), dualFiltered->buffer, w, x,
                                     y);
                    commands.submit();
                    progress = 5 + int(15.0 * (size_t(y) * w + x) / (size_t(w) * h));
                }
            if (!ok) dualFiltered.reset();  // aligned frame checked above; paranoia only
        }
        const int mainBase = dualFiltered ? 20 : 5, mainSpan = dualFiltered ? 25 : 40;
        for (uint32_t y = 0; y < h; y += tile)
            for (uint32_t x = 0; x < w; x += tile) {
                check();
                uint32_t left = x > halo ? x - halo : 0, top = y > halo ? y - halo : 0,
                         right = std::min(x + tile + halo, w), bottom = std::min(y + tile + halo, h);
                uint32_t tw = right - left, th = bottom - top, cw = std::min(tile, w - x), ch = std::min(tile, h - y);
                // quadfix runs on an expanded tile and compacts its core back
                // to tw x th, so demosaic work is unchanged. The dual
                // full-frame tile is served from the pre-pass buffer above;
                // unaligned geometries fall back to the plain path.
                uint32_t qleft = x > quadHalo ? x - quadHalo : 0, qtop = y > quadHalo ? y - quadHalo : 0,
                         qright = std::min(x + tile + quadHalo, w), qbottom = std::min(y + tile + quadHalo, h);
                uint32_t qw = qright - qleft, qh = qbottom - qtop;
                const bool quad = options.quadfix && !dualFiltered && tile <= 1024 && (qw & 15u) == 0 &&
                                  (qh & 15u) == 0 && qw >= 64 && qh >= 64;
                const VkDeviceSize bayerBytes = VkDeviceSize(size_t(tw) * th * 4);
                Buffer upload(vulkan_, size_t(bayerBytes)), download(vulkan_, size_t(tw) * th * 8);
                Image output(vulkan_, tw, th);
                commands.begin();
                if (quad) {
                    if (!quadfixCore(left, top, tw, th, upload.buffer, tw, 0, 0))
                        throw std::runtime_error("quadfix tile rejected after alignment check");
                } else if (dualFiltered) {
                    // Full-frame filtered input: copy this tile's region.
                    std::vector<VkBufferCopy> regions;
                    regions.reserve(th);
                    for (uint32_t yy = 0; yy < th; ++yy)
                        regions.push_back({VkDeviceSize(size_t(top + yy) * w + left) * 4,
                                           VkDeviceSize(size_t(yy) * tw) * 4, VkDeviceSize(size_t(tw)) * 4});
                    vkCmdCopyBuffer(commands.command, dualFiltered->buffer, upload.buffer, uint32_t(regions.size()),
                                    regions.data());
                    VkBufferMemoryBarrier bb{};
                    bb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                    bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    bb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                    bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    bb.buffer = upload.buffer;
                    bb.size = bayerBytes;
                    vkCmdPipelineBarrier(commands.command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &bb, 0, nullptr);
                } else {
                    auto raw = normalized(left, top, tw, th);
                    std::memcpy(upload.data, raw.data(), raw.size() * 4);
                }
                barrier(commands.command, output.value.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
                std::unique_ptr<vng4::Vng4Pipeline> vng;
                std::unique_ptr<dual::DualDemosaicPipeline> dualPipeline;
                rcd::RcdPipeline* rcdPipeline = nullptr;
                if (options.demosaic == "Vng4") {
                    vng4::PipelineConfig cfg{};
                    cfg.width = tw;
                    cfg.height = th;
                    cfg.pattern = static_cast<vng4::BayerPattern>(source.cfa);
                    cfg.inputMode = vng4::InputMode::NormalizedFloatBuffer;
                    vng = std::make_unique<vng4::Vng4Pipeline>(
                        vng4::VulkanContext{vulkan_.physicalDevice(), vulkan_.device(), vulkan_.queueFamily(), nullptr},
                        raw_demosaic::embeddedVng4Shaders(), cfg);
                    vng->record(
                        commands.command,
                        vng4::NormalizedBayerBufferView{{upload.buffer, 0, size_t(bayerBytes)}, tw, th, cfg.pattern},
                        {output.value.image, output.value.view, output.value.format, VK_IMAGE_LAYOUT_GENERAL, tw, th});
                } else if (options.demosaic == "DualRcdVng4") {
                    dual::PipelineConfig cfg{};
                    cfg.width = tw;
                    cfg.height = th;
                    cfg.pattern = static_cast<dual::BayerPattern>(source.cfa);
                    cfg.inputMode = dual::InputMode::NormalizedFloatBuffer;
                    cfg.autoContrast = options.dualAutoContrast;
                    cfg.contrastPercent = options.dualContrastPercent;
                    dualPipeline = std::make_unique<dual::DualDemosaicPipeline>(
                        dual::VulkanContext{vulkan_.physicalDevice(), vulkan_.device(), vulkan_.queueFamily(), nullptr},
                        raw_demosaic::embeddedDualShaders(), cfg);
                    dualPipeline->record(
                        commands.command,
                        dual::NormalizedBayerBufferView{{upload.buffer, 0, size_t(bayerBytes)}, tw, th, cfg.pattern},
                        {output.value.image, output.value.view, output.value.format, VK_IMAGE_LAYOUT_GENERAL, tw, th});
                } else {
                    rcd::PipelineConfig cfg{};
                    cfg.inputBalance = frameBalance;
                    cfg.width = tw;
                    cfg.height = th;
                    cfg.pattern = static_cast<rcd::BayerPattern>(source.cfa);
                    cfg.inputMode = rcd::InputMode::NormalizedFloatBuffer;
                    auto& pipeline = pipelines[{tw, th}];
                    if (!pipeline)
                        pipeline = std::make_unique<rcd::RcdPipeline>(
                            rcd::VulkanContext{vulkan_.physicalDevice(), vulkan_.device(), vulkan_.queueFamily(),
                                               nullptr},
                            raw_demosaic::embeddedRcdShaders(), cfg);
                    rcdPipeline = pipeline.get();
                    pipeline->record(
                        commands.command,
                        rcd::NormalizedBayerBufferView{{upload.buffer, 0, size_t(bayerBytes)}, tw, th, cfg.pattern},
                        {output.value.image, output.value.view, output.value.format, VK_IMAGE_LAYOUT_GENERAL, tw, th});
                }
                barrier(commands.command, output.value.image, VK_IMAGE_LAYOUT_GENERAL,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                VkBufferImageCopy region{};
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageExtent = {tw, th, 1};
                vkCmdCopyImageToBuffer(commands.command, output.value.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       download.buffer, 1, &region);
                commands.submit();
                auto* pixels = static_cast<uint16_t*>(download.data);
                for (uint32_t yy = 0; yy < ch; ++yy)
                    std::memcpy(dst + (size_t(y + yy) * w + x) * 4, pixels + (size_t(y - top + yy) * tw + x - left) * 4,
                                size_t(cw) * 8);
                if (rcdPipeline) rcdPipeline->forgetExternalImageView(output.value.view);
                progress = mainBase + int(double(mainSpan) * (size_t(y) * w + x) / (size_t(w) * h));
            }
        if (msync(file.data, file.size, MS_SYNC) || fsync(file.fd))
            throw std::runtime_error("Cannot persist renderer cache");
        std::filesystem::rename(temporary, path);
        lastDemosaicMs_ =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - demosaicBegin).count();
    } catch (...) {
        std::filesystem::remove(temporary);
        throw;
    }
}
void RendererEngine::prepareProxy(DngSource& source, const std::string& path, bool shading) {
    const auto sw = source.info().width, sh = source.info().height;
    const uint32_t w = std::min(2048u, sw / 2), h = std::max(1u, uint32_t(uint64_t(sh) * w / sw));
    if (std::filesystem::exists(path) && std::filesystem::file_size(path) == uint64_t(w) * h * 8) return;
    const auto temporary = path + ".tmp";
    try {
        MappedFile file(temporary, size_t(w) * h * 8, true);
        auto* dst = static_cast<_Float16*>(file.data);
        // LJPEG is independently compressed by tile or strip. Reading only
        // two sensor rows per output row decodes the same segment hundreds of
        // times. Decode a bounded band, then consume every preview row it
        // covers before moving to the next band.
        constexpr uint32_t bandHeight = 1024;
        uint32_t y = 0;
        for (uint32_t top = 0; top < sh && y < h; top += bandHeight) {
            check();
            const uint32_t lines = std::min(bandHeight, sh - top);
            auto band = source.normalized(0, top, sw, lines, shading);
            for (; y < h; ++y) {
                const uint32_t sy = std::min(sh - 2, uint32_t(uint64_t(y) * sh / h) & ~1u);
                if (sy + 1 >= top + lines) break;
                const auto* row = band.data() + size_t(sy - top) * sw;
                for (uint32_t x = 0; x < w; ++x) {
                    const uint32_t sx = std::min(sw - 2, uint32_t(uint64_t(x) * sw / w) & ~1u);
                    float rgb[3]{};
                    for (uint32_t yy = 0; yy < 2; ++yy)
                        for (uint32_t xx = 0; xx < 2; ++xx) {
                            auto c = source.info().cfa.pattern[yy * 2 + xx];
                            rgb[c] += row[size_t(yy) * sw + sx + xx] / 255.0f * (c == 1 ? .5f : 1.0f);
                        }
                    for (int c = 0; c < 3; ++c) dst[(size_t(y) * w + x) * 4 + c] = _Float16(rgb[c]);
                    dst[(size_t(y) * w + x) * 4 + 3] = 1;
                }
                progress = int(40.0 * y / h);
            }
        }
        if (y != h) throw std::runtime_error("Incomplete overview decode");
        if (msync(file.data, file.size, MS_SYNC) || fsync(file.fd))
            throw std::runtime_error("Cannot save overview cache");
        std::filesystem::rename(temporary, path);
    } catch (...) {
        std::filesystem::remove(temporary);
        throw;
    }
}
std::vector<uint8_t> RendererEngine::render(DngSource& source, const std::string& path, const RenderOptions& options) {
    check();
    // Overview previews never feed the JPEG writer; keep stale export
    // timings from leaking into a later export's attribution.
    if (options.overview) lastDemosaicMs_ = 0.0;
    const uint32_t w = options.width, h = options.height;
    const uint32_t sw = options.overview ? std::min(2048u, source.info().width / 2)
                                         : source.info().width / (options.bayerBin2x ? 2u : 1u);
    const uint32_t sh = options.overview
                            ? std::max(1u, uint32_t(uint64_t(source.info().height) * sw / source.info().width))
                            : source.info().height / (options.bayerBin2x ? 2u : 1u);
    auto crop = source.crop;
    if (options.bayerBin2x)
        for (auto& component : crop) component /= 2u;
    if (options.overview) {
        crop[0] = uint32_t(uint64_t(crop[0]) * sw / source.info().width);
        crop[1] = uint32_t(uint64_t(crop[1]) * sh / source.info().height);
        crop[2] = std::min(sw - crop[0], uint32_t(uint64_t(crop[2]) * sw / source.info().width));
        crop[3] = std::min(sh - crop[1], uint32_t(uint64_t(crop[3]) * sh / source.info().height));
    }
    if (!w || !h || uint64_t(w) * h > 250000000) throw std::runtime_error("Invalid export dimensions");
    // Renderer-only pre-work before the shared still stages: reduced-image
    // resample, distortion warp, and GPU upload. Folded into the Demosaic
    // EXIF line so the format keeps its fixed stage set; still captures
    // have no equivalent step.
    const auto preBegin = std::chrono::steady_clock::now();
    const std::string reducedPath = path + "-" + std::to_string(crop[0]) + "-" + std::to_string(crop[1]) + "-" +
                                    std::to_string(crop[2]) + "-" + std::to_string(crop[3]) + "-" + std::to_string(w) +
                                    "x" + std::to_string(h);
    const std::string inputKey = reducedPath + (options.distortion ? "-warp" : "-straight");
    const bool reuseOverview = options.overview && overviewInput_.image && overviewInputKey_ == inputKey;
    if (!reuseOverview && !std::filesystem::exists(reducedPath)) {
        const auto temporary = reducedPath + ".tmp";
        try {
            // The 200MP source can be >1 GB mapped. Release it as soon as
            // resampling finishes, before allocating the film GPU arena.
            MappedFile file(path, size_t(sw) * sh * 8, false);
            const auto* input = static_cast<const _Float16*>(file.data);
            MappedFile reduced(temporary, size_t(w) * h * 8, true);
            resampleLinear(input, sw, sh, static_cast<_Float16*>(reduced.data), w, h, crop, [&](uint32_t y) {
                check();
                progress = 45 + int(25.0 * y / h);
            });
            if (msync(reduced.data, reduced.size, MS_SYNC) || fsync(reduced.fd))
                throw std::runtime_error("Cannot persist reduced RAW");
            std::filesystem::rename(temporary, reducedPath);
        } catch (...) {
            std::filesystem::remove(temporary);
            throw;
        }
    }
    std::unique_ptr<Image> transientImage;
    std::unique_ptr<Image> workingOverview;
    vulkan::OwnedImage* preparedImage = &overviewInput_;
    if (!reuseOverview) {
        auto reduced = std::make_unique<MappedFile>(reducedPath, size_t(w) * h * 8, false);
        auto upload = std::make_unique<Buffer>(vulkan_, size_t(w) * h * 8);
        auto* dst = static_cast<_Float16*>(upload->data);
        const auto* pixels = static_cast<const _Float16*>(reduced->data);
        if (!options.distortion || source.info().raw.warp_count == 0)
            std::memcpy(dst, pixels, size_t(w) * h * 8);
        else {
            for (uint32_t y = 0; y < h; ++y) {
                check();
                for (uint32_t x = 0; x < w; ++x) {
                    double px = crop[0] + (x + .5) * crop[2] / w, py = crop[1] + (y + .5) * crop[3] / h;
                    for (size_t i = 0; i < source.info().raw.warp_count; ++i) {
                        auto& warp = source.info().raw.warps[i];
                        if (warp.plane_count != 1) throw std::runtime_error("Unsupported multi-plane distortion");
                        auto* k = warp.coeff[0];
                        double cx = warp.center[0] * sw, cy = warp.center[1] * sh,
                               norm = std::hypot(std::max(cx, sw - cx), std::max(cy, sh - cy));
                        double u = (px - cx) / norm, v = (py - cy) / norm, r = u * u + v * v,
                               f = k[0] + r * (k[1] + r * (k[2] + r * k[3]));
                        px = cx + norm * (u * f + 2 * k[4] * u * v + k[5] * (r + 2 * u * u));
                        py = cy + norm * (v * f + k[4] * (r + 2 * v * v) + 2 * k[5] * u * v);
                    }
                    px = (px - crop[0]) * w / crop[2] - .5;
                    py = (py - crop[1]) * h / crop[3] - .5;
                    float sum[3]{}, lo[3]{INFINITY, INFINITY, INFINITY}, hi[3]{-INFINITY, -INFINITY, -INFINITY};
                    double weights = 0;
                    for (int yy = int(std::ceil(py - 3)); yy <= int(std::floor(py + 3)); ++yy)
                        for (int xx = int(std::ceil(px - 3)); xx <= int(std::floor(px + 3)); ++xx) {
                            float wt = lanczos(float(yy - py)) * lanczos(float(xx - px));
                            if (std::abs(wt) < 1e-8) continue;
                            auto* p = pixels +
                                      (size_t(std::clamp(yy, 0, int(h) - 1)) * w + std::clamp(xx, 0, int(w) - 1)) * 4;
                            for (int c = 0; c < 3; ++c) {
                                float v = float(p[c]);
                                sum[c] += wt * v;
                                lo[c] = std::min(lo[c], v);
                                hi[c] = std::max(hi[c], v);
                            }
                            weights += wt;
                        }
                    for (int c = 0; c < 3; ++c)
                        dst[(size_t(y) * w + x) * 4 + c] = _Float16(std::clamp(float(sum[c] / weights), lo[c], hi[c]));
                    dst[(size_t(y) * w + x) * 4 + 3] = 1;
                }
            }
        }
        transientImage = std::make_unique<Image>(vulkan_, w, h);
        Commands commands(vulkan_);
        commands.begin();
        barrier(commands.command, transientImage->value.image, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {w, h, 1};
        vkCmdCopyBufferToImage(commands.command, upload->buffer, transientImage->value.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        barrier(commands.command, transientImage->value.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_GENERAL);
        commands.submit();
        if (options.overview) {
            vulkan::destroyOwnedImage(vulkan_.device(), overviewInput_);
            overviewInput_ = transientImage->value;
            transientImage->value = {};
            overviewInputKey_ = inputKey;
        } else {
            preparedImage = &transientImage->value;
        }
    }
    if (options.overview) {
        // PostDemosaicProcessor writes highlight recovery and defringe into
        // its source image. Keep the uploaded overview pristine and copy it
        // on the GPU for every slider render.
        workingOverview = std::make_unique<Image>(vulkan_, w, h);
        Commands commands(vulkan_);
        commands.begin();
        barrier(commands.command, workingOverview->value.image, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.extent = {w, h, 1};
        vkCmdCopyImage(commands.command, overviewInput_.image, VK_IMAGE_LAYOUT_GENERAL, workingOverview->value.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        barrier(commands.command, workingOverview->value.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_GENERAL);
        commands.submit();
        preparedImage = &workingOverview->value;
    }
    lastDemosaicMs_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - preBegin).count();
    auto context = options.context;
    context.width = w;
    context.height = h;
    context.surfacePreview = options.surfacePreview;
    context.whiteBalanceRgb = source.wb;
    context.cfaPattern = source.cfa;
    // Profiled denoise model from the source DNG NoiseProfile (green S,O,
    // CFA-aware pick) with the renderer normalization (black[site]/white).
    // Absent or invalid profiles force strength 0 (never hallucinate).
    context.denoiseNoiseA = 0.0f;
    context.denoiseNoiseB = 0.0f;
    if (context.denoiseStrength > 0.0f) {
        const int g0 = (source.cfa == 1u || source.cfa == 2u) ? 0 : 1;
        const int g1 = (source.cfa == 1u || source.cfa == 2u) ? 3 : 2;
        double white = source.whiteLevel;
        if (!(white > 0.0)) {
            const auto bits = source.info().bits_per_sample;
            if (bits >= 8 && bits <= 16) white = double((1u << bits) - 1u);
        }
        const double black = 0.5 * (source.black[size_t(g0)] + source.black[size_t(g1)]);
        float na = 0.0f, nb = 0.0f;
        if (source.noiseProfileCount == 8 &&
            raw_denoise::GreenNoiseToNormalized(source.noiseProfile.data(), source.noiseProfile.size(), source.cfa,
                                                float(black), float(white), na, nb)) {
            context.denoiseNoiseA = na;
            context.denoiseNoiseB = nb;
        } else {
            context.denoiseStrength = 0.0f;
        }
    }
    context.whiteBalanceRgb[0] *= std::exp2(options.temperature / 100.0f);
    context.whiteBalanceRgb[1] *= std::exp2(-options.tint / 100.0f);
    context.whiteBalanceRgb[2] *= std::exp2(-options.temperature / 100.0f);
    context.sensorToLinearSrgb = source.cameraToSrgb;
    context.cameraToWorkingColumnMajor = color::math::cameraToWorkingColumnMajor(source.cameraToSrgb);
    // UltraHDR (JPEG_R): preview/overview never allocates map resources,
    // matching the still-capture contract where preview skips the half-res
    // map. The HDR-tap CST is the same calibrated matrix the tonemap/film
    // path uses (never the AP1->sRGB default); source.cameraToSrgb is final
    // here (JNI applies resolvedFrame before render()).
    if (options.overview) {
        context.ultraHdrEnabled = false;
    }
    if (context.ultraHdrEnabled) {
        context.gainmapCstRowMajor = source.cameraToSrgb;
        context.hasGainmapCst = true;
    }
    // Self-heal: a prior aborted render may have left pixels mapped or a
    // completion unconsumed, both of which reject start(). Mirrors
    // MfsrCaptureJob's reset-before-start. Cheap here: engine
    // persistence is enabled, so cached tonemap/post/film engines survive
    // and only pixel resources are dropped.
    processor_.reset();
    if (!processor_.start(vulkan_, queueMutex_, context, preparedImage->image, preparedImage->view))
        throw std::runtime_error("Renderer is busy");
    try {
        std::optional<develop::rendered::RenderedStillCompletion> completion;
        while (!(completion = processor_.pollCompletion())) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        check();
        if (!completion->success) throw std::runtime_error(completion->error);
        lastCompletion_ = *completion;
        if (options.surfacePreview) {
            if (!hasSurface()) throw std::runtime_error("Renderer display is unavailable");
            surface_->present(processor_.outputImageView(), w, h, source.orientation);
            processor_.releasePixels();
            progress = 95;
            return {};
        }
        auto* bytes = static_cast<const uint8_t*>(processor_.pixelData());
        std::vector<uint8_t> result(bytes, bytes + processor_.pixelBytes());
        // Stable gain-map copy for the JPEG exporter (processor memory is
        // unmapped by releasePixels() below). Empty when SDR/preview.
        if (context.ultraHdrEnabled && processor_.gainmapPixelData() && processor_.gainmapPixelBytes() > 0 &&
            processor_.gainmapWidth() > 0 && processor_.gainmapHeight() > 0) {
            const auto* mapBytes = static_cast<const uint8_t*>(processor_.gainmapPixelData());
            lastGainmap_.assign(mapBytes, mapBytes + processor_.gainmapPixelBytes());
            lastGainmapWidth_ = processor_.gainmapWidth();
            lastGainmapHeight_ = processor_.gainmapHeight();
        } else {
            lastGainmap_.clear();
            lastGainmapWidth_ = 0;
            lastGainmapHeight_ = 0;
        }
        processor_.releasePixels();
        progress = 95;
        return result;
    } catch (...) {
        // Never wedge the session: an abort (slider cancel) or render
        // failure must not leave mapped pixels behind to reject the next
        // start(). The worker has always published its completion before
        // the poll loop can exit, so no GPU work is still in flight here.
        lastGainmap_.clear();
        lastGainmapWidth_ = 0;
        lastGainmapHeight_ = 0;
        processor_.releasePixels();
        throw;
    }
}
}  // namespace rawrcam::renderer
