#include "pipeline/RawCpuUploadPool.h"

#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>

#include "imaging/Raw16CpuSnapshot.h"
#include "imaging/RawPixelSource.h"
#include "vulkan/Synchronization.h"

namespace rawrcam::pipeline {
namespace {
void vkCheck(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string(what) + " VkResult=" + std::to_string(r));
}
uint32_t hostVisibleType(VkPhysicalDevice physical, uint32_t bits) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(physical, &mp);
    constexpr VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & wanted) == wanted) return i;
    throw std::runtime_error("RAW CPU upload: no host-visible coherent memory type");
}
constexpr uint32_t kParityRows = 8;
uint32_t parityOrigin(uint32_t index, uint32_t height) { return index == 0 ? 0u : (height / 2u) & ~1u; }
}  // namespace

static_assert(rawr::raw_ingress::Raw10Unpacker::kSlots == rawrcam::imaging::kRealtimeFramesInFlight,
              "RAW10 unpacker descriptor slots must match the realtime frame slots");

RawCpuUploadPool::~RawCpuUploadPool() { destroy(); }

void RawCpuUploadPool::initialize(VkPhysicalDevice physical, VkDevice device, uint32_t queueFamily) noexcept {
    if (device != device_) {
        destroy();
        unpacker_.reset();
    }
    physical_ = physical;
    device_ = device;
    queueFamily_ = queueFamily;
}

void RawCpuUploadPool::configure(uint32_t width, uint32_t height) noexcept {
    if (width == width_ && height == height_) return;
    destroy();
    width_ = width;
    height_ = height;
}

void RawCpuUploadPool::destroy() noexcept {
    for (auto& slot : slots_) release(slot);
    releaseParity();
    // Its descriptor sets reference slot images and camera buffers.
    unpacker_.reset();
}

void RawCpuUploadPool::release(Slot& slot) noexcept {
    if (!device_) return;
    if (slot.mapped) vkUnmapMemory(device_, slot.bufferMemory);
    if (slot.buffer) vkDestroyBuffer(device_, slot.buffer, nullptr);
    if (slot.bufferMemory) vkFreeMemory(device_, slot.bufferMemory, nullptr);
    rawrcam::vulkan::destroyOwnedImage(device_, slot.image);
    slot = Slot{};
}

void RawCpuUploadPool::allocateImage(Slot& slot) {
    if (slot.image.image) return;
    slot.image = rawrcam::vulkan::createOwnedImage(physical_, device_, width_, height_, VK_FORMAT_R16_UINT,
                                                   VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                                       VK_IMAGE_USAGE_TRANSFER_DST_BIT);
}

void RawCpuUploadPool::allocateBuffer(Slot& slot) {
    allocateImage(slot);
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(width_) * height_ * sizeof(uint16_t);
    try {
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = bytes;
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCheck(vkCreateBuffer(device_, &bi, nullptr, &slot.buffer), "RAW CPU upload buffer");
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device_, slot.buffer, &req);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = hostVisibleType(physical_, req.memoryTypeBits);
        vkCheck(vkAllocateMemory(device_, &ai, nullptr, &slot.bufferMemory), "RAW CPU upload memory");
        vkCheck(vkBindBufferMemory(device_, slot.buffer, slot.bufferMemory, 0), "RAW CPU upload bind");
        vkCheck(vkMapMemory(device_, slot.bufferMemory, 0, VK_WHOLE_SIZE, 0, &slot.mapped), "RAW CPU upload map");
    } catch (...) {
        release(slot);
        throw;
    }
    slot.raw = {};
    slot.raw.cpuUploaded = true;
    slot.raw.image = slot.image.image;
    slot.raw.memory = slot.image.memory;
    slot.raw.view = slot.image.view;
    slot.raw.importBuffer = slot.buffer;
    slot.raw.importBufferStridePixels = width_;
    slot.raw.importBufferAvailable = true;
    slot.raw.importBufferAttempted = true;
}

rawrcam::vulkan::ImportedRaw& RawCpuUploadPool::upload(uint32_t slotIndex, AImage* image, AHardwareBuffer* ahb,
                                                       int acquireFenceFd) {
    if (!device_ || slotIndex >= slots_.size() || !width_ || !height_)
        throw std::runtime_error("RAW CPU upload: pool not configured");
    if (!image || !ahb) throw std::runtime_error("RAW CPU upload: missing camera image");
    Slot& slot = slots_[slotIndex];
    if (!slot.buffer) allocateBuffer(slot);

    const auto format = rawrcam::imaging::rawPixelFormatOf(image);
    int32_t rowStride = 0;
    if (!format || AImage_getPlaneRowStride(image, 0, &rowStride) != AMEDIA_OK || rowStride <= 0)
        throw std::runtime_error("RAW CPU upload: unsupported camera image layout");
    AHardwareBuffer_Desc desc{};
    AHardwareBuffer_describe(ahb, &desc);
    if (desc.width != width_ || desc.height != height_)
        throw std::runtime_error("RAW CPU upload: frame dimensions do not match configured RAW geometry");
    if (!(desc.usage & AHARDWAREBUFFER_USAGE_CPU_READ_MASK))
        throw std::runtime_error("RAW CPU upload: camera reader is not CPU-readable");

    const int fence = acquireFenceFd >= 0 ? dup(acquireFenceFd) : -1;
    void* address = nullptr;
    const int lockResult = AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, fence, nullptr, &address);
    if (lockResult != 0 || !address)
        throw std::runtime_error("RAW CPU upload: AHB lock failed status=" + std::to_string(lockResult));
    const rawrcam::imaging::RawPixelSource source{static_cast<const uint8_t*>(address), *format, width_, height_,
                                                  static_cast<size_t>(rowStride)};
    const bool copied = rawrcam::imaging::copyRawToPackedRaw16(source, static_cast<uint16_t*>(slot.mapped));
    int releaseFence = -1;
    AHardwareBuffer_unlock(ahb, &releaseFence);
    if (releaseFence >= 0) close(releaseFence);
    if (!copied) throw std::runtime_error("RAW CPU upload: invalid source stride");

    slot.raw.ahb = ahb;
    slot.raw.desc = desc;
    return slot.raw;
}

rawrcam::vulkan::ImportedRaw& RawCpuUploadPool::gpuUnpack(uint32_t slotIndex,
                                                          const rawrcam::vulkan::ImportedRaw& camera) {
    if (!device_ || slotIndex >= slots_.size() || !width_ || !height_)
        throw std::runtime_error("RAW GPU unpack: pool not configured");
    if (!camera.gpuUnpack || !camera.importBufferAvailable || !camera.importBuffer)
        throw std::runtime_error("RAW GPU unpack: camera buffer not imported");
    if (camera.desc.width != width_ || camera.desc.height != height_)
        throw std::runtime_error("RAW GPU unpack: frame dimensions do not match configured RAW geometry");
    if (!unpacker_) unpacker_ = rawr::raw_ingress::Raw10Unpacker::create({physical_, device_, queueFamily_});
    Slot& slot = slots_[slotIndex];
    allocateImage(slot);
    auto& raw = slot.gpuRaw;
    raw = {};
    raw.ahb = camera.ahb;
    raw.desc = camera.desc;
    raw.image = slot.image.image;
    raw.memory = slot.image.memory;
    raw.view = slot.image.view;
    // Consumers read the unpacked image; the packed buffer is not RAW16.
    raw.importBufferAvailable = false;
    raw.importBufferAttempted = true;
    raw.gpuUnpack = true;
    raw.rowStrideBytes = camera.rowStrideBytes;
    raw.unpackSource = camera.importBuffer;
    raw.unpacker = unpacker_.get();
    raw.unpackSlot = slotIndex;
    if (parity_.armed && !parity_.recorded && parity_.slot == slotIndex) {
        const VkBuffer source = camera.importBuffer;
        const VkImage image = raw.image;
        raw.afterUnpack = [this, source, image](VkCommandBuffer command) {
            if (parity_.recorded) return;
            parity_.recorded = true;
            VkImageMemoryBarrier toCopy{};
            toCopy.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            toCopy.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            toCopy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            toCopy.oldLayout = toCopy.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            toCopy.srcQueueFamilyIndex = toCopy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toCopy.image = image;
            toCopy.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &toCopy);
            const VkDeviceSize rowBytes = parity_.strideBytes;
            for (uint32_t i = 0; i < 2; ++i) {
                const uint32_t y = parityOrigin(i, height_);
                VkBufferCopy bc{VkDeviceSize(y) * rowBytes, VkDeviceSize(i) * kParityRows * rowBytes,
                                kParityRows * rowBytes};
                vkCmdCopyBuffer(command, source, parity_.packed, 1, &bc);
                VkBufferImageCopy ic{};
                ic.bufferOffset = VkDeviceSize(i) * kParityRows * width_ * 2u;
                ic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                ic.imageOffset = {0, int32_t(y), 0};
                ic.imageExtent = {width_, kParityRows, 1};
                vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_GENERAL, parity_.unpacked, 1, &ic);
            }
            VkMemoryBarrier toHost{};
            toHost.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &toHost, 0,
                                 nullptr, 0, nullptr);
        };
    }
    return raw;
}

void RawCpuUploadPool::releaseParity() noexcept {
    if (device_) {
        if (parity_.packedMapped) vkUnmapMemory(device_, parity_.packedMemory);
        if (parity_.unpackedMapped) vkUnmapMemory(device_, parity_.unpackedMemory);
        if (parity_.packed) vkDestroyBuffer(device_, parity_.packed, nullptr);
        if (parity_.unpacked) vkDestroyBuffer(device_, parity_.unpacked, nullptr);
        if (parity_.packedMemory) vkFreeMemory(device_, parity_.packedMemory, nullptr);
        if (parity_.unpackedMemory) vkFreeMemory(device_, parity_.unpackedMemory, nullptr);
    }
    parity_ = Parity{};
}

void RawCpuUploadPool::armRaw10Parity(uint32_t slotIndex, AImage* image, AHardwareBuffer* ahb, int acquireFenceFd) {
    int32_t rowStride = 0;
    if (!image || !ahb || AImage_getPlaneRowStride(image, 0, &rowStride) != AMEDIA_OK || rowStride <= 0) return;
    const auto makeHost = [&](VkDeviceSize bytes, VkBuffer& buffer, VkDeviceMemory& memory, void*& mapped) {
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = bytes;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCheck(vkCreateBuffer(device_, &bi, nullptr, &buffer), "RAW10 parity buffer");
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device_, buffer, &req);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = hostVisibleType(physical_, req.memoryTypeBits);
        vkCheck(vkAllocateMemory(device_, &ai, nullptr, &memory), "RAW10 parity memory");
        vkCheck(vkBindBufferMemory(device_, buffer, memory, 0), "RAW10 parity bind");
        vkCheck(vkMapMemory(device_, memory, 0, VK_WHOLE_SIZE, 0, &mapped), "RAW10 parity map");
    };
    try {
        releaseParity();
        parity_.strideBytes = static_cast<uint32_t>(rowStride);
        makeHost(VkDeviceSize(2) * kParityRows * parity_.strideBytes, parity_.packed, parity_.packedMemory,
                 parity_.packedMapped);
        makeHost(VkDeviceSize(2) * kParityRows * width_ * 2u, parity_.unpacked, parity_.unpackedMemory,
                 parity_.unpackedMapped);
        const int fence = acquireFenceFd >= 0 ? dup(acquireFenceFd) : -1;
        void* address = nullptr;
        if (AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, fence, nullptr, &address) != 0 || !address)
            throw std::runtime_error("lock");
        parity_.cpuPacked.resize(size_t(2) * kParityRows * parity_.strideBytes);
        for (uint32_t i = 0; i < 2; ++i)
            std::memcpy(parity_.cpuPacked.data() + size_t(i) * kParityRows * parity_.strideBytes,
                        static_cast<const uint8_t*>(address) + size_t(parityOrigin(i, height_)) * parity_.strideBytes,
                        size_t(kParityRows) * parity_.strideBytes);
        int releaseFence = -1;
        AHardwareBuffer_unlock(ahb, &releaseFence);
        if (releaseFence >= 0) close(releaseFence);
        parity_.armed = true;
        parity_.slot = slotIndex;
    } catch (...) {
        releaseParity();
    }
}

std::string RawCpuUploadPool::takeRaw10ParityReport(uint32_t slotIndex) {
    if (!parity_.armed || !parity_.recorded || parity_.slot != slotIndex) return {};
    const uint32_t stride = parity_.strideBytes;
    const auto* gpuPacked = static_cast<const uint8_t*>(parity_.packedMapped);
    const auto* gpuUnpacked = static_cast<const uint16_t*>(parity_.unpackedMapped);
    size_t packedDiff = 0, firstPacked = SIZE_MAX, unpackDiff = 0, firstUnpack = SIZE_MAX, cpuVsGpuBytes = 0;
    const size_t rowBytes = size_t(width_) / 4u * 5u;
    std::vector<uint16_t> expected(width_);
    for (uint32_t r = 0; r < 2u * kParityRows; ++r) {
        const uint8_t* cpuRow = parity_.cpuPacked.data() + size_t(r) * stride;
        const uint8_t* gpuRow = gpuPacked + size_t(r) * stride;
        for (size_t b = 0; b < rowBytes; ++b)
            if (cpuRow[b] != gpuRow[b] && packedDiff++ == 0) firstPacked = size_t(r) * stride + b;
        // Unpack check against what the GPU itself read, so layout and
        // shader errors are told apart.
        rawrcam::imaging::unpackRaw10Row(gpuRow, width_, expected.data());
        for (uint32_t x = 0; x < width_; ++x)
            if (gpuUnpacked[size_t(r) * width_ + x] != expected[x] && unpackDiff++ == 0)
                firstUnpack = size_t(r) * width_ + x;
        cpuVsGpuBytes += rowBytes;
    }
    std::ostringstream out;
    out << "RAW10_PARITY stride=" << stride << " rows=" << 2u * kParityRows << " packedBytesDiff=" << packedDiff << "/"
        << cpuVsGpuBytes;
    if (firstPacked != SIZE_MAX) {
        out << " firstPacked=(row" << firstPacked / stride << ",byte" << firstPacked % stride << ") cpu=";
        for (size_t i = firstPacked; i < std::min(firstPacked + 8, parity_.cpuPacked.size()); ++i)
            out << std::hex << int(parity_.cpuPacked[i]) << ' ';
        out << "gpu=";
        for (size_t i = firstPacked; i < std::min(firstPacked + 8, parity_.cpuPacked.size()); ++i)
            out << std::hex << int(gpuPacked[i]) << ' ';
        out << std::dec;
    }
    out << " unpackDiff=" << unpackDiff;
    if (firstUnpack != SIZE_MAX) out << " firstUnpack=(" << firstUnpack % width_ << "," << firstUnpack / width_ << ")";
    {
        // First row's mismatches as x:expected/got, to show the pattern.
        rawrcam::imaging::unpackRaw10Row(gpuPacked, width_, expected.data());
        out << " row0:";
        uint32_t shown = 0;
        uint32_t byK[4] = {};
        for (uint32_t x = 0; x < width_; ++x)
            if (gpuUnpacked[x] != expected[x]) {
                ++byK[x % 4u];
                if (shown++ < 10) out << ' ' << x << ':' << expected[x] << '/' << gpuUnpacked[x];
            }
        out << " byK=" << byK[0] << ',' << byK[1] << ',' << byK[2] << ',' << byK[3];
    }
    releaseParity();
    return out.str();
}

void acquireRawInputImage(VkCommandBuffer command, const rawrcam::vulkan::ImportedRaw& raw, uint32_t queueFamily,
                          VkPipelineStageFlags dstStage, VkAccessFlags dstAccess) {
    if (raw.gpuUnpack) {
        rawrcam::vulkan::acquireForeignBuffer(command, raw.unpackSource, queueFamily,
                                              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);
        VkImageMemoryBarrier toWrite{};
        toWrite.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toWrite.srcAccessMask = 0;
        toWrite.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        toWrite.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toWrite.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        toWrite.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toWrite.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toWrite.image = raw.image;
        toWrite.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &toWrite);
        raw.unpacker->record(command, raw.unpackSlot, raw.unpackSource, raw.view, raw.desc.width, raw.desc.height,
                             raw.rowStrideBytes);
        if (raw.afterUnpack) raw.afterUnpack(command);
        VkImageMemoryBarrier toRead = toWrite;
        toRead.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        toRead.dstAccessMask = dstAccess;
        toRead.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, dstStage,
                             0, 0, nullptr, 0, nullptr, 1, &toRead);
        return;
    }
    if (!raw.cpuUploaded) {
        rawrcam::vulkan::acquireForeignImage(command, raw.image, queueFamily, dstStage, dstAccess);
        return;
    }
    // Fill the owned image from this frame's buffer. Contents are fully
    // replaced, so the previous layout can be discarded.
    VkImageMemoryBarrier toTransfer{};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransfer.srcAccessMask = 0;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = raw.image;
    toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &toTransfer);
    VkBufferImageCopy region{};
    region.bufferRowLength = raw.importBufferStridePixels;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {raw.desc.width, raw.desc.height, 1};
    vkCmdCopyBufferToImage(command, raw.importBuffer, raw.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    VkImageMemoryBarrier toRead = toTransfer;
    toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toRead.dstAccessMask = dstAccess;
    toRead.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, dstStage, 0, 0, nullptr, 0, nullptr, 1, &toRead);
}

void releaseRawInputImage(VkCommandBuffer command, const rawrcam::vulkan::ImportedRaw& raw, uint32_t queueFamily,
                          VkPipelineStageFlags srcStage, VkAccessFlags srcAccess) {
    // App-owned images stay with this queue; the slot fence guards reuse.
    // A GPU-unpacked frame hands the camera buffer back after the unpack read it.
    if (raw.gpuUnpack) {
        rawrcam::vulkan::releaseForeignBuffer(command, raw.unpackSource, queueFamily,
                                              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        return;
    }
    if (!raw.cpuUploaded) rawrcam::vulkan::releaseForeignImage(command, raw.image, queueFamily, srcStage, srcAccess);
}

}  // namespace rawrcam::pipeline
