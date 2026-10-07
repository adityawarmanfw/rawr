#pragma once
#include <rawr/raw_gpu_pipeline/AndroidBurstCoordinator.h>
#include <rawr/zsl_ring/RawImageRing.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "capture/multiframe/FrozenBurst.h"
#include "color/FrameColorTransform.h"
#include "metadata/FrameMetadataSnapshot.h"

namespace rawrcam::capture::multiframe {

// Owns the uncompressed RAW16 GPU ring + per-frame metadata/color maps.
// Realtime path pushes; shutter path freezes. Previously inline maps +
// recordPush/markReady logic inside FrameSubmitCoordinator::submitAhb,
// retireZslSlot, prepareExperimentalMultiframeOnShutter.
class MultiframeFrameRing {
   public:
    MultiframeFrameRing(VkPhysicalDevice physical, VkDevice device, std::uint32_t width, std::uint32_t height,
                        std::size_t capacity);
    ~MultiframeFrameRing() = default;

    // Record the just-submitted preview command buffer's RAW copy. Returns false when disabled.
    bool observeSubmit(VkCommandBuffer command, VkImage rawCopyImage, std::uint64_t timestampNs,
                       const rawrcam::metadata::FrameMetadataSnapshot& metadata,
                       const rawrcam::color::FrameColorTransform& color);

    void commitTimestamp(uint64_t timestampNs) noexcept;
    void discardTimestamp(uint64_t timestampNs) noexcept;
    // Mark the frame behind a retired preview slot as snapshot-ready. Returns
    // its ring frame id when it became ready.
    std::optional<std::uint64_t> markReadyForTimestamp(std::uint64_t timestampNs);
    // Metadata recorded for a ring frame (null once pruned).
    const rawrcam::metadata::FrameMetadataSnapshot* metadataFor(std::uint64_t frameId) const;

    // Freeze up to [2, maxFrames] frames for a shutter capture. Returns nullopt
    // when fewer than 2 ready frames exist or any metadata/color/gpu gap is found.
    // Pins immutable images shared by overlapping bursts; live storage remains bounded.
    std::optional<FrozenBurst> freeze(std::size_t maxFrames);
    // Pins one ready frame (by ring frame id) as a single-frame burst.
    std::optional<FrozenBurst> freezeFrame(std::uint64_t frameId);

    std::size_t frameCount() const;
    std::uint64_t usedBytes() const;

   private:
    std::shared_ptr<rawr::zsl_ring::RawImageRing> ring_;
    std::unordered_map<uint64_t, bool> committed_;
    std::uint64_t nextFrameId_ = 1;
    std::uint64_t latestInputTimestampNs_ = 0;
    std::unordered_map<std::uint64_t, rawrcam::metadata::FrameMetadataSnapshot> metadata_;
    std::unordered_map<std::uint64_t, rawrcam::color::FrameColorTransform> color_;
    std::unordered_map<std::uint64_t, std::uint64_t> byTimestamp_;
};
}  // namespace rawrcam::capture::multiframe
