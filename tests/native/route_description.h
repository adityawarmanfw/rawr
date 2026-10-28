#pragma once
// Every field of a parsed lens route as text, for golden comparisons across profile formats.
#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

#include "camera/CameraRouting.h"

namespace rawrcam::camera::testing {

inline std::string describeRouteForGolden(const LensRoute& r) {
    std::ostringstream o;
    o.precision(17);
    const auto opt = [&](const char* name, const auto& v) {
        o << name << '=';
        if (v) o << *v; else o << "none";
        o << '\n';
    };
    const auto levels = [&](const char* name, const LevelOverride& l) {
        o << name << "=static:" << l.isStatic << " black:" << l.blackRggb[0] << ',' << l.blackRggb[1] << ','
          << l.blackRggb[2] << ',' << l.blackRggb[3] << " white:" << l.white << '\n';
    };
    const auto size = [&](const char* name, const std::optional<std::pair<uint32_t, uint32_t>>& s) {
        o << name << '=';
        if (s) o << s->first << 'x' << s->second; else o << "none";
        o << '\n';
    };
    o << "lens=" << r.lensId << "\ncamera=" << r.cameraId << "\nphysical=" << r.physicalCameraId << '\n';
    o << "stream=" << int(r.preferredStream.format) << ':' << r.preferredStream.width << 'x' << r.preferredStream.height
      << " unlisted:" << r.unlistedStream << " useCase:" << r.rawStreamUseCase << '\n';
    levels("levels", r.levels);
    if (r.viewfinderLevels) levels("viewfinderLevels", *r.viewfinderLevels); else o << "viewfinderLevels=none\n";
    if (r.captureLevels) levels("captureLevels", *r.captureLevels); else o << "captureLevels=none\n";
    o << "exposure=extended:" << r.exposure.extended << " isoMax:" << r.exposure.isoMax
      << " shutterMaxNs:" << r.exposure.shutterMaxNs << '\n';
    opt("readerUsage", r.readerUsage);
    o << "reader=maxImages:" << r.rawMaxImages << " probe:" << r.readerProbe << " demote:" << r.readerDemote << '\n';
    o << "backend=" << backendChoiceName(r.backend) << " opMode=" << r.opMode << '\n';
    o << "session=params:" << sessionParamsName(r.sessionParams) << " requestDefaults:" << requestDefaultsName(r.requestDefaults)
      << " fps:" << r.fixedFps << " startupDelayMs:" << r.startupDelayMs << '\n';
    o << "template=" << (r.stillTemplate ? (*r.stillTemplate ? "still" : "preview") : "default") << '\n';
    o << "quirks=";
    for (const auto& q : quirkNames(r.quirks)) o << q << ' ';
    o << "\ngate=" << r.vendorGate << " hold:"
      << (r.gateHold ? (*r.gateHold == GateHold::UntilClosed ? "closed" : "configured") : "default")
      << " stopRepeatingForStill:" << r.stopRepeatingForStill << '\n';
    o << "captureStream=";
    if (r.captureStream)
        o << int(r.captureStream->format) << ':' << r.captureStream->width << 'x' << r.captureStream->height;
    else
        o << "none";
    o << " useCase:" << r.captureStreamUseCase << " burst:" << r.captureBurst << " sequential:" << r.captureBurstSequential
      << '\n';
    opt("captureReaderUsage", r.captureReaderUsage);
    size("companionSize", r.companionSize);
    o << "companion=disabled:" << r.disableCompanion << " private:" << r.companionPrivate
      << " targeted:" << r.companionTargeted << " maxImages:" << r.companionMaxImages
      << " fallback:" << int(r.companionFallback) << " useCase:" << r.companionStreamUseCase << '\n';
    opt("companionUsage", r.companionUsage);
    opt("companionPhysical", r.companionPhysical);
    opt("companionDetach", r.companionDetach);
    size("auxJpeg", r.auxJpegSize);
    opt("auxJpegUsage", r.auxJpegUsage);
    o << "auxJpegUseCase=" << r.auxJpegStreamUseCase << '\n';
    o << "cfa=block:" << r.cfaBlock << " live:" << r.cfaLiveQuality << " photo:" << r.cfaPhotoQuality << '\n';
    opt("cfaOrder", r.cfaOrder);
    o << "distortion=";
    if (r.distortion) {
        const auto& d = *r.distortion;
        o << d.width << 'x' << d.height << " f:" << d.fx << ',' << d.fy << " c:" << d.cx << ',' << d.cy << " k:";
        for (double k : d.k) o << k << ',';
        o << " scale:" << d.outputScale;
    } else {
        o << "none";
    }
    o << "\nmirror=" << r.mirror << "\ndcp=" << r.dcpPath << '\n';
    // Keys by the phase they go out in. Within a phase they are sorted: a request is a map from tag to value, so
    // the order of distinct tags does not reach the camera.
    using Scope = CameraKeySetting::Scope;
    for (const auto& [phase, name] : {std::pair{Scope::Session, "session"}, std::pair{Scope::Request, "request"},
                                      std::pair{Scope::Still, "still"}, std::pair{Scope::Decision, "decision"}}) {
        std::vector<std::string> lines;
        for (const auto& k : r.keys) {
            if (!keyAppliesIn(k.scope, phase)) continue;
            std::ostringstream line;
            line.precision(17);
            line << "key." << name << '=' << k.tag << " type:" << int(k.type) << " enabled:" << k.enabled << " values:";
            for (double v : k.values) line << v << ',';
            lines.push_back(line.str());
        }
        std::sort(lines.begin(), lines.end());
        for (const auto& line : lines) o << line << '\n';
    }
    return o.str();
}

}  // namespace rawrcam::camera::testing
