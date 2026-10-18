#pragma once
#include <rawr/raw_gpu_pipeline/AndroidBurstCoordinator.h>
#include <rawr/zsl_ring/RawImageRing.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "color/FrameColorTransform.h"
#include "metadata/FrameMetadataSnapshot.h"
namespace rawrcam::capture::multiframe {
class BracketCollector;
struct FrozenBurst {
    std::shared_ptr<void> recoveryImages;  // Owns uploaded disk-backed inputs through the final GPU fence.
    std::unique_ptr<rawr::zsl_ring::RawImageRing::Snapshot> snapshot;
    // Pins of frames appended after the freeze (HDR+ bracket dark frames).
    std::vector<std::unique_ptr<rawr::zsl_ring::RawImageRing::Snapshot>> extraSnapshots;
    // Set while post-shutter bracket frames are still arriving; the spool
    // worker appends them before the burst becomes durable.
    std::shared_ptr<BracketCollector> pendingBracket;
    // Frames held outside the GPU (full-size bursts): fills the packed RAW16 of
    // frame i (buffer sized by the caller), once, when the burst is journaled;
    // frames[i].raw then carries only its ref. The source owns the camera
    // images until the burst is released.
    std::function<void(std::size_t, std::vector<std::uint8_t>&)> cpuFrame;
    // Journal-backed bursts (lazy load): makes frames[i].raw usable / drops it.
    // Unset: frames are resident.
    std::function<void(std::size_t)> loadFrame;
    std::function<void(std::size_t)> releaseFrame;
    // Journal-backed: later loads read only rows [y0, y0 + rows) of each
    // frame (strip merge); drops every loaded frame. rows 0 = whole frames.
    std::function<void(std::uint32_t y0, std::uint32_t rows)> setRowWindow;
    std::vector<rawr::zsl_ring::RawImageRef> refs;
    std::vector<rawr::raw_gpu_pipeline::BurstFrame> frames;
    std::vector<rawrcam::metadata::FrameMetadataSnapshot> metadata;
    std::vector<rawrcam::color::FrameColorTransform> colors;
    std::uint32_t referenceIndex = 0;
    rawrcam::metadata::FrameMetadataSnapshot referenceMetadata{};
    rawrcam::color::FrameColorTransform referenceColor{};
};
}  // namespace rawrcam::capture::multiframe
