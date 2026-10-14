#pragma once
#include <array>
#include <optional>

#include "DngSource.h"

namespace rawrcam::renderer {

struct SceneKey {
    float keyEv = 0.0f;        // trimmed weighted median of log2 luma, before any render gain
    float highlightEv = 0.0f;  // weighted P95
};

// The camera's scene-key meter (color/AutoKeyExposure.h over image_scopes'
// SceneExposureStatsData) run on a DNG: white-balanced, lens-shaded camera RGB,
// luma = AP1 luma . camera->AP1, one sample per block at about the preview's
// sampling density, centre half weighted 3, samples at 0.9 x white in the top
// bin. For sources whose capture did not meter (Off, other cameras); a
// measured key stored with the DNG always takes precedence.
std::optional<SceneKey> measureSceneKey(DngSource& source, bool shading, const std::array<float, 3>& whiteBalance,
                                        const std::array<float, 9>& cameraToAp1ColumnMajor);

}  // namespace rawrcam::renderer
