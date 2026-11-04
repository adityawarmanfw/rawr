#pragma once

#include "encoding/LensMetadata.h"
#include "encoding/jpeg/JpegCaptureContext.h"

namespace rawrcam::encoding::jpeg {

struct JpegLensMetadataView {
    std::string_view make;
    std::string_view model;
    std::optional<std::uint16_t> focalLength35mm;
};

inline JpegLensMetadataView lensMetadataForJpeg(const JpegCaptureContext& context, LensMetadata& generated) {
    if (context.captureSensorSizeMm) {
        generated = buildLensMetadata(context.deviceMake, context.deviceModel, context.focalLengthMm,
                                       context.captureLensOptics, *context.captureSensorSizeMm);
        return {generated.make, generated.model, generated.focalLength35mm};
    }
    // Imported DNG metadata belongs to the source camera, not this device.
    return {context.lensMake, context.lensModel, context.focalLength35mm};
}

}  // namespace rawrcam::encoding::jpeg
