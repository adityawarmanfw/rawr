// Deterministic GPU transfer -> CPU visibility check for cached/noncoherent
// readback. Also measures CPU byte reads independently of camera scene changes.
#include "vulkan/HostMemory.h"
#include "vk_test_common.hpp"
#include <chrono>
#include <cstdio>
#include <vector>

int main() {
    using namespace vktest;
    try {
        auto c = ctx();
        VkPhysicalDeviceMemoryProperties properties{};
        vkGetPhysicalDeviceMemoryProperties(c.pd, &properties);
        constexpr VkDeviceSize bytes = 16u * 1024u * 1024u;
        VkCommandPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pi.queueFamilyIndex = c.qf;
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VkCommandPool pool{};
        ck(vkCreateCommandPool(c.dev, &pi, nullptr, &pool), "pool");
        VkCommandBufferAllocateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ci.commandPool = pool;
        ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ci.commandBufferCount = 1;
        VkCommandBuffer command{};
        ck(vkAllocateCommandBuffers(c.dev, &ci, &command), "command");
        for (const bool cached : {false, true}) {
            VkBufferCreateInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bi.size = bytes;
            bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            VkBuffer buffer{};
            ck(vkCreateBuffer(c.dev, &bi, nullptr, &buffer), "buffer");
            VkMemoryRequirements req{};
            vkGetBufferMemoryRequirements(c.dev, buffer, &req);
            const auto type = rawrcam::vulkan::selectReadbackMemoryType(properties, req.memoryTypeBits, cached);
            VkMemoryAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = type.index;
            VkDeviceMemory memory{};
            ck(vkAllocateMemory(c.dev, &ai, nullptr, &memory), "memory");
            ck(vkBindBufferMemory(c.dev, buffer, memory, 0), "bind");
            void* mapped{};
            ck(vkMapMemory(c.dev, memory, 0, VK_WHOLE_SIZE, 0, &mapped), "map");
            for (uint32_t pass = 0; pass < 3; ++pass) {
                ck(vkResetCommandBuffer(command, 0), "reset");
                VkCommandBufferBeginInfo begin{};
                begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                ck(vkBeginCommandBuffer(command, &begin), "begin");
                const uint32_t pattern = 0x12345678u + pass;
                vkCmdFillBuffer(command, buffer, 0, bytes, pattern);
                VkMemoryBarrier barrier{};
                barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                                     0, 1, &barrier, 0, nullptr, 0, nullptr);
                ck(vkEndCommandBuffer(command), "end");
                VkFenceCreateInfo fi{};
                fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
                VkFence fence{};
                ck(vkCreateFence(c.dev, &fi, nullptr, &fence), "fence");
                VkSubmitInfo submit{};
                submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                submit.commandBufferCount = 1;
                submit.pCommandBuffers = &command;
                ck(vkQueueSubmit(c.q, 1, &submit, fence), "submit");
                ck(vkWaitForFences(c.dev, 1, &fence, VK_TRUE, UINT64_MAX), "wait");
                if (!type.coherent) {
                    VkMappedMemoryRange range{};
                    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
                    range.memory = memory;
                    range.size = VK_WHOLE_SIZE;
                    ck(vkInvalidateMappedMemoryRanges(c.dev, 1, &range), "invalidate");
                }
                const auto start = std::chrono::steady_clock::now();
                uint64_t sum = 0;
                const auto* data = static_cast<const volatile unsigned char*>(mapped);
                for (VkDeviceSize i = 0; i < bytes; ++i) sum += data[i];
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                const uint64_t expected = uint64_t((pattern & 255u) + ((pattern >> 8) & 255u) +
                    ((pattern >> 16) & 255u) + (pattern >> 24)) * (bytes / 4);
                if (sum != expected) throw std::runtime_error("readback bytes differ");
                std::printf("GPU=%s cachedRequested=%d type=%u coherent=%d pass=%u bytes=%llu cpuReadMs=%.3f exact=PASS\n",
                            c.prop.deviceName, cached ? 1 : 0, type.index, type.coherent ? 1 : 0, pass,
                            static_cast<unsigned long long>(bytes), ms);
                vkDestroyFence(c.dev, fence, nullptr);
            }
            vkUnmapMemory(c.dev, memory);
            vkDestroyBuffer(c.dev, buffer, nullptr);
            vkFreeMemory(c.dev, memory, nullptr);
        }
        vkDestroyCommandPool(c.dev, pool, nullptr);
        delCtx(c);
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
