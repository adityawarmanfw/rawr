#pragma once

#include <string>
#include <vector>

namespace rawrcam::camera {

// Behaviours a camera provider needs avoided. Named, so a profile states why a lens is special
// and the controller never has to test a vendor or a profile id. They live on lenses only
// (LensRoute::workarounds), plus what a lens's shape implies (effectiveWorkarounds).
struct CameraWorkarounds {
    // A tap's focus holds until the next tap. Returning to continuous AF after a tap scan, or
    // clearing the tap's AF region, crashes the provider; face-steered AF (which moves the
    // region) is off with it.
    bool holdTapFocus = false;
    // The HAL's shutter / ISO priority AE crashes the provider once it raises gain: only the
    // app's own AE may own S/I.
    bool appPriorityAe = false;
    // AF region changes crash the provider: tap AF and AF regions are unavailable.
    bool noTapFocus = false;
    // Every request must carry an AE target fps range, even with AE off.
    bool alwaysSendFrameRate = false;
    // The companion the unverified-device rule would add is not added: the HAL cannot scale
    // the readout down to it.
    bool noAutoCompanion = false;
    // A request without a tap leaves the template's own AF and AE regions alone instead of writing an
    // empty one. The vivo X200 Ultra's AF node takes an emptied region as a null ROI and kills the
    // provider (SIGSEGV in convertAPPROIToCoreROI) on its first full-size request.
    bool keepTemplateRegions = false;

    [[nodiscard]] bool any() const noexcept {
        return holdTapFocus || appPriorityAe || noTapFocus || alwaysSendFrameRate || noAutoCompanion ||
               keepTemplateRegions;
    }
    bool operator==(const CameraWorkarounds& o) const noexcept {
        return holdTapFocus == o.holdTapFocus && appPriorityAe == o.appPriorityAe && noTapFocus == o.noTapFocus &&
               alwaysSendFrameRate == o.alwaysSendFrameRate && noAutoCompanion == o.noAutoCompanion &&
               keepTemplateRegions == o.keepTemplateRegions;
    }
};

[[nodiscard]] inline CameraWorkarounds mergeWorkarounds(const CameraWorkarounds& a, const CameraWorkarounds& b) noexcept {
    return {a.holdTapFocus || b.holdTapFocus, a.appPriorityAe || b.appPriorityAe, a.noTapFocus || b.noTapFocus,
            a.alwaysSendFrameRate || b.alwaysSendFrameRate, a.noAutoCompanion || b.noAutoCompanion,
            a.keepTemplateRegions || b.keepTemplateRegions};
}

// Whether a request writes its AF / AE region entry: a tap's region always; otherwise it clears the entry
// (an empty region) unless the lens keeps the template's own (keepTemplateRegions).
[[nodiscard]] inline bool writesRegionEntry(bool hasRegion, bool keepTemplateRegions) noexcept {
    return hasRegion || !keepTemplateRegions;
}

// False for an unknown name.
[[nodiscard]] inline bool setWorkaround(CameraWorkarounds& workarounds, const std::string& name) {
    if (name == "holdTapFocus") workarounds.holdTapFocus = true;
    else if (name == "appPriorityAe") workarounds.appPriorityAe = true;
    else if (name == "noTapFocus") workarounds.noTapFocus = true;
    else if (name == "alwaysSendFrameRate") workarounds.alwaysSendFrameRate = true;
    else if (name == "noAutoCompanion") workarounds.noAutoCompanion = true;
    else if (name == "keepTemplateRegions") workarounds.keepTemplateRegions = true;
    else return false;
    return true;
}

// The schema-1 name of a workaround (when profiles called them quirks); empty when it never had another name.
[[nodiscard]] inline std::string workaroundFromSchema1(const std::string& name) {
    if (name == "afTapHold") return "holdTapFocus";
    if (name == "noHalPriorityAe") return "appPriorityAe";
    if (name == "noTapAf") return "noTapFocus";
    if (name == "requireFpsRange") return "alwaysSendFrameRate";
    if (name == "keepRegions") return "keepTemplateRegions";
    return name;
}

[[nodiscard]] inline std::vector<std::string> workaroundNames(const CameraWorkarounds& workarounds) {
    std::vector<std::string> names;
    if (workarounds.holdTapFocus) names.push_back("holdTapFocus");
    if (workarounds.appPriorityAe) names.push_back("appPriorityAe");
    if (workarounds.noTapFocus) names.push_back("noTapFocus");
    if (workarounds.alwaysSendFrameRate) names.push_back("alwaysSendFrameRate");
    if (workarounds.noAutoCompanion) names.push_back("noAutoCompanion");
    if (workarounds.keepTemplateRegions) names.push_back("keepTemplateRegions");
    return names;
}

}  // namespace rawrcam::camera
