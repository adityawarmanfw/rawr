#pragma once
// Stage-by-stage dumps of the Rexture merge for the NumPy comparison (tmp/indep/m7/compare.py): reads arena images back
// to float32 and writes <dir>/<name with / -> __>.bin plus manifest.json (same layout as the NumPy reference dumps).
#include <rawr/raw_gpu_pipeline/ResourceArena.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "vk_test_common.hpp"

namespace stagedump {

class StageDump {
   public:
    using Record = std::function<void(VkCommandBuffer)>;
    using Submit = std::function<void(const Record&)>;  // records, submits, waits
    StageDump(const vktest::Ctx& ctx, std::filesystem::path dir, Submit submit)
        : ctx_(ctx), dir_(std::move(dir)), submit_(std::move(submit)) {
        std::filesystem::create_directories(dir_);
    }
    ~StageDump() { finish(); }

    // Dumps channels [0, channels) of an arena image as float32, shape (h, w) for one channel else (h, w, channels).
    void image(const std::string& name, const rawr::raw_gpu_pipeline::ArenaImage& img, std::uint32_t channels) {
        std::uint32_t texel = 0, have = 0;
        bool half = false;
        switch (img.format) {
            case VK_FORMAT_R32_SFLOAT: texel = 4; have = 1; break;
            case VK_FORMAT_R16_SFLOAT: texel = 2; have = 1; half = true; break;
            case VK_FORMAT_R32G32_SFLOAT: texel = 8; have = 2; break;
            case VK_FORMAT_R32G32B32A32_SFLOAT: texel = 16; have = 4; break;
            case VK_FORMAT_R16G16B16A16_SFLOAT: texel = 8; have = 4; half = true; break;
            default: throw std::runtime_error("stage dump: unsupported format for " + name);
        }
        if (channels > have) throw std::runtime_error("stage dump: too many channels for " + name);
        const std::uint32_t w = img.extent.width, h = img.extent.height;
        const VkDeviceSize bytes = VkDeviceSize(w) * h * texel;
        auto buf = vktest::mkBuf(ctx_.pd, ctx_.dev, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        submit_([&](VkCommandBuffer c) {
            barrier(c, img.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {w, h, 1};
            vkCmdCopyImageToBuffer(c, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf.b, 1, &copy);
            barrier(c, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        });
        void* mapped = nullptr;
        vktest::ck(vkMapMemory(ctx_.dev, buf.m, 0, bytes, 0, &mapped), "stage dump map");
        std::vector<float> out(std::size_t(w) * h * channels);
        const auto* src = static_cast<const std::uint8_t*>(mapped);
        const std::uint32_t chBytes = half ? 2u : 4u;
        for (std::size_t i = 0; i < std::size_t(w) * h; ++i)
            for (std::uint32_t k = 0; k < channels; ++k) {
                const std::uint8_t* at = src + i * texel + k * chBytes;
                float v;
                if (half) {
                    std::uint16_t hv;
                    std::memcpy(&hv, at, 2);
                    v = vktest::halfToFloat(hv);
                } else {
                    std::memcpy(&v, at, 4);
                }
                out[i * channels + k] = v;
            }
        vkUnmapMemory(ctx_.dev, buf.m);
        vktest::delBuf(ctx_.dev, buf);
        std::vector<std::uint32_t> shape{h, w};
        if (channels > 1) shape.push_back(channels);
        put(name, out.data(), out.size() * sizeof(float), "float32", shape);
    }

    void put(const std::string& name, const void* data, std::size_t bytes, const std::string& dtype,
             const std::vector<std::uint32_t>& shape) {
        std::string file = name;
        for (auto& ch : file)
            if (ch == '/') ch = '_';
        // the NumPy side writes "a/b" as "a__b"
        std::string fname;
        for (const char ch : name) fname += ch == '/' ? std::string("__") : std::string(1, ch);
        std::ofstream f(dir_ / (fname + ".bin"), std::ios::binary | std::ios::trunc);
        f.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
        if (!f) throw std::runtime_error("stage dump: cannot write " + fname);
        std::ostringstream s;
        s << "{\"shape\": [";
        for (std::size_t i = 0; i < shape.size(); ++i) s << (i ? ", " : "") << shape[i];
        s << "], \"dtype\": \"" << dtype << "\", \"file\": \"" << fname << ".bin\"}";
        entries_[name] = s.str();
    }

    void finish() {
        if (finished_) return;
        finished_ = true;
        std::ofstream m(dir_ / "manifest.json", std::ios::trunc);
        m << "{\n";
        bool first = true;
        for (const auto& [k, v] : entries_) {
            m << (first ? "" : ",\n") << " \"" << k << "\": " << v;
            first = false;
        }
        m << "\n}\n";
    }

   private:
    static void barrier(VkCommandBuffer c, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src,
                        VkAccessFlags dst, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = src;
        b.dstAccessMask = dst;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(c, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
    }
    vktest::Ctx ctx_;
    std::filesystem::path dir_;
    Submit submit_;
    std::map<std::string, std::string> entries_;
    bool finished_ = false;
};

}  // namespace stagedump
