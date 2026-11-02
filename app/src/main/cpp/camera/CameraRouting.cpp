#include "camera/CameraRouting.h"

#include "camera/CameraProfileJson.h"

namespace rawrcam::camera {

namespace {

// Profiles that fail to parse are dropped here; camera_profile_test parses
// every file strictly, so a broken one fails the host tests instead.
std::vector<BuiltInProfile> loadBuiltInProfiles() {
    static const char* const kSources[] = {
#include "camera/BuiltInCameraProfiles.inc"
        nullptr,  // Keeps the array non-empty when there are no profile files.
    };
    std::vector<BuiltInProfile> out;
    for (const char* source : kSources) {
        if (!source) continue;
        if (auto parsed = parseBuiltInProfile(source)) out.push_back(std::move(*parsed));
    }
    return out;
}

CameraProfile makeGeneric() {
    LensRoute r;
    r.lensId = "1x";
    r.cameraId = "0";
    return {"generic", {r}};
}

bool contains(const std::vector<std::string>& list, const std::string& value) {
    for (const auto& v : list)
        if (v == value) return true;
    return false;
}

}  // namespace

const std::vector<BuiltInProfile>& builtInProfiles() {
    static const std::vector<BuiltInProfile> profiles = loadBuiltInProfiles();
    return profiles;
}

const CameraProfile& builtInCameraProfile(const std::string& profileId) {
    static const CameraProfile generic = makeGeneric();
    for (const auto& p : builtInProfiles())
        if (p.profile.id == profileId) return p.profile;
    return generic;
}

bool isBuiltInProfileId(const std::string& profileId) {
    return profileId == "generic" || builtInCameraProfile(profileId).id == profileId;
}

ProfileMatch matchBuiltInProfile(const PropertyReader& property) {
    const std::string model = property("ro.product.model");
    for (const auto& p : builtInProfiles())
        if (!model.empty() && contains(p.match.models, model))
            return {p.profile.id, "model", contains(p.match.verifiedModels, model)};
    for (const auto& p : builtInProfiles())
        for (const auto& [name, prefix] : p.match.propertyPrefixes)
            if (property(name).rfind(prefix, 0) == 0)
                return {p.profile.id, "property:" + name, contains(p.match.verifiedModels, model)};
    return {};
}

std::optional<LensRoute> routeForLens(const CameraProfile& profile, const std::string& lensId) {
    for (const auto& r : profile.lenses)
        if (r.lensId == lensId) return r;
    return std::nullopt;
}

LensRoute routeForCameraId(const CameraProfile& profile, const std::string& cameraId) {
    for (const auto& r : profile.lenses)
        if (r.cameraId == cameraId && r.physicalCameraId.empty()) return r;
    LensRoute adHoc;
    adHoc.lensId = "1x";
    adHoc.cameraId = cameraId;
    return adHoc;
}

}  // namespace rawrcam::camera
