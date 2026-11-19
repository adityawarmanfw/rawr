#pragma once

#include <cstdint>

// Rexture detail fit (rx_fit.comp, with the green sums rx_accumulate keeps): the push constants and the tuning shared by
// RextureRecorder and the tests.
namespace rawr::raw_gpu_pipeline::rexture_fit {
struct PushConstants {  // as rx_fit.comp declares it
    float black[4];
    float white;
    std::int32_t w, h, cfa, op;  // op 0: base green plane, 1: fitted green, 2: its detail added to the output,
                                 // 3: a companion's motion weight
    float omega, delta;
    float iwb[3];                // 1 / white balance per channel
    float alpha, beta;           // op 3: single-frame noise model
    float gain;                  // op 3: reference exposure / the companion's exposure
};
static_assert(sizeof(PushConstants) <= 128);
// One over-relaxed step: 2 recovers what three plain steps do (half-burst shared detail on a daylight street: brick, slats
// and foliage within 5% of three steps, the same noise).
constexpr float kOmega = 2.0f;
// Damping, in frames of weight: a pixel that few samples reach keeps the merge.
constexpr float kDelta = 1.0f;
}  // namespace rawr::raw_gpu_pipeline::rexture_fit
