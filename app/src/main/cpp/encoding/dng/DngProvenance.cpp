#include "encoding/dng/DngProvenance.h"

#include <optional>
#include <string>
#include <utility>

namespace rawrcam::encoding::dng {
namespace {

// --- DNGPrivateData provenance (byte-identical schema to the legacy writer;
// the renderer keys on the identifier, so it is intentionally unchanged). ---
void appendJsonString(std::string& out, const std::string& s) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out.push_back(static_cast<char>(c));
                break;
        }
    }
    out.push_back('"');
}

struct Provenance {
    std::optional<std::string> applicationName, processingPipelineVersion;
    std::optional<std::string> logicalCameraId, physicalCameraId, sensorMode;
    std::vector<std::string> captureFlags;
    std::vector<std::pair<std::string, std::string>> extensions;
};

std::vector<uint8_t> serializeProvenance(const Provenance& app) {
    const std::string identifier = "RawrCam tinydng provenance";
    std::string json = "{\"schema\":\"tinydng.provenance\",\"version\":1";
    const auto field = [&](const char* key, const std::optional<std::string>& v) {
        if (!v) return;
        json.push_back(',');
        appendJsonString(json, key);
        json.push_back(':');
        appendJsonString(json, *v);
    };
    field("applicationName", app.applicationName);
    field("processingPipelineVersion", app.processingPipelineVersion);
    field("logicalCameraId", app.logicalCameraId);
    field("physicalCameraId", app.physicalCameraId);
    field("sensorMode", app.sensorMode);
    if (!app.captureFlags.empty()) {
        json.push_back(',');
        json += "\"captureFlags\":[";
        for (size_t i = 0; i < app.captureFlags.size(); ++i) {
            if (i) json.push_back(',');
            appendJsonString(json, app.captureFlags[i]);
        }
        json.push_back(']');
    }
    if (!app.extensions.empty()) {
        json.push_back(',');
        json += "\"extensions\":{";
        for (size_t i = 0; i < app.extensions.size(); ++i) {
            if (i) json.push_back(',');
            appendJsonString(json, app.extensions[i].first);
            json.push_back(':');
            appendJsonString(json, app.extensions[i].second);
        }
        json.push_back('}');
    }
    json.push_back('}');
    std::vector<uint8_t> out(identifier.begin(), identifier.end());
    out.push_back(0);
    out.insert(out.end(), json.begin(), json.end());
    return out;
}

}  // namespace

std::vector<uint8_t> buildDngProvenance(const rawrcam::imaging::RawSnapshot& frame,
                                        const DngCaptureContext& captureContext, const char* geometryMode) {
    const auto& ctx = *frame.metadata.cameraContext;
    Provenance prov{};
    prov.applicationName = "RawrCam";
    prov.processingPipelineVersion = "rawr-still-1";
    prov.logicalCameraId = ctx.cameraId;
    prov.physicalCameraId = ctx.cameraId;
    prov.sensorMode = geometryMode;
    prov.captureFlags = captureContext.reconstructedGeometry
                            ? std::vector<std::string>{"gpu_multiframe_reconstruction", "paired_sensor_timestamp"}
                            : std::vector<std::string>{"cpu_snapshot", "paired_sensor_timestamp"};

    // Replay recipes, rendered by the capture layer and embedded verbatim.
    if (!captureContext.processingRecipe.empty())
        prov.extensions.emplace_back("com.rawrcam.processing.recipe.v1", captureContext.processingRecipe);
    if (!captureContext.resolvedRecipe.empty())
        prov.extensions.emplace_back("com.rawrcam.processing.resolved.v1", captureContext.resolvedRecipe);
    prov.extensions.emplace_back("com.rawrcam.processing.source_role", captureContext.sourceRole);
    prov.extensions.emplace_back("com.rawrcam.processing.applied",
                                 captureContext.sourceRole == "merged" ? "multiframe_merge" : "sensor_raw");
    if (!captureContext.frameRecipe.empty())
        prov.extensions.emplace_back("com.rawrcam.processing.frame.v1", captureContext.frameRecipe);

    // Sensor and capture facts.
    prov.extensions.emplace_back("com.rawrcam.sensor.timestamp_ns", std::to_string(frame.timestampNs));
    prov.extensions.emplace_back("com.rawrcam.source.row_stride_bytes", std::to_string(frame.sourceRowStrideBytes));
    prov.extensions.emplace_back("com.rawrcam.geometry.mapping", geometryMode);
    prov.extensions.emplace_back("com.rawrcam.white.effective_source", rawrcam::metadata::effectiveWhiteLevelSourceName(
                                                                           frame.metadata.effectiveWhiteLevelSource));
    prov.extensions.emplace_back("com.rawrcam.white.effective", std::to_string(frame.metadata.effectiveWhiteLevel));
    if (frame.metadata.reportedDynamicWhiteLevel)
        prov.extensions.emplace_back("com.rawrcam.white.reported_dynamic",
                                     std::to_string(*frame.metadata.reportedDynamicWhiteLevel));
    if (ctx.maxAnalogSensitivity > 0)
        prov.extensions.emplace_back("com.rawrcam.sensor.max_analog_sensitivity",
                                     std::to_string(ctx.maxAnalogSensitivity));
    if (frame.metadata.requestedSensitivity)
        prov.extensions.emplace_back("com.rawrcam.sensitivity.requested",
                                     std::to_string(*frame.metadata.requestedSensitivity));
    prov.extensions.emplace_back("com.rawrcam.sensitivity.reported", std::to_string(frame.metadata.sensitivity));
    if (ctx.hasLensShadingApplied)
        prov.extensions.emplace_back("com.rawrcam.shading.applied", ctx.lensShadingApplied ? "true" : "false");
    if (!frame.metadata.lensShadingMap.empty() && ctx.hasLensShadingApplied && ctx.lensShadingApplied)
        prov.extensions.emplace_back("com.rawrcam.shading.gainmap_skipped", "already_applied");
    if (ctx.hasGreenSplit)
        prov.extensions.emplace_back("com.rawrcam.sensor.green_split", std::to_string(ctx.greenSplit));
    if (!ctx.opticalBlackRegions.empty())
        prov.extensions.emplace_back("com.rawrcam.optical_black.regions",
                                     std::to_string(ctx.opticalBlackRegions.size()));
    if (frame.metadata.flashState >= 0)
        prov.extensions.emplace_back("com.rawrcam.flash.state", std::to_string(frame.metadata.flashState));
    if (ctx.hasFlashInfoAvailable)
        prov.extensions.emplace_back("com.rawrcam.flash.available", ctx.flashInfoAvailable ? "true" : "false");
    if (!ctx.lensDistortion.empty()) prov.extensions.emplace_back("com.rawrcam.lens.distortion_present", "true");
    if (!ctx.lensIntrinsicCalibration.empty())
        prov.extensions.emplace_back("com.rawrcam.lens.intrinsic_present", "true");
    if (!frame.metadata.hotPixelMap.empty())
        prov.extensions.emplace_back("com.rawrcam.hot_pixel.count",
                                     std::to_string(frame.metadata.hotPixelMap.size() / 2));
    return serializeProvenance(prov);
}

}  // namespace rawrcam::encoding::dng
