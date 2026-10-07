#pragma once
#include <rawr/raw_gpu_pipeline/AndroidBurstCoordinator.h>
#include <rawr/zsl_ring/RawImageRing.h>

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
    std::vector<rawr::zsl_ring::RawImageRef> refs;
    std::vector<rawr::raw_gpu_pipeline::BurstFrame> frames;
    std::vector<rawrcam::metadata::FrameMetadataSnapshot> metadata;
    std::vector<rawrcam::color::FrameColorTransform> colors;
    std::uint32_t referenceIndex = 0;
    rawrcam::metadata::FrameMetadataSnapshot referenceMetadata{};
    rawrcam::color::FrameColorTransform referenceColor{};
};
}  // namespace rawrcam::capture::multiframe
