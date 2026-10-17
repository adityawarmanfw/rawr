#include "FramePairer.h"

#include <utility>
namespace rawrcam::imaging {
void FramePairer::putImage(uint64_t ts, RawFrameLease v) { images_.insert_or_assign(ts, std::move(v)); }
void FramePairer::putMetadata(uint64_t ts, FrameRenderMetadata v) { metadata_[ts] = std::move(v); }
bool FramePairer::takeMatched(uint64_t ts, MatchedFrame* out) {
    if (!out) return false;
    auto i = images_.find(ts);
    auto m = metadata_.find(ts);
    if (i == images_.end() || m == metadata_.end()) return false;
    out->timestampNs = ts;
    out->image = std::move(i->second);
    out->metadata = m->second;
    images_.erase(i);
    metadata_.erase(m);
    return true;
}
uint64_t FramePairer::trim(size_t maxPendingImages, size_t maxPendingMetadata) {
    uint64_t dropped = 0;
    while (images_.size() > maxPendingImages) {
        images_.erase(images_.begin());
        ++dropped;
    }
    while (metadata_.size() > maxPendingMetadata) metadata_.erase(metadata_.begin());
    return dropped;
}
void FramePairer::clear() {
    images_.clear();
    metadata_.clear();
}
}  // namespace rawrcam::imaging
