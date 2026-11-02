#pragma once

#include <string>
#include <vector>

namespace rawrcam::camera {

// Behaviours a camera provider needs avoided. Named, so a profile states why a lens is special
// and the controller never has to test a vendor or a profile id. A lens inherits the quirks of
// the device (BuiltInProfile::quirks) and adds its own (LensRoute::quirks).
struct CameraQuirks {
    // A tap's focus holds until the next tap. Returning to continuous AF after a tap scan, or
    // clearing the tap's AF region, crashes the provider; face-steered AF (which moves the
    // region) is off with it.
    bool afTapHold = false;
    // The HAL's shutter / ISO priority AE crashes the provider once it raises gain: only the
    // app's own AE may own S/I.
    bool noHalPriorityAe = false;
    // AF region changes crash the provider: tap AF and AF regions are unavailable.
    bool noTapAf = false;
    // Every request must carry an AE target fps range, even with AE off.
    bool requireFpsRange = false;
    // The companion the unverified-device rule would add is not added: the HAL cannot scale
    // the readout down to it.
    bool noAutoCompanion = false;
    // A request without a tap leaves the template's own AF and AE regions alone instead of writing an
    // empty one. The vivo X200 Ultra's AF node takes an emptied region as a null ROI and kills the
    // provider (SIGSEGV in convertAPPROIToCoreROI) on its first full-size request.
    bool keepRegions = false;

    [[nodiscard]] bool any() const noexcept {
        return afTapHold || noHalPriorityAe || noTapAf || requireFpsRange || noAutoCompanion || keepRegions;
    }
    bool operator==(const CameraQuirks& o) const noexcept {
        return afTapHold == o.afTapHold && noHalPriorityAe == o.noHalPriorityAe && noTapAf == o.noTapAf &&
               requireFpsRange == o.requireFpsRange && noAutoCompanion == o.noAutoCompanion &&
               keepRegions == o.keepRegions;
    }
};

[[nodiscard]] inline CameraQuirks mergeQuirks(const CameraQuirks& a, const CameraQuirks& b) noexcept {
    return {a.afTapHold || b.afTapHold, a.noHalPriorityAe || b.noHalPriorityAe, a.noTapAf || b.noTapAf,
            a.requireFpsRange || b.requireFpsRange, a.noAutoCompanion || b.noAutoCompanion,
            a.keepRegions || b.keepRegions};
}

// Whether a request writes its AF / AE region entry: a tap's region always; otherwise it clears the entry
// (an empty region) unless the lens keeps the template's own (keepRegions).
[[nodiscard]] inline bool writesRegionEntry(bool hasRegion, bool keepRegions) noexcept {
    return hasRegion || !keepRegions;
}

// False for an unknown name.
[[nodiscard]] inline bool setQuirk(CameraQuirks& quirks, const std::string& name) {
    if (name == "afTapHold") quirks.afTapHold = true;
    else if (name == "noHalPriorityAe") quirks.noHalPriorityAe = true;
    else if (name == "noTapAf") quirks.noTapAf = true;
    else if (name == "requireFpsRange") quirks.requireFpsRange = true;
    else if (name == "noAutoCompanion") quirks.noAutoCompanion = true;
    else if (name == "keepRegions") quirks.keepRegions = true;
    else return false;
    return true;
}

[[nodiscard]] inline std::vector<std::string> quirkNames(const CameraQuirks& quirks) {
    std::vector<std::string> names;
    if (quirks.afTapHold) names.push_back("afTapHold");
    if (quirks.noHalPriorityAe) names.push_back("noHalPriorityAe");
    if (quirks.noTapAf) names.push_back("noTapAf");
    if (quirks.requireFpsRange) names.push_back("requireFpsRange");
    if (quirks.noAutoCompanion) names.push_back("noAutoCompanion");
    if (quirks.keepRegions) names.push_back("keepRegions");
    return names;
}

}  // namespace rawrcam::camera
