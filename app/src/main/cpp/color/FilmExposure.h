#pragma once

#include <algorithm>
#include <cmath>

#include "spektrafilm/SpektraFilm.h"

namespace rawrcam::color {

// Shared by live preview and JPEG rendering. Camera EV is already in the
// sensor exposure; a second scene meter would cancel that creative choice.
// Cap only the additional gain, preserving the user's film exposure offset.
// renderExposureEv is the Linear-stage exposure gain, a separate input from
// the stock's Film EV; it reaches the film shader as part of the same scalar.
inline float filmExposureEv(const spektrafilm_native::FilmLook& look, float aePostGain,
                            float renderExposureEv = 0.0f) noexcept {
    const float lift = std::log2(std::max(aePostGain, 1.0e-6f));
    return std::clamp(look.filmExposureEv + std::min(lift, spektrafilm_native::FilmLook::kMaxFilmLiftEv) +
                          renderExposureEv,
                      -10.0f, 10.0f);
}

}  // namespace rawrcam::color
