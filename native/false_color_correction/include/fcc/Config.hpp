#pragma once
#include <cstdint>
namespace fcc {
struct PipelineConfig {
    uint32_t width=0;
    uint32_t height=0;
    // Maximum supported iterations for this instance. The implementation supports 1..8.
    // More than one step reuses a single RGBA16F ping-pong scratch image.
    uint32_t maxSteps=1;
    // Filter luminance-relative chroma to avoid importing bright-edge chroma into dark pixels.
    // False retains the legacy absolute-chroma mode for diagnostic comparisons.
    bool normalizedChroma=true;
    // Edge-aware 3x3 chroma average: bilateral sigma in Y units. 0 selects
    // the legacy uniform average (bit-identical when chromaBound is also 0).
    float edgeSigma=0.0f;
    // Luminance-preserving chroma bound: >0 rescales the averaged chroma so
    // fromYIQ(Y,iq) stays in [0,1]. 0 disables (legacy unbounded).
    float chromaBound=0.0f;
    // False skips the full-resolution scratch image used by
    // record() for more than one step: the caller then runs more
    // than one step with recordPingPong(), which alternates between its own two
    // images instead.
    bool ownScratch=true;
};
}
