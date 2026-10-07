#include "capture/multiframe/MultiframeFrameRing.h"

#include <algorithm>
#include <iterator>

#include "color/WhiteBalance.h"
#include "geometry/CfaPattern.h"

namespace rawrcam::capture::multiframe {

MultiframeFrameRing::MultiframeFrameRing(VkPhysicalDevice physical, VkDevice device, std::uint32_t width,
                                         std::uint32_t height, std::size_t capacity)
    : ring_(std::make_shared<rawr::zsl_ring::RawImageRing>(physical, device, width, height, capacity)) {}

bool MultiframeFrameRing::observeSubmit(VkCommandBuffer command, VkImage rawCopyImage, std::uint64_t timestampNs,
                                        const rawrcam::metadata::FrameMetadataSnapshot& metadata,
                                        const rawrcam::color::FrameColorTransform& color) {
    latestInputTimestampNs_ = timestampNs;
    if (!ring_) return false;
    const std::uint64_t frameId = nextFrameId_++;
    const auto ref = ring_->recordPush(command, frameId, timestampNs, rawCopyImage, VK_IMAGE_LAYOUT_GENERAL);
    if (!ref) return false;
    byTimestamp_[timestampNs] = frameId;
    committed_[timestampNs] = false;
    metadata_[frameId] = metadata;
    color_[frameId] = color;
    if (frameId > 64u) {
        const std::uint64_t oldestRetained = frameId - 64u;
        for (auto it = metadata_.begin(); it != metadata_.end();) {
            it = it->first < oldestRetained ? metadata_.erase(it) : std::next(it);
        }
        for (auto it = color_.begin(); it != color_.end();) {
            it = it->first < oldestRetained ? color_.erase(it) : std::next(it);
        }
    }
    return true;
}

void MultiframeFrameRing::commitTimestamp(uint64_t timestampNs) noexcept {
    const auto it = committed_.find(timestampNs);
    if (it != committed_.end()) it->second = true;
}
void MultiframeFrameRing::discardTimestamp(uint64_t timestampNs) noexcept {
    const auto it = byTimestamp_.find(timestampNs);
    const auto committed = committed_.find(timestampNs);
    if (it == byTimestamp_.end() || committed == committed_.end() || committed->second) return;
    ring_->discardUnsubmitted(it->second);
    metadata_.erase(it->second);
    color_.erase(it->second);
    committed_.erase(timestampNs);
    byTimestamp_.erase(it);
}
std::optional<std::uint64_t> MultiframeFrameRing::markReadyForTimestamp(std::uint64_t timestampNs) {
    if (!ring_) return std::nullopt;
    const auto it = byTimestamp_.find(timestampNs);
    if (it == byTimestamp_.end()) return std::nullopt;
    const auto committed = committed_.find(timestampNs);
    if (committed == committed_.end() || !committed->second) return std::nullopt;
    committed_.erase(committed);
    const std::uint64_t frameId = it->second;
    ring_->markReady(frameId);
    byTimestamp_.erase(it);
    return frameId;
}

const rawrcam::metadata::FrameMetadataSnapshot* MultiframeFrameRing::metadataFor(std::uint64_t frameId) const {
    const auto it = metadata_.find(frameId);
    return it == metadata_.end() ? nullptr : &it->second;
}

std::optional<FrozenBurst> MultiframeFrameRing::freezeFrame(std::uint64_t frameId) {
    if (!ring_) return std::nullopt;
    const auto meta = metadata_.find(frameId);
    const auto col = color_.find(frameId);
    if (meta == metadata_.end() || col == color_.end()) return std::nullopt;
    FrozenBurst out;
    // The frame was just marked ready, so it is among the newest ready ones.
    out.snapshot = ring_->snapshot(4u);
    if (!out.snapshot) return std::nullopt;
    for (const auto& ref : out.snapshot->refs()) {
        if (ref.frameId == frameId)
            out.refs.push_back(ref);
        else
            out.snapshot->release(ref.frameId);
    }
    const auto gpu = out.snapshot->gpuImage(frameId);
    if (out.refs.size() != 1u || !gpu) return std::nullopt;
    rawr::raw_gpu_pipeline::BurstFrame frame{};
    frame.raw = *gpu;
    const std::uint32_t cfa = meta->second.cameraContext ? meta->second.cameraContext->rawPreviewCfa : 0u;
    frame.parameters.normalization.blackByPhase =
        rawrcam::geometry::reorderRggbByCode(meta->second.blackLevelPhysicalRggb, cfa);
    frame.parameters.normalization.whiteLevel = meta->second.effectiveWhiteLevel;
    frame.parameters.whiteBalance = rawrcam::color::collapseRggbToRgb(col->second.baselineWbRggb);
    out.frames.push_back(frame);
    out.metadata.push_back(meta->second);
    out.colors.push_back(col->second);
    out.referenceMetadata = meta->second;
    out.referenceColor = col->second;
    return out;
}

std::optional<FrozenBurst> MultiframeFrameRing::freeze(std::size_t maxFrames) {
    if (!ring_) return std::nullopt;
    FrozenBurst out;
    out.snapshot = ring_->snapshot(std::clamp<std::size_t>(maxFrames, 2u, 30u));
    out.refs = out.snapshot ? out.snapshot->refs() : std::vector<rawr::zsl_ring::RawImageRef>{};
    const std::size_t requested = std::clamp<std::size_t>(maxFrames, 2u, 30u);
    while (out.refs.size() > requested) {
        out.snapshot->release(out.refs.front().frameId);
        out.refs.erase(out.refs.begin());
    }
    if (out.refs.size() < 2u) return std::nullopt;
    // Under sustained pressure use a fresh single frame, not an old pinned burst.
    if (latestInputTimestampNs_ > out.refs.back().timestampNs &&
        latestInputTimestampNs_ - out.refs.back().timestampNs > 250'000'000u)
        return std::nullopt;
    out.frames.reserve(out.refs.size());
    out.metadata.reserve(out.refs.size());
    out.colors.reserve(out.refs.size());
    for (const auto& ref : out.refs) {
        const auto meta = metadata_.find(ref.frameId);
        const auto col = color_.find(ref.frameId);
        const auto gpu = out.snapshot->gpuImage(ref.frameId);
        if (meta == metadata_.end() || col == color_.end() || !gpu) return std::nullopt;
        rawr::raw_gpu_pipeline::BurstFrame frame{};
        frame.raw = *gpu;
        // raw_normalize indexes black by physical 2x2 position; metadata keeps
        // it in R,G1,G2,B colour order.
        const std::uint32_t cfa = meta->second.cameraContext ? meta->second.cameraContext->rawPreviewCfa : 0u;
        frame.parameters.normalization.blackByPhase =
            rawrcam::geometry::reorderRggbByCode(meta->second.blackLevelPhysicalRggb, cfa);
        frame.parameters.normalization.whiteLevel = meta->second.effectiveWhiteLevel;
        frame.parameters.whiteBalance = rawrcam::color::collapseRggbToRgb(col->second.baselineWbRggb);
        out.frames.push_back(frame);
        out.metadata.push_back(meta->second);
        out.colors.push_back(col->second);
    }
    out.referenceIndex = static_cast<std::uint32_t>(out.frames.size() / 2u);
    const auto referenceId = out.refs[out.referenceIndex].frameId;
    out.referenceMetadata = out.metadata[out.referenceIndex];
    out.referenceColor = color_.at(referenceId);
    return out;
}

std::size_t MultiframeFrameRing::frameCount() const { return ring_ ? ring_->frameCount() : 0; }
std::uint64_t MultiframeFrameRing::usedBytes() const { return ring_ ? ring_->usedBytes() : 0; }

}  // namespace rawrcam::capture::multiframe
