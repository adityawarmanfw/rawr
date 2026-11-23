// The realtime RAW reader depth and the ingress queue depth that follows it.
#include <cassert>

#include "imaging/FrameLimits.h"

using namespace rawrcam::imaging;

int main() {
    constexpr std::uint64_t k12Mp = 4096ull * 3072 * 2;    // a binned viewfinder: 24 MiB per image
    constexpr std::uint64_t k50Mp = 8192ull * 6144 * 2;    // a 50 MP live lens: 96 MiB
    constexpr std::uint64_t k200Mp = 16320ull * 12288 * 2;  // the 200 MP frame

    // Small images get the deep reader, big ones keep today's 6; a profile's count always wins.
    static_assert(realtimeRawReaderMaxImages(0, k12Mp) == kDeepRealtimeRawReaderMaxImages);
    static_assert(realtimeRawReaderMaxImages(0, k50Mp) == kRawReaderMaxImages);
    static_assert(realtimeRawReaderMaxImages(0, k200Mp) == kRawReaderMaxImages);
    static_assert(realtimeRawReaderMaxImages(0, kDeepReaderMaxImageBytes) == kDeepRealtimeRawReaderMaxImages);
    static_assert(realtimeRawReaderMaxImages(0, kDeepReaderMaxImageBytes + 1) == kRawReaderMaxImages);
    static_assert(realtimeRawReaderMaxImages(4, k12Mp) == 4);
    static_assert(realtimeRawReaderMaxImages(12, k200Mp) == 12);

    // The queue takes what the reader can spare: one waiting frame for 6 (unchanged), three for 8.
    static_assert(ingressImageCapacity(kRawReaderMaxImages) == kMaxIngressImages);
    static_assert(ingressImageCapacity(kDeepRealtimeRawReaderMaxImages) == kDeepMaxIngressImages);
    static_assert(ingressImageCapacity(7) == 2);
    static_assert(ingressImageCapacity(16) == kDeepMaxIngressImages);  // never deeper than the deep queue
    static_assert(ingressImageCapacity(2) == kMaxIngressImages);       // a probing profile still gets one
    static_assert(ingressImageCapacity(0) == kMaxIngressImages);

    // Whatever the reader, the queue leaves the frames in flight, the pairer and one lease of headroom (down to
    // the profile readers of 6 and more; a smaller one is for probing and has always been tight).
    for (std::size_t reader = kRawReaderMaxImages; reader <= 16; ++reader)
        assert(kRealtimeFramesInFlight + ingressImageCapacity(reader) + kMaxPairerImages < reader);
    return 0;
}
