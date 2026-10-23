#include <cassert>
#include <deque>
#include <thread>
#include <vector>
#include "imaging/RawFrameIngress.h"
#include "imaging/RawFrameLease.h"

AImageReader* current = nullptr;
int AImageReader_newWithUsage(int32_t, int32_t, int32_t, uint64_t, int32_t limit, AImageReader** out) {
    current = new AImageReader;
    current->limit = limit;
    *out = current;
    return 0;
}
int AImageReader_setImageListener(AImageReader* r, AImageReader_ImageListener* l) { r->listener = *l; return 0; }
void AImageReader_delete(AImageReader* r) { assert(r->held == 0); delete r; current = nullptr; }
int AImageReader_getWindow(AImageReader* r, ANativeWindow** out) { *out = &r->window; return 0; }
int AImageReader_acquireNextImageAsync(AImageReader* r, AImage** out, int* fence) {
    if (r->held == r->limit) return AMEDIA_IMGREADER_MAX_IMAGES_ACQUIRED;
    if (r->pending.empty()) return -2;
    *out = new AImage{r, r->pending.front(), {}};
    r->pending.pop_front();
    ++r->held;
    *fence = -1;
    return 0;
}
void AImage_delete(AImage* image) { --image->reader->held; delete image; } // deliberately no notification

int main() {
    using namespace rawrcam::imaging;
    std::deque<RawFrameLease> held;
    std::vector<uint64_t> timestamps;
    RawFrameIngress ingress([&](AcquiredRawFrame f) {
        timestamps.push_back(f.timestampNs);
        held.emplace_back(f);
    }, {}, {});
    for (int count : {8, 16, 30}) {
        timestamps.clear();
        ingress.create(count, 16, 12, rawrcam::geometry::RawPixelFormat::Raw16, 0);
        for (int i = 1; i <= count; ++i) current->pending.push_back(i);
        const auto listener = current->listener;
        listener.onImageAvailable(listener.context, current);
        assert(held.size() == 6);
        listener.onImageAvailable(listener.context, current); // slot limit, no more notifications
        assert(timestamps.size() == 6 && current->pending.size() == static_cast<size_t>(count - 6));
        for (int i = 1; i <= count; ++i) {
            assert(!held.empty() && held.front().get().timestampNs == static_cast<uint64_t>(i));
            held.pop_front();
            ingress.resumeAfterRelease(count);
            assert(current->held <= 6);
        }
        assert(timestamps.size() == static_cast<size_t>(count));
        assert(held.empty() && current->pending.empty() && current->held == 0);
        ingress.destroy();
        ingress.resumeAfterRelease(count); // closed reader: no-op
    }
    ingress.create(100, 16, 12, rawrcam::geometry::RawPixelFormat::Raw16, 0);
    current->pending.push_back(99);
    ingress.resumeAfterRelease(30); // stale worker cannot drain a replacement reader
    assert(current->pending.size() == 1);
    // Callback and release retry both drain the same reader under serialization.
    auto* reader = current;
    const auto listener = reader->listener;
    std::thread callback([&] { listener.onImageAvailable(listener.context, reader); });
    std::thread retry([&] { ingress.resumeAfterRelease(100); });
    callback.join(); retry.join();
    assert(held.size() == 1 && current->pending.empty());
    held.clear();
    ingress.destroy();
}
