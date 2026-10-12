// Host (MoltenVK) validation: CFA dump -> packed RGBA16F -> AmazePipeline ->
// camera-linear float32 RGB. Compare with librtprocess using
// validation/compare_librtprocess.py.
#include <amaze/Amaze.hpp>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
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
    std::vector<uint8_t> b(static_cast<size_t>(n), uint8_t{});
    if (n > 0) f.read(reinterpret_cast<char*>(b.data()), n);
    return b;
}

// spvDir overrides both build-time directories (on-device runs push one folder).
static amaze::ShaderProvider provider(std::string spvDir) {
    return [spvDir](std::string_view name) {
        const std::string n(name);
        const bool glsl = n.size() > 5 && n.compare(n.size() - 5, 5, ".comp") == 0;
        const std::string stem = n.substr(0, n.rfind('.'));
        const std::string dir = !spvDir.empty() ? spvDir : (glsl ? AMAZE_GLSL_SPV_DIR : AMAZE_WGSL_SPV_DIR);
        auto b = readBytes(dir + "/" + stem + ".spv");
        if (b.size() % 4) throw std::runtime_error("Bad SPIR-V size");
        std::vector<uint32_t> w(b.size() / 4);
        std::memcpy(w.data(), b.data(), b.size());
        return w;
    };
}

static uint16_t floatToHalf(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    int32_t e = int32_t((x >> 23) & 0xffu) - 127 + 15;
    uint32_t m = x & 0x7fffffu;
    if (e >= 31) return uint16_t(sign | 0x7c00u);
    if (e <= 0) {
        if (e < -10) return uint16_t(sign);
        m |= 0x800000u;
        const uint32_t shift = uint32_t(14 - e);
        uint32_t h = m >> shift;
        const uint32_t rem = m & ((1u << shift) - 1u), halfway = 1u << (shift - 1u);
        if (rem > halfway || (rem == halfway && (h & 1u))) ++h;
        return uint16_t(sign | h);
    }
    uint32_t h = (uint32_t(e) << 10) | (m >> 13);
    const uint32_t rem = m & 0x1fffu;
    if (rem > 0x1000u || (rem == 0x1000u && (h & 1u))) ++h;
    return uint16_t(sign | h);
}

static VkCommandBuffer beginOnce(const Ctx& c, VkCommandPool pool) {
    VkCommandBuffer cmd{};
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    ck(vkAllocateCommandBuffers(c.dev, &ai, &cmd), "alloc cmd");
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    ck(vkBeginCommandBuffer(cmd, &bi), "begin cmd");
    return cmd;
}
static void submitWait(const Ctx& c, VkCommandPool pool, VkCommandBuffer cmd) {
    ck(vkEndCommandBuffer(cmd), "end cmd");
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    ck(vkQueueSubmit(c.q, 1, &si, VK_NULL_HANDLE), "submit");
    ck(vkQueueWaitIdle(c.q), "wait");
    vkFreeCommandBuffers(c.dev, pool, 1, &cmd);
}
static void layout(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src,
                   VkAccessFlags dst, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

int main(int argc, char** argv) {
    try {
        std::string in, out, spvDir;
        uint32_t W = 0, H = 0, pattern = 0, slots = 8;
        bool floatInput = false;
        float black = 0, white = 0;
        std::array<float, 3> wb{1, 1, 1};
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            auto next = [&] {
                if (i + 1 >= argc) throw std::runtime_error("missing value for " + a);
                return std::string(argv[++i]);
            };
            if (a == "--cfa") in = next();
            else if (a == "--out") out = next();
            else if (a == "--width") W = uint32_t(std::stoul(next()));
            else if (a == "--height") H = uint32_t(std::stoul(next()));
            else if (a == "--pattern") pattern = uint32_t(std::stoul(next()));
            else if (a == "--black") black = std::stof(next());
            else if (a == "--white") white = std::stof(next());
            else if (a == "--slots") slots = uint32_t(std::stoul(next()));
            else if (a == "--spv-dir") spvDir = next();
            else if (a == "--float-input") floatInput = true;
            else if (a == "--wb") {
                for (auto& g : wb) g = std::stof(next());
            } else throw std::runtime_error("unknown argument " + a);
        }
        if (in.empty() || out.empty() || !W || !H || !(white > black))
            throw std::runtime_error(
                "usage: amaze_validate --cfa raw.u16 --out rgb.f32 --width W --height H --black B --white W "
                "[--pattern 0..3] [--wb r g b] [--slots N] [--spv-dir DIR] [--float-input]");

        // Packed {R,G1,G2,B}: same component mapping as amaze_tile.glsl.
        auto raw = readBytes(in);
        if (raw.size() != size_t(W) * H * 2) throw std::runtime_error("CFA size mismatch");
        const uint16_t* cfa = reinterpret_cast<const uint16_t*>(raw.data());
        auto component = [&](uint32_t x, uint32_t y) {
            const uint32_t px = x & 1u, py = y & 1u;
            static const int map[4][4] = {{0, 1, 2, 3}, {1, 0, 3, 2}, {2, 3, 0, 1}, {3, 2, 1, 0}};
            return map[pattern][py * 2 + px];
        };
        const uint32_t pw = (W + 1) / 2, ph = (H + 1) / 2;  // odd sizes: float input only
        std::vector<uint16_t> packed(size_t(pw) * ph * 4);
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x)
                packed[(size_t(y / 2) * pw + x / 2) * 4 + component(x, y)] =
                    floatToHalf((float(cfa[size_t(y) * W + x]) - black) / (white - black));

        // Float input: full-resolution 0..255 working domain (renderer path).
        std::vector<float> bayer255;
        if (floatInput) {
            bayer255.resize(size_t(W) * H);
            for (size_t i = 0; i < bayer255.size(); ++i) bayer255[i] = (float(cfa[i]) - black) / (white - black) * 255.0f;
        }

        Ctx c = ctx();
        std::cout << "device=" << c.prop.deviceName << "\n";
        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.queueFamilyIndex = c.qf;
        VkCommandPool pool{};
        ck(vkCreateCommandPool(c.dev, &pci, nullptr, &pool), "pool");

        Img inImg = mkImg(c.pd, c.dev, pw, ph, VK_FORMAT_R16G16B16A16_SFLOAT,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        Img outImg = mkImg(c.pd, c.dev, W, H, VK_FORMAT_R16G16B16A16_SFLOAT,
                           VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        const VkDeviceSize upBytes = packed.size() * 2, downBytes = VkDeviceSize(W) * H * 8;
        Buf up = mkBuf(c.pd, c.dev, upBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        Buf down = mkBuf(c.pd, c.dev, downBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        Buf cfaBuf{};
        void* m = nullptr;
        if (floatInput) {
            cfaBuf = mkBuf(c.pd, c.dev, bayer255.size() * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            ck(vkMapMemory(c.dev, cfaBuf.m, 0, bayer255.size() * 4, 0, &m), "map cfa");
            std::memcpy(m, bayer255.data(), bayer255.size() * 4);
            vkUnmapMemory(c.dev, cfaBuf.m);
        }
        ck(vkMapMemory(c.dev, up.m, 0, upBytes, 0, &m), "map up");
        std::memcpy(m, packed.data(), size_t(upBytes));
        vkUnmapMemory(c.dev, up.m);

        VkCommandBuffer cmd = beginOnce(c, pool);
        layout(cmd, inImg.i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy bc{};
        bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bc.imageExtent = {pw, ph, 1};
        vkCmdCopyBufferToImage(cmd, up.b, inImg.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
        layout(cmd, inImg.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        layout(cmd, outImg.i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        submitWait(c, pool, cmd);

        amaze::PipelineConfig cfg{};
        cfg.width = W;
        cfg.height = H;
        cfg.pattern = amaze::BayerPattern(pattern);
        cfg.tileSlots = slots;
        cfg.inputMode = floatInput ? amaze::InputMode::NormalizedFloatBuffer : amaze::InputMode::PackedCfaRgba16fImage;
        amaze::VulkanContext vc{};
        vc.physicalDevice = c.pd;
        vc.device = c.dev;
        vc.queueFamilyIndex = c.qf;
        auto pipeline = std::make_unique<amaze::AmazePipeline>(vc, provider(spvDir), cfg);
        std::cout << "persistentBytes=" << pipeline->currentAllocatedBytes() << "\n";

        amaze::PackedCfaImageView pin{};
        pin.image = inImg.i;
        pin.view = inImg.v;
        pin.width = pw;
        pin.height = ph;
        pin.rawWidth = W;
        pin.rawHeight = H;
        pin.pattern = cfg.pattern;
        amaze::LinearRgbImage pout{};
        pout.image = outImg.i;
        pout.view = outImg.v;
        pout.width = W;
        pout.height = H;
        for (int run = 0; run < 2; ++run) {  // second run is the warm timing
            cmd = beginOnce(c, pool);
            if (floatInput)
                pipeline->record(cmd, amaze::NormalizedBayerBufferView{{cfaBuf.b, 0, VK_WHOLE_SIZE}, W, H, cfg.pattern},
                                 pout, wb);
            else
                pipeline->record(cmd, pin, pout, wb);
            const auto t0 = std::chrono::steady_clock::now();
            submitWait(c, pool, cmd);
            std::cout << "run" << run << "_ms="
                      << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count()
                      << "\n";
        }

        cmd = beginOnce(c, pool);
        layout(cmd, outImg.i, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
               VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy rc{};
        rc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        rc.imageExtent = {W, H, 1};
        vkCmdCopyImageToBuffer(cmd, outImg.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, down.b, 1, &rc);
        submitWait(c, pool, cmd);
        ck(vkMapMemory(c.dev, down.m, 0, downBytes, 0, &m), "map down");
        const uint16_t* h = static_cast<const uint16_t*>(m);
        std::ofstream o(out, std::ios::binary);
        for (size_t i = 0; i < size_t(W) * H; ++i) {
            float rgb[3] = {halfToFloat(h[i * 4]), halfToFloat(h[i * 4 + 1]), halfToFloat(h[i * 4 + 2])};
            o.write(reinterpret_cast<const char*>(rgb), sizeof(rgb));
        }
        vkUnmapMemory(c.dev, down.m);

        pipeline.reset();
        delBuf(c.dev, down);
        if (cfaBuf.b) delBuf(c.dev, cfaBuf);
        delBuf(c.dev, up);
        delImg(c.dev, outImg);
        delImg(c.dev, inImg);
        vkDestroyCommandPool(c.dev, pool, nullptr);
        delCtx(c);
        std::cout << "wrote " << out << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "amaze_validate: " << e.what() << "\n";
        return 1;
    }
}
