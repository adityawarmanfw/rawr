#include <rawr/raw_gpu_pipeline/ResourceArena.h>

#include <algorithm>
#include <array>
#include <map>
#include <stdexcept>
#include <vector>
namespace rawr::raw_gpu_pipeline {
namespace {
void ck(VkResult r, const char* s) {
    if (r != VK_SUCCESS) throw std::runtime_error(s);
}
uint32_t mt(VkPhysicalDevice p, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties m{};
    vkGetPhysicalDeviceMemoryProperties(p, &m);
    for (uint32_t i = 0; i < m.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (m.memoryTypes[i].propertyFlags & want) == want) return i;
    for (uint32_t i = 0; i < m.memoryTypeCount; ++i)
        if (bits & (1u << i)) return i;
    throw std::runtime_error("multiframe arena: no compatible memory type");
}
VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment) {
    return alignment <= 1u ? value : ((value + alignment - 1u) / alignment) * alignment;
}
}  // namespace
VkFormat pixelStorageFormat(PixelStorage s) {
    switch (s) {
        case PixelStorage::R8Uint:
            return VK_FORMAT_R8_UINT;
        case PixelStorage::R16Uint:
            return VK_FORMAT_R16_UINT;
        case PixelStorage::R16Float:
            return VK_FORMAT_R16_SFLOAT;
        case PixelStorage::R32Float:
            return VK_FORMAT_R32_SFLOAT;
        case PixelStorage::RG32Float:
            return VK_FORMAT_R32G32_SFLOAT;
        case PixelStorage::RGBA16Float:
            return VK_FORMAT_R16G16B16A16_SFLOAT;
        case PixelStorage::RGBA32Float:
            return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
    throw std::invalid_argument("multiframe arena: invalid pixel storage");
}
void ResourceArena::initialize(VkPhysicalDevice p, VkDevice d, const ScratchLayout& l) {
    reset();
    if (!p || !d) throw std::invalid_argument("multiframe arena: null Vulkan handle");
    physical_ = p;
    device_ = d;
    try {
        struct PendingImage {
            std::string name;
            VkMemoryRequirements requirements{};
            std::uint32_t memoryType = 0;
            VkDeviceSize offset = 0;
            bool dedicated = false;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            std::string pool, placeAt;
        };
        std::map<std::string, const ImagePlacement*> placementOf;
        for (const auto& pl : l.placements) placementOf[pl.name] = &pl;
        std::vector<PendingImage> pendingImages;
        for (const auto& s : l.images) {
            ArenaImage a{};
            a.extent = s.extent;
            a.format = pixelStorageFormat(s.storage);
            VkFormatProperties fp{};
            vkGetPhysicalDeviceFormatProperties(p, a.format, &fp);
            if ((fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) == 0)
                throw std::runtime_error("multiframe arena: storage-image format unsupported: " + s.name);
            VkImageCreateInfo ci{};
            ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            ci.imageType = VK_IMAGE_TYPE_2D;
            ci.format = a.format;
            ci.extent = {s.extent.width, s.extent.height, 1};
            ci.mipLevels = 1;
            ci.arrayLayers = 1;
            ci.samples = VK_SAMPLE_COUNT_1_BIT;
            ci.tiling = VK_IMAGE_TILING_OPTIMAL;
            ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            ck(vkCreateImage(d, &ci, nullptr, &a.image), "multiframe arena: create image");
            VkMemoryDedicatedRequirements dedicated{};
            dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;
            VkMemoryRequirements2 requirements{};
            requirements.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
            requirements.pNext = &dedicated;
            VkImageMemoryRequirementsInfo2 requirementsInfo{};
            requirementsInfo.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2;
            requirementsInfo.image = a.image;
            if (vkGetImageMemoryRequirements2) {
                vkGetImageMemoryRequirements2(d, &requirementsInfo, &requirements);
            } else {
                vkGetImageMemoryRequirements(d, a.image, &requirements.memoryRequirements);
            }
            const auto& mr = requirements.memoryRequirements;
            a.allocationBytes = mr.size;
            if (!images_.emplace(s.name, a).second) throw std::runtime_error("multiframe arena: duplicate image name");
            pendingImages.push_back(
                {s.name, mr, mt(p, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT), 0u,
                 dedicated.requiresDedicatedAllocation == VK_TRUE, VK_NULL_HANDLE, placementOf.count(s.name) ? placementOf[s.name]->pool : std::string(),
                 placementOf.count(s.name) ? placementOf[s.name]->placeAt : std::string()});
        }
        constexpr VkDeviceSize kImageBlockTargetBytes = 128ull * 1024ull * 1024ull;
        for (std::uint32_t type = 0; type < VK_MAX_MEMORY_TYPES; ++type) {
            std::vector<PendingImage*> block;
            VkDeviceSize cursor = 0u;
            const auto allocateBlock = [&]() {
                if (block.empty()) return;
                VkMemoryAllocateInfo ai{};
                ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                ai.allocationSize = cursor;
                ai.memoryTypeIndex = type;
                VkDeviceMemory memory = VK_NULL_HANDLE;
                ck(vkAllocateMemory(d, &ai, nullptr, &memory), "multiframe arena: allocate image block");
                imageMemoryBlocks_.push_back(memory);
                physicalImageBytes_ += cursor;
                for (auto* pending : block) pending->memory = memory;
                block.clear();
                cursor = 0u;
            };
            for (auto& pending : pendingImages) {
                if (pending.dedicated || pending.memoryType != type || !pending.pool.empty()) continue;
                VkDeviceSize offset = alignUp(cursor, pending.requirements.alignment);
                if (!block.empty() && offset + pending.requirements.size > kImageBlockTargetBytes) {
                    allocateBlock();
                    offset = 0u;
                }
                pending.offset = offset;
                cursor = offset + pending.requirements.size;
                block.push_back(&pending);
            }
            allocateBlock();
        }
        for (auto& pending : pendingImages) {
            if (!pending.dedicated) continue;
            VkMemoryDedicatedAllocateInfo dedicatedInfo{};
            dedicatedInfo.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
            dedicatedInfo.image = images_.at(pending.name).image;
            VkMemoryAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            ai.pNext = &dedicatedInfo;
            ai.allocationSize = pending.requirements.size;
            ai.memoryTypeIndex = pending.memoryType;
            ck(vkAllocateMemory(d, &ai, nullptr, &pending.memory), "multiframe arena: allocate dedicated image");
            imageMemoryBlocks_.push_back(pending.memory);
            physicalImageBytes_ += pending.requirements.size;
        }
        // Pools: one allocation per pool, members placed by their placeAt rule (aliasing images with disjoint lifetimes).
        {
            std::map<std::string, std::vector<PendingImage*>> pools;
            for (auto& pending : pendingImages)
                if (!pending.pool.empty()) pools[pending.pool].push_back(&pending);
            for (auto& [poolName, members] : pools) {
                std::map<std::string, PendingImage*> byName;
                VkDeviceSize total = 0, alignment = 1;
                std::uint32_t type = members.front()->memoryType;
                std::uint32_t typeBits = ~0u;
                for (auto* m : members) {
                    if (m->dedicated) throw std::runtime_error("multiframe arena: pooled image requires dedicated memory");
                    typeBits &= m->requirements.memoryTypeBits;
                    alignment = std::max(alignment, m->requirements.alignment);
                }
                if (!(typeBits & (1u << type))) throw std::runtime_error("multiframe arena: pool members need different memory types");
                for (auto* m : members) {
                    VkDeviceSize offset = 0;
                    const std::string& at = m->placeAt;
                    if (at.rfind("start:", 0) == 0) {
                        offset = byName.at(at.substr(6))->offset;
                    } else if (at.rfind("end:", 0) == 0) {
                        std::string list = at.substr(4);
                        std::size_t pos = 0;
                        while (pos <= list.size()) {
                            const auto comma = list.find(',', pos);
                            const std::string n = list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                            const auto* o = byName.at(n);
                            offset = std::max(offset, o->offset + o->requirements.size);
                            if (comma == std::string::npos) break;
                            pos = comma + 1;
                        }
                        offset = alignUp(offset, m->requirements.alignment);
                    }
                    m->offset = offset;
                    byName[m->name] = m;
                    total = std::max(total, offset + m->requirements.size);
                }
                VkMemoryAllocateInfo ai{};
                ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                ai.allocationSize = alignUp(total, alignment);
                ai.memoryTypeIndex = type;
                VkDeviceMemory memory = VK_NULL_HANDLE;
                ck(vkAllocateMemory(d, &ai, nullptr, &memory), "multiframe arena: allocate image pool");
                imageMemoryBlocks_.push_back(memory);
                physicalImageBytes_ += ai.allocationSize;
                for (auto* m : members) m->memory = memory;
            }
        }
        for (const auto& pending : pendingImages) {
            auto& a = images_.at(pending.name);
            a.memory = pending.memory;
            ck(vkBindImageMemory(d, a.image, a.memory, pending.offset), "multiframe arena: bind image");
            VkImageViewCreateInfo vi{};
            vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vi.image = a.image;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = a.format;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            ck(vkCreateImageView(d, &vi, nullptr, &a.view), "multiframe arena: image view");
        }

        struct PendingBuffer {
            std::string name;
            VkMemoryRequirements requirements{};
            std::uint32_t memoryType = 0;
            VkDeviceSize offset = 0;
            bool dedicated = false;
            VkDeviceMemory memory = VK_NULL_HANDLE;
        };
        std::vector<PendingBuffer> pendingBuffers;
        for (const auto& s : l.buffers) {
            ArenaBuffer a{};
            a.bytes = s.bytes;
            VkBufferCreateInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bi.size = s.bytes;
            bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                       VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            ck(vkCreateBuffer(d, &bi, nullptr, &a.buffer), "multiframe arena: create buffer");
            VkMemoryDedicatedRequirements dedicated{};
            dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;
            VkMemoryRequirements2 requirements{};
            requirements.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
            requirements.pNext = &dedicated;
            VkBufferMemoryRequirementsInfo2 requirementsInfo{};
            requirementsInfo.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2;
            requirementsInfo.buffer = a.buffer;
            if (vkGetBufferMemoryRequirements2) {
                vkGetBufferMemoryRequirements2(d, &requirementsInfo, &requirements);
            } else {
                vkGetBufferMemoryRequirements(d, a.buffer, &requirements.memoryRequirements);
            }
            const auto& mr = requirements.memoryRequirements;
            a.allocationBytes = mr.size;
            if (!buffers_.emplace(s.name, a).second)
                throw std::runtime_error("multiframe arena: duplicate buffer name");
            pendingBuffers.push_back(
                {s.name, mr, mt(p, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT), 0u,
                 dedicated.requiresDedicatedAllocation == VK_TRUE, VK_NULL_HANDLE});
        }
        std::array<VkDeviceSize, VK_MAX_MEMORY_TYPES> bufferBlockBytes{};
        for (auto& pending : pendingBuffers) {
            if (pending.dedicated) continue;
            auto& cursor = bufferBlockBytes[pending.memoryType];
            pending.offset = alignUp(cursor, pending.requirements.alignment);
            cursor = pending.offset + pending.requirements.size;
        }
        for (auto& pending : pendingBuffers) {
            if (!pending.dedicated) continue;
            VkMemoryDedicatedAllocateInfo dedicatedInfo{};
            dedicatedInfo.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
            dedicatedInfo.buffer = buffers_.at(pending.name).buffer;
            VkMemoryAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            ai.pNext = &dedicatedInfo;
            ai.allocationSize = pending.requirements.size;
            ai.memoryTypeIndex = pending.memoryType;
            ck(vkAllocateMemory(d, &ai, nullptr, &pending.memory), "multiframe arena: allocate dedicated buffer");
            bufferMemoryBlocks_.push_back(pending.memory);
            physicalBufferBytes_ += pending.requirements.size;
        }
        std::array<VkDeviceMemory, VK_MAX_MEMORY_TYPES> bufferBlocks{};
        for (std::uint32_t type = 0; type < VK_MAX_MEMORY_TYPES; ++type) {
            if (bufferBlockBytes[type] == 0u) continue;
            VkMemoryAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            ai.allocationSize = bufferBlockBytes[type];
            ai.memoryTypeIndex = type;
            ck(vkAllocateMemory(d, &ai, nullptr, &bufferBlocks[type]), "multiframe arena: allocate buffer block");
            bufferMemoryBlocks_.push_back(bufferBlocks[type]);
            physicalBufferBytes_ += bufferBlockBytes[type];
        }
        for (const auto& pending : pendingBuffers) {
            auto& a = buffers_.at(pending.name);
            a.memory = pending.dedicated ? pending.memory : bufferBlocks[pending.memoryType];
            ck(vkBindBufferMemory(d, a.buffer, a.memory, pending.offset), "multiframe arena: bind buffer");
        }
    } catch (...) {
        reset();
        throw;
    }
}
void ResourceArena::recordInitializeLayouts(VkCommandBuffer c) const {
    for (const auto& kv : images_) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.image = kv.second.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &b);
    }
}
void ResourceArena::recordClearBurstState(VkCommandBuffer c) const {
    const VkClearColorValue z{};
    for (const char* n : {"support", "rgb_sum", "rgb_weight", "rgb_weight_square_b"}) {
        auto it = images_.find(n);
        if (it == images_.end()) continue;
        VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(c, it->second.image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &r);
    }
    VkMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b, 0, nullptr,
                         0, nullptr);
}
void ResourceArena::recordPrepareCompanionAlignment(VkCommandBuffer c) const {
    const auto flow = images_.find("tile_flow_l0"), valid = images_.find("tile_valid");
    if (flow == images_.end() || valid == images_.end())
        throw std::logic_error("multiframe arena: alignment state missing");
    VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const VkClearColorValue zero{};
    VkClearColorValue one{};
    one.uint32[0] = 1u;
    vkCmdClearColorImage(c, flow->second.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &r);
    // Frozen ReferenceParity validation corpus reports invalid=0 for every aligned tile;
    // preserve that baseline contract explicitly instead of deriving a new LK-validity rule.
    vkCmdClearColorImage(c, valid->second.image, VK_IMAGE_LAYOUT_GENERAL, &one, 1, &r);
}
const ArenaImage& ResourceArena::image(const std::string& n) const {
    auto i = images_.find(n);
    if (i == images_.end()) throw std::out_of_range("multiframe arena: unknown image " + n);
    return i->second;
}
const ArenaBuffer& ResourceArena::buffer(const std::string& n) const {
    auto i = buffers_.find(n);
    if (i == buffers_.end()) throw std::out_of_range("multiframe arena: unknown buffer " + n);
    return i->second;
}
void ResourceArena::reset() noexcept {
    if (device_) {
        for (auto& kv : images_) {
            auto& a = kv.second;
            if (a.view) vkDestroyImageView(device_, a.view, nullptr);
            if (a.image) vkDestroyImage(device_, a.image, nullptr);
        }
        for (auto& kv : buffers_) {
            auto& a = kv.second;
            if (a.buffer) vkDestroyBuffer(device_, a.buffer, nullptr);
        }
        for (auto memory : imageMemoryBlocks_)
            if (memory) vkFreeMemory(device_, memory, nullptr);
        for (auto memory : bufferMemoryBlocks_)
            if (memory) vkFreeMemory(device_, memory, nullptr);
    }
    images_.clear();
    buffers_.clear();
    imageMemoryBlocks_.clear();
    bufferMemoryBlocks_.clear();
    physical_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    physicalImageBytes_ = physicalBufferBytes_ = 0;
}
}  // namespace rawr::raw_gpu_pipeline
