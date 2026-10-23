#include <cassert>
#include <cmath>
#include <iostream>

#include "color/ColorMath.h"
#include "color/DcpRenderData.h"
#include "develop/render/DcpRenderStage.h"
#include "vk_test_common.hpp"
using namespace rawrcam;
int main() {
    auto ctx = vktest::ctx();
    const auto image =
        vktest::mkImg(ctx.pd, ctx.dev, 1, 1, VK_FORMAT_R16G16B16A16_SFLOAT,
                      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    auto readback = vktest::mkBuf(ctx.pd, ctx.dev, 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkCommandPool pool;
    VkCommandPoolCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    ci.queueFamilyIndex = ctx.qf;
    ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vktest::ck(vkCreateCommandPool(ctx.dev, &ci, nullptr, &pool), "pool");
    VkCommandBuffer command;
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    vktest::ck(vkAllocateCommandBuffers(ctx.dev, &ai, &command), "command");
    {
        develop::rendered::DcpRenderStage stage(ctx.pd, ctx.dev);
        using namespace color::math;
        const Matrix3 proPhotoToD50{.7976749f, .1351917f, .0313534f, .2880402f, .7118741f, .0000857f, 0, 0, .82521f};
        const auto matrix = multiply(kXyzD65ToLinearSrgb, multiply(kD50ToD65Bradford, proPhotoToD50));
        const auto run = [&](const color::DcpProfile& profile, float weight, Vec3 expected) {
            vktest::ck(vkResetCommandBuffer(command, 0), "reset");
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            vktest::ck(vkBeginCommandBuffer(command, &begin), "begin");
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image.i;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &barrier);
            const VkClearColorValue clear{{.5f, .25f, .125f, .75f}};
            vkCmdClearColorImage(command, image.i, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &barrier.subresourceRange);
            barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &barrier);
            stage.record(command, std::make_shared<const color::DcpProfile>(profile), matrix, weight, 1, 1, image.v);
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {1, 1, 1};
            vkCmdCopyImageToBuffer(command, image.i, VK_IMAGE_LAYOUT_GENERAL, readback.b, 1, &copy);
            vktest::ck(vkEndCommandBuffer(command), "end");
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &command;
            vktest::ck(vkQueueSubmit(ctx.q, 1, &submit, VK_NULL_HANDLE), "submit");
            vktest::ck(vkQueueWaitIdle(ctx.q), "wait");
            void* mapped;
            vktest::ck(vkMapMemory(ctx.dev, readback.m, 0, 8, 0, &mapped), "read");
            const auto* half = static_cast<const uint16_t*>(mapped);
            for (size_t c = 0; c < 3; ++c) {
                const float actual = vktest::halfToFloat(half[c]);
                if (std::abs(actual - expected[c]) > .001f) {
                    std::cerr << "channel " << c << " expected " << expected[c] << " got " << actual << '\n';
                    std::abort();
                }
            }
            assert(vktest::halfToFloat(half[3]) == .75f);
            vkUnmapMemory(ctx.dev, readback.m);
        };
        color::DcpProfile p;
        run(p, 1, {.5f, .25f, .125f});
        p.baselineExposureOffset = 1;
        run(p, 1, {1, .5f, .25f});
        p.baselineExposureOffset.reset();
        p.toneCurve = {0, 0, 1, .5f};
        run(p, 1, {.25f, .125f, .0625f});
        p.toneCurve = {0, 0, .5f, .75f, 1, 1};
        const auto packed = color::dcpRenderData(p);
        assert(std::abs(packed[size_t(packed[15]) + 2048] - .75f) < 1e-6f);
        p.toneCurve.clear();
        auto table = [](float hue, float sat, uint32_t vals = 1) {
            color::DcpHueSatTable t;
            t.hues = 6;
            t.sats = 2;
            t.vals = vals;
            for (uint32_t i = 0; i < t.hues * t.sats * t.vals; ++i) t.data.insert(t.data.end(), {hue, sat, 1});
            return t;
        };
        p.hueSatMap1 = table(60, 1);
        p.hueSatMap2 = table(-60, 1);
        run(p, 1, {.375f, .5f, .125f});
        run(p, 0, {.5f, .125f, .375f});
        run(p, .5f, {.5f, .25f, .125f});
        p.hueSatMap1 = {};
        p.hueSatMap2 = {};
        p.lookTable = table(0, 0);
        run(p, 1, {.5f, .5f, .5f});
        p.lookTable = table(0, 1, 2);
        p.lookTable.encoding = 1;
        for (size_t i = 2; i < p.lookTable.data.size(); i += 3) p.lookTable.data[i] = .5f;
        const float encoded = (1.055f * std::pow(.5f, 1 / 2.4f) - .055f) * .5f;
        const float value = std::pow((encoded + .055f) / 1.055f, 2.4f);
        run(p, 1, {value, value * .5f, value * .25f});
        // The half-value pixel interpolates halfway between value slices.
        p.lookTable = table(0, 1, 2);
        for (size_t i = 6 * 2 * 3; i < p.lookTable.data.size(); i += 3) p.lookTable.data[i] = 120;
        run(p, 1, {.375f, .5f, .125f});
    }
    auto owned = image;
    vktest::delImg(ctx.dev, owned);
    vktest::delBuf(ctx.dev, readback);
    vkDestroyCommandPool(ctx.dev, pool, nullptr);
    vktest::delCtx(ctx);
    std::cout << "DCP GPU rendering passed\n";
}
