#include <array>
#include <cstring>
#include <dual/Dual.hpp>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "../../../vk_common/testing/vk_test_common.hpp"
using namespace vktest;
static std::vector<uint8_t> readBytes(const std::string& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("Cannot open " + p);
    auto n = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> b(static_cast<size_t>(n));
    if (n > 0) f.read(reinterpret_cast<char*>(b.data()), n);
    return b;
}
static dual::ShaderProvider provider(std::string dir) {
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
static std::vector<uint16_t> readback(const Ctx& c, VkCommandPool pool, Img& img, uint32_t W, uint32_t H) {
    VkDeviceSize n = VkDeviceSize(size_t(W) * H * 8);
    Buf d = mkBuf(c.pd, c.dev, n, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkCommandBuffer cmd{};
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    ck(vkAllocateCommandBuffers(c.dev, &ai, &cmd), "alloc read cmd");
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    ck(vkBeginCommandBuffer(cmd, &bi), "begin read");
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img.i;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    VkBufferImageCopy bc{};
    bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bc.imageExtent = {W, H, 1};
    vkCmdCopyImageToBuffer(cmd, img.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d.b, 1, &bc);
    ck(vkEndCommandBuffer(cmd), "end read");
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    ck(vkQueueSubmit(c.q, 1, &si, VK_NULL_HANDLE), "submit read");
    ck(vkQueueWaitIdle(c.q), "wait read");
    void* m = nullptr;
    ck(vkMapMemory(c.dev, d.m, 0, n, 0, &m), "map read");
    std::vector<uint16_t> o(size_t(W) * H * 4);
    std::memcpy(o.data(), m, size_t(n));
    vkUnmapMemory(c.dev, d.m);
    vkFreeCommandBuffers(c.dev, pool, 1, &cmd);
    delBuf(c.dev, d);
    return o;
}
int main(int argc, char** argv) {
    try {
        std::string sd, in, out;
        uint32_t W = 0, H = 0, pattern = 0;
        float contrast = 20.f;
        uint32_t mode = 1;
        bool autoContrast = false;
        bool amazePrimary = false;
        std::array<float, 3> wb{1.0f, 1.0f, 1.0f};
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--shader-dir")
                sd = argv[++i];
            else if (a == "--input-f32")
                in = argv[++i];
            else if (a == "--output-rgba16f")
                out = argv[++i];
            else if (a == "--width")
                W = uint32_t(std::stoul(argv[++i]));
            else if (a == "--height")
                H = uint32_t(std::stoul(argv[++i]));
            else if (a == "--pattern")
                pattern = uint32_t(std::stoul(argv[++i]));
            else if (a == "--contrast-percent")
                contrast = std::stof(argv[++i]);
            else if (a == "--optimization-mode")
                mode = uint32_t(std::stoul(argv[++i]));
            else if (a == "--auto-contrast")
                autoContrast = true;
            else if (a == "--primary")
                amazePrimary = std::string(argv[++i]) == "amaze";
            else if (a == "--wb") {
                wb[0] = std::stof(argv[++i]);
                wb[1] = std::stof(argv[++i]);
                wb[2] = std::stof(argv[++i]);
            }
        }
        if (sd.empty() || in.empty() || out.empty() || !W || !H || pattern > 3)
            throw std::runtime_error(
                "usage: dual_validate --shader-dir DIR --input-f32 FILE --output-rgba16f FILE --width W --height H "
                "--pattern 0..3 [--contrast-percent P] [--auto-contrast] [--primary rcd|amaze] [--wb r g b]");
        auto bytes = readBytes(in);
        if (bytes.size() != size_t(W) * H * 4) throw std::runtime_error("input size mismatch");
        Ctx c = ctx();
        std::cout << "Using GPU: " << c.prop.deviceName << "\n";
        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.queueFamilyIndex = c.qf;
        VkCommandPool pool{};
        ck(vkCreateCommandPool(c.dev, &pci, nullptr, &pool), "pool");
        Buf ib = mkBuf(c.pd, c.dev, bytes.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        void* m = nullptr;
        ck(vkMapMemory(c.dev, ib.m, 0, bytes.size(), 0, &m), "map in");
        std::memcpy(m, bytes.data(), bytes.size());
        vkUnmapMemory(c.dev, ib.m);
        Img oi = mkImg(c.pd, c.dev, W, H, VK_FORMAT_R16G16B16A16_SFLOAT,
                       VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        dual::PipelineConfig cfg{};
        cfg.width = W;
        cfg.height = H;
        cfg.pattern = static_cast<dual::BayerPattern>(pattern);
        cfg.inputMode = dual::InputMode::NormalizedFloatBuffer;
        cfg.contrastPercent = contrast;
        cfg.autoContrast = autoContrast;
        cfg.optimizationMode = static_cast<dual::OptimizationMode>(mode);
        if (amazePrimary) cfg.primary = dual::Primary::Amaze;
        auto pipe = std::make_unique<dual::DualDemosaicPipeline>(dual::VulkanContext{c.pd, c.dev, c.qf, nullptr},
                                                                 provider(sd), cfg);
        pipe->setWhiteBalance(wb);
        VkCommandBuffer cmd{};
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        ck(vkAllocateCommandBuffers(c.dev, &cai, &cmd), "cmd");
        VkCommandBufferBeginInfo cbi{};
        cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        ck(vkBeginCommandBuffer(cmd, &cbi), "begin");
        transition(cmd, oi.i);
        dual::NormalizedBayerBufferView iv{{ib.b, 0, bytes.size()}, W, H, static_cast<dual::BayerPattern>(pattern)};
        dual::LinearRgbImage ov{oi.i, oi.v, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_LAYOUT_GENERAL, W, H};
        pipe->record(cmd, iv, ov);
        ck(vkEndCommandBuffer(cmd), "end");
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        ck(vkQueueSubmit(c.q, 1, &si, VK_NULL_HANDLE), "submit");
        ck(vkQueueWaitIdle(c.q), "wait");
        auto rgba = readback(c, pool, oi, W, H);
        std::ofstream f(out, std::ios::binary);
        f.write(reinterpret_cast<const char*>(rgba.data()), std::streamsize(rgba.size() * 2));
        std::cout << "DUAL_VULKAN_VALIDATE_PASS pixels=" << size_t(W) * H << " contrast_percent=" << contrast
                  << " auto_contrast=" << (autoContrast ? 1 : 0) << " optimization_mode=" << mode << "\n";
        pipe.reset();
        vkFreeCommandBuffers(c.dev, pool, 1, &cmd);
        delImg(c.dev, oi);
        delBuf(c.dev, ib);
        vkDestroyCommandPool(c.dev, pool, nullptr);
        delCtx(c);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "DUAL_VULKAN_VALIDATE_FAIL: " << e.what() << "\n";
        return 1;
    }
}
