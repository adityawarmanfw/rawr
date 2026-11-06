#pragma once

#include "encoding/LensMetadata.h"
#include "encoding/jpeg/JpegCaptureContext.h"
#include "metadata/MetadataTypes.h"

namespace rawrcam::encoding::jpeg {

inline std::optional<std::array<std::uint32_t, 2>> captureDigitalZoomRatio(
    const metadata::RectI& activeArray, const metadata::RectI& cropRegion) {
    if (!metadata::validRect(activeArray) || !metadata::validRect(cropRegion)) return std::nullopt;
    const auto fullWidth = metadata::rectWidth(activeArray);
    const auto cropWidth = metadata::rectWidth(cropRegion);
    if (fullWidth < cropWidth) return std::nullopt;
    return std::array<std::uint32_t, 2>{fullWidth, cropWidth};
}

struct JpegLensMetadataView {
    std::string_view make;
    std::string_view model;
    std::optional<std::uint16_t> focalLength35mm;
};

inline JpegLensMetadataView lensMetadataForJpeg(const JpegCaptureContext& context, LensMetadata& generated) {
    if (context.captureSensorSizeMm) {
        generated = buildLensMetadata(context.deviceMake, context.deviceModel, context.focalLengthMm,
                                       context.captureLensOptics, *context.captureSensorSizeMm,
                                       context.captureLensFacing);
        return {generated.make, generated.model, generated.focalLength35mm};
    }
    // Imported DNG metadata belongs to the source camera, not this device.
    return {context.lensMake, context.lensModel, context.focalLength35mm};
}

}  // namespace rawrcam::encoding::jpeg
