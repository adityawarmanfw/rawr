#pragma once
#include "capture/CaptureRequest.h"
#include "capture/single/SingleFrameCaptureTypes.h"
namespace rawrcam::capture {
const char* singleFrameDemosaicName(develop::DemosaicAlgorithm algorithm) noexcept;
develop::rendered::RenderedStillContext makeSingleFrameRenderContext(
    const imaging::RawSnapshot& captured, const tonemap::TonemapParams& tone, float gain, uint32_t turns, bool film,
    const spektrafilm_native::FilmLook& look, const JpegCaptureRequest* jpeg, const CaptureDiagnostic& diagnostic);
// Lens profile distortion (CameraContextMetadata::profileDistortion) scaled to
// a width x height frame of the lens, e.g. a merge or super-resolution output.
// Sets the calibration and returns true when the profile has one that fits.
// Always carries the profile's mirror over.
bool applyProfileLensDistortion(develop::rendered::RenderedStillContext& rendered,
                                const metadata::CameraContextMetadata& camera, uint32_t width, uint32_t height,
                                bool enabled);
void applySingleFrameCaptureMetadata(encoding::jpeg::JpegCaptureContext& output, const imaging::RawSnapshot& captured);
}  // namespace rawrcam::capture
