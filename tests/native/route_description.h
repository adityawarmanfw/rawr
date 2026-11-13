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
      << " highResLive:" << r.highResLive << " useCase:" << r.rawStreamUseCase << '\n';
    levels("levels", r.levels);
    if (r.viewfinderLevels) levels("viewfinderLevels", *r.viewfinderLevels); else o << "viewfinderLevels=none\n";
    if (r.photoLevels) levels("photoLevels", *r.photoLevels); else o << "photoLevels=none\n";
    o << "exposure=extended:" << r.exposure.extended << " isoMax:" << r.exposure.isoMax
      << " shutterMaxNs:" << r.exposure.shutterMaxNs << '\n';
    opt("rawBufferUsage", r.rawBufferUsage);
    o << "rawBuffers=count:" << r.rawBufferCount << " testFirst:" << r.rawBufferTestFirst << " fallBack:" << r.rawBufferFallBack << '\n';
    o << "cameraApi=" << cameraApiName(r.cameraApi) << " opMode=" << r.opMode << '\n';
    o << "session=params:" << sessionParamsName(r.sessionParams) << " requestDefaults:" << requestDefaultsName(r.requestDefaults)
      << " frameRate:" << r.frameRate << " startupDelayMs:" << r.startupDelayMs << '\n';
    o << "template=" << (r.stillTemplate ? (*r.stillTemplate ? "still" : "preview") : "default") << '\n';
    o << "workarounds=";
    for (const auto& q : workaroundNames(r.workarounds)) o << q << ' ';
    o << "\ngate=" << r.vendorGate << " hold:"
      << (r.gateHold ? (*r.gateHold == GateHold::UntilClosed ? "closed" : "configured") : "default")
      << " stopViewfinderForPhotos:" << r.stopViewfinderForPhotos << '\n';
    o << "photoStream=";
    if (r.photoStream)
        o << int(r.photoStream->format) << ':' << r.photoStream->width << 'x' << r.photoStream->height;
    else
        o << "none";
    o << " useCase:" << r.photoStreamUseCase << " mergeFrameLimit:" << r.mergeFrameLimit << " mergeFramesOneByOne:" << r.mergeFramesOneByOne
      << '\n';
    opt("photoBufferUsage", r.photoBufferUsage);
    size("companionSize", r.companionSize);
    o << "companion=disabled:" << r.disableCompanion << " private:" << r.companionPrivate
      << " targeted:" << r.companionTargeted << " bufferCount:" << r.companionBufferCount
      << " fallback:" << int(r.companionFallback) << " useCase:" << r.companionStreamUseCase << '\n';
    opt("companionBufferUsage", r.companionBufferUsage);
    opt("companionPhysical", r.companionPhysical);
    opt("companionDetach", r.companionDetach);
    size("auxJpeg", r.auxJpegSize);
    opt("auxJpegBufferUsage", r.auxJpegBufferUsage);
    o << "auxJpegUseCase=" << r.auxJpegStreamUseCase << '\n';
    o << "sensorLayout=block:" << r.sensorLayoutBlock << " liveRemosaic:" << r.liveRemosaic << " photoRemosaic:" << r.photoRemosaic << '\n';
    if (!r.photoIncludePreview) o << "photoIncludePreview=0\n";
    if (r.photoBufferCount) o << "photoBufferCount=" << r.photoBufferCount << '\n';
    if (r.photoSensorLayoutBlock > 1) o << "photoSensorLayoutBlock=" << r.photoSensorLayoutBlock << '\n';
    opt("colourOrder", r.colourOrder);
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
        for (const auto& k : r.vendorKeys) {
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
