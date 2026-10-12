#pragma once
#include <spektrafilm/SpektraFilm.h>
#include <tonemap/TonemapMath.h>

#include <iomanip>
#include <locale>
#include <sstream>

namespace rawrcam::capture::persistence {
// Named scalar fields are portable across native ABI and struct-layout changes.
// The adaptive curve is serialized independently of the baseline HAL gain.
inline std::string resolvedRecipe(const tonemap::TonemapParams& tone, float gain, bool filmEnabled,
                                  const spektrafilm_native::FilmLook& film) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::boolalpha << std::setprecision(9) << "{\"version\":1,\"aePostGain\":" << gain
        << ",\"autoKeyEv\":" << tone.adaptiveTone.liftEv << ",\"filmEnabled\":" << (filmEnabled ? "true" : "false")
        << ",\"tonemap\":{\"version\":1";
#define RAWR_TONE_FIELD(name) out << ",\"" #name "\":" << tone.name;
    RAWR_TONE_FIELD(exposureEV)
    RAWR_TONE_FIELD(blackPointEV)
    RAWR_TONE_FIELD(shadowLiftEV)
    RAWR_TONE_FIELD(midtoneLiftEV)
    RAWR_TONE_FIELD(contrast)
    RAWR_TONE_FIELD(shoulderStartEV)
    RAWR_TONE_FIELD(whitePointEV)
    RAWR_TONE_FIELD(highlightBiasEV)
    RAWR_TONE_FIELD(saturation)
    RAWR_TONE_FIELD(vibrance)
    out << ",\"adaptiveTone\":{\"version\":1,\"liftEv\":" << tone.adaptiveTone.liftEv
        << ",\"keyEv\":" << tone.adaptiveTone.keyEv << ",\"highlightEv\":" << tone.adaptiveTone.highlightEv << "}";
#undef RAWR_TONE_FIELD
    out << "},\"film\":{\"version\":1";
#define RAWR_FILM_FIELD(type, name) out << ",\"" #name "\":" << film.name;
#include "tonemap/FilmLookFields.inc"
#undef RAWR_FILM_FIELD
    out << "}}";
    return out.str();
}
}  // namespace rawrcam::capture::persistence
