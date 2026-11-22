// The live preview mirrors the whole frame after mapping its crop window (present.frag), so the
// window it gets is mirrored too (VideoCrop.h mirrorVideoSourceRect).
#include <cassert>
#include <iostream>

#include "video_pipeline/VideoCrop.h"

using rawrcam::video::mirrorVideoSourceRect;
using rawrcam::video::scaleVideoSourceRect;
using rawrcam::video::VideoSourceRect;
using rawrcam::video::videoKeptArea;
using rawrcam::video::videoSourceRect;

namespace {
bool same(const VideoSourceRect& a, const VideoSourceRect& b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height && a.reduceCfa == b.reduceCfa;
}
}  // namespace

int main() {
    // Xiaomi 15 front at 60 fps: the 3264x1836 mode fills rows 0..1835 of the 3264x2448 RAW;
    // the profile drops the 612 rows below (image.cropEdgesHighFps).
    const auto kept = videoKeptArea(3264, 2448, 0, 0, 0, 612);
    assert(kept.valid() && kept.y == 0 && kept.height == 1836);
    const auto window = videoSourceRect(3264, 2448, 2448, 1836, kept);
    assert(window.y == 0 && window.height == 1836);
    // Mirrored vertically, the window moves to the bottom of the frame: after the shader's mirror it
    // samples rows 0..1835 again, not the 612 rows the camera no longer writes.
    const auto mirrored = mirrorVideoSourceRect(window, 3264, 2448, false, true);
    assert(mirrored.y == 612 && mirrored.height == 1836 && mirrored.x == window.x && mirrored.width == window.width);
    // The same in preview pixels.
    const auto preview = scaleVideoSourceRect(window, 3264, 2448, 1632, 1224);
    const auto previewMirrored = mirrorVideoSourceRect(preview, 1632, 1224, false, true);
    assert(previewMirrored.y == 1224 - preview.y - preview.height);
    // Its own inverse; horizontal moves x only; no mirror changes nothing.
    assert(same(mirrorVideoSourceRect(mirrored, 3264, 2448, false, true), window));
    const VideoSourceRect left{100, 40, 1000, 800, true};
    const auto flipped = mirrorVideoSourceRect(left, 3264, 2448, true, false);
    assert(flipped.x == 3264 - 1100 && flipped.y == 40 && flipped.reduceCfa);
    assert(same(mirrorVideoSourceRect(left, 3264, 2448, false, false), left));
    // A window centred on the frame is its own mirror, so centred crops behave as before.
    const auto centred = videoSourceRect(4096, 3072, 3840, 2160);
    assert(same(mirrorVideoSourceRect(centred, 4096, 3072, true, true), centred));
    // Empty or out-of-frame windows come back unchanged.
    assert(same(mirrorVideoSourceRect(VideoSourceRect{}, 3264, 2448, true, true), VideoSourceRect{}));
    const VideoSourceRect outside{3000, 0, 500, 100, false};
    assert(same(mirrorVideoSourceRect(outside, 3264, 2448, true, true), outside));
    std::cout << "video_crop_mirror_test: ok\n";
    return 0;
}
