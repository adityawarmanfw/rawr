#pragma once

#include <android/hardware_buffer.h>
#include <media/NdkImage.h>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "imaging/FrameLimits.h"
#include "rawr/raw_ingress/Raw10Unpacker.h"
#include "vulkan/ImageResources.h"
#include "vulkan/RawAhbImporter.h"

namespace rawrcam::pipeline {

// CPU ingress for camera frames the GPU can't import directly: RAW10 streams,
// and RAW16 AHBs whose Vulkan import the driver rejects. The camera frame is
// locked for CPU read and copied (RAW10 unpacked) into a per-slot host-visible
// buffer laid out exactly like the imported RAW buffer (stride = width), so
// the realtime recorders consume it unchanged through ImportedRaw. An owned
// R16 image is filled from that buffer on GPU when a recorder acquires it.
//
// RAW10 normally skips the CPU: gpuUnpack() hands out the same owned R16
// image, filled by unpacking the imported camera buffer on the GPU
// (native/raw_ingress) when a recorder acquires it. upload() stays as the
// fallback for drivers that refuse the buffer import.
class RawCpuUploadPool final {
   public:
    RawCpuUploadPool() = default;
    ~RawCpuUploadPool();
    RawCpuUploadPool(const RawCpuUploadPool&) = delete;
    RawCpuUploadPool& operator=(const RawCpuUploadPool&) = delete;

    void initialize(VkPhysicalDevice physical, VkDevice device, uint32_t queueFamily) noexcept;
    // Resources are allocated lazily per slot on first upload.
    void configure(uint32_t width, uint32_t height) noexcept;
    void destroy() noexcept;

    // Copies the camera frame into the slot's buffer. The caller must have
    // retired the slot's previous GPU work. acquireFenceFd stays owned by the caller.
    rawrcam::vulkan::ImportedRaw& upload(uint32_t slotIndex, AImage* image, AHardwareBuffer* ahb, int acquireFenceFd);
    // Routes a RAW10 camera buffer import (RawAhbImporter::importRaw10Buffer)
    // to the slot's owned image through the GPU unpacker. Same retirement
    // contract as upload(). Throws if the unpacker can't be created.
    rawrcam::vulkan::ImportedRaw& gpuUnpack(uint32_t slotIndex, const rawrcam::vulkan::ImportedRaw& camera);
    // Debug-only GPU/CPU parity check for the RAW10 unpack. arm() CPU-locks the
    // camera frame and keeps sample rows; the next gpuUnpack of that slot
    // records GPU readbacks; once the slot comes round again (retired),
    // takeRaw10ParityReport() compares them. Returns "" when nothing is ready.
    bool raw10ParityIdle() const noexcept { return !parity_.armed; }
    void armRaw10Parity(uint32_t slotIndex, AImage* image, AHardwareBuffer* ahb, int acquireFenceFd);
    std::string takeRaw10ParityReport(uint32_t slotIndex);

   private:
    struct Slot {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory bufferMemory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        rawrcam::vulkan::OwnedImage image;
        rawrcam::vulkan::ImportedRaw raw;
        rawrcam::vulkan::ImportedRaw gpuRaw;
    };
    void allocateImage(Slot& slot);
    void allocateBuffer(Slot& slot);
    void release(Slot& slot) noexcept;

    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    std::unique_ptr<rawr::raw_ingress::Raw10Unpacker> unpacker_;
    struct Parity {
        bool armed = false, recorded = false;
        uint32_t slot = 0, strideBytes = 0;
        std::vector<uint8_t> cpuPacked;  // kParityRows rows at each origin
        VkBuffer packed = VK_NULL_HANDLE, unpacked = VK_NULL_HANDLE;
        VkDeviceMemory packedMemory = VK_NULL_HANDLE, unpackedMemory = VK_NULL_HANDLE;
        void* packedMapped = nullptr;
        void* unpackedMapped = nullptr;
    } parity_;
    void releaseParity() noexcept;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    std::array<Slot, rawrcam::imaging::kRealtimeFramesInFlight> slots_{};
};

// Barriers for the frame's RAW image. Camera AHB imports are foreign-owned and
// use queue-family ownership transfers; CPU-uploaded frames are app-owned and
// are filled from their buffer instead; GPU-unpacked frames acquire the
// foreign RAW10 buffer and unpack it into the owned image.
void acquireRawInputImage(VkCommandBuffer command, const rawrcam::vulkan::ImportedRaw& raw, uint32_t queueFamily,
                          VkPipelineStageFlags dstStage, VkAccessFlags dstAccess);
void releaseRawInputImage(VkCommandBuffer command, const rawrcam::vulkan::ImportedRaw& raw, uint32_t queueFamily,
                          VkPipelineStageFlags srcStage, VkAccessFlags srcAccess);

}  // namespace rawrcam::pipeline
