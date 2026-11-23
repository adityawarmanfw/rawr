#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace rawrcam::imaging {

// Application-wide realtime buffering contract. Every per-frame-slot subsystem
// must size and validate its resources from this single owner so preview,
// statistics, monitoring, diagnostics, and presentation cannot drift apart.
inline constexpr std::uint32_t kRealtimeFramesInFlight = 3;

inline constexpr std::size_t kRawReaderMaxImages = 6;
inline constexpr std::size_t kMaxIngressImages = 1;
inline constexpr std::size_t kMaxIngressMetadata = 8;
inline constexpr std::size_t kMaxPairerImages = 1;
inline constexpr std::size_t kMaxPairerMetadata = 3;
static_assert(kRealtimeFramesInFlight + kMaxIngressImages + kMaxPairerImages < kRawReaderMaxImages,
              "RAW transport must retain one reader lease of headroom");

// A one-off 45-80 ms stall about 0.2 s into a take let the single waiting RAW frame be evicted
// (INGRESS_QUEUE_PRESSURE rawDropped=1; traced on an X14U), and the vivo X300 FE shows runs of 3 and 7 evictions.
// A deeper realtime reader and ingress queue ride out a stall of a few frames. They cost two more reader images,
// so they apply only where an image is small: a 50 MP live lens keeps the usual 6. A lens profile's own buffer
// count always wins.
inline constexpr std::size_t kDeepRealtimeRawReaderMaxImages = 8;
inline constexpr std::size_t kDeepMaxIngressImages = 3;
inline constexpr std::uint64_t kDeepReaderMaxImageBytes = 40ull << 20;  // ~20 MP of RAW16
static_assert(kRealtimeFramesInFlight + kDeepMaxIngressImages + kMaxPairerImages < kDeepRealtimeRawReaderMaxImages,
              "the deep reader must keep one lease of headroom too");

// Images the realtime RAW reader holds. imageBytes is an upper bound for one image (width x height x 2).
[[nodiscard]] constexpr std::size_t realtimeRawReaderMaxImages(std::size_t profileMaxImages,
                                                               std::uint64_t imageBytes) noexcept {
    if (profileMaxImages) return profileMaxImages;
    return imageBytes <= kDeepReaderMaxImageBytes ? kDeepRealtimeRawReaderMaxImages : kRawReaderMaxImages;
}

// RAW images the ingress queue may hold: what a reader of readerMaxImages can spare beyond the frames in flight,
// the pairer and one lease of headroom (1 for the usual 6, 3 for 8).
[[nodiscard]] constexpr std::size_t ingressImageCapacity(std::size_t readerMaxImages) noexcept {
    constexpr std::size_t reserved = kRealtimeFramesInFlight + kMaxPairerImages + 1;
    const std::size_t spare = readerMaxImages > reserved ? readerMaxImages - reserved : 0;
    return std::clamp(spare, kMaxIngressImages, kDeepMaxIngressImages);
}

}  // namespace rawrcam::imaging
