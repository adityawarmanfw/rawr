#pragma once
#include <cstdint>
#include <map>

#include "color/FrameColorTransform.h"
#include "imaging/RawFrameLease.h"
#include "metadata/FrameMetadataSnapshot.h"

namespace rawrcam::imaging {
struct FrameRenderMetadata {
    metadata::FrameMetadataSnapshot snapshot{};
    color::FrameColorTransform colorState{};
};
struct MatchedFrame {
    uint64_t timestampNs = 0;
    RawFrameLease image{AcquiredRawFrame{}};
    FrameRenderMetadata metadata{};
};
// Unmatched images are leases: replacement, trim, and destruction release them.
class FramePairer {
   public:
    void putImage(uint64_t timestampNs, RawFrameLease image);
    void putMetadata(uint64_t timestampNs, FrameRenderMetadata metadata);
    bool takeMatched(uint64_t timestampNs, MatchedFrame* out);
    uint64_t trim(size_t maxPendingImages, size_t maxPendingMetadata);
    void clear();
    size_t imageCount() const noexcept { return images_.size(); }
    size_t metadataCount() const noexcept { return metadata_.size(); }

   private:
    std::map<uint64_t, RawFrameLease> images_;
    std::map<uint64_t, FrameRenderMetadata> metadata_;
};
}  // namespace rawrcam::imaging
