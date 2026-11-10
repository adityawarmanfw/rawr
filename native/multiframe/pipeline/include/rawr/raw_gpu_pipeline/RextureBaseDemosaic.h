#pragma once
#include <rawr/raw_gpu_pipeline/MultiframeRecorder.h>
#include <rawr/raw_gpu_pipeline/ResourceArena.h>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>

namespace rawr::raw_gpu_pipeline {

// RCD demosaic of the base frame (the low-confidence fallback of the Rexture merge), through the unmodified
// native/raw_demosaic RCD pipeline in its RAW-image mode with a fixed white balance. Owns the RCD pipeline and its
// scratch memory; release() frees both so they do not coexist with the companion-frame buffers.
class RextureBaseDemosaic final {
   public:
    RextureBaseDemosaic();
    ~RextureBaseDemosaic();
    RextureBaseDemosaic(const RextureBaseDemosaic&) = delete;
    RextureBaseDemosaic& operator=(const RextureBaseDemosaic&) = delete;

    static bool available() noexcept;
    // Creates (or reuses) the pipeline for this geometry / CFA / balance. Call outside command recording.
    void ensure(VkPhysicalDevice, VkDevice, std::uint32_t queueFamily, std::uint32_t width, std::uint32_t height,
                std::uint32_t cfa, const std::array<float, 3>& whiteBalance);
    // Records the demosaic of rawImage (R16_UINT, GENERAL) into output (RGBA16F, GENERAL, same size).
    void record(VkCommandBuffer, VkImage rawImage, VkImageView rawView, const RawNormalization&, const ArenaImage& output);
    void release() noexcept;
    [[nodiscard]] std::uint64_t allocatedBytes() const noexcept;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace rawr::raw_gpu_pipeline
