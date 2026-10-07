#pragma once
#include <spektrafilm/SpektraFilm.h>
#include <tonemap/TonemapEngine.h>

#include <cstdint>
#include <chrono>
#include <memory>
#include <vector>

#include "capture/CaptureRequest.h"
#include "capture/multiframe/FrozenBurst.h"
#include "capture/multiframe/MultiframeBaseFrame.h"
#include "capture/multiframe/MultiframeTuning.h"
#include "encoding/dng/DngCaptureContext.h"
#include "encoding/jpeg/JpegCaptureContext.h"

namespace rawrcam::capture::multiframe {

// Frozen shutter burst transferred from the realtime ring to the background
// worker. Field-identical to FrozenBurst; kept as an alias so
// the two-phase prepare→start contract has a named capture type.
using PendingMultiframeCapture = FrozenBurst;

struct MultiframeWorkItem {
    std::uint64_t requestId = 0;
    // Live-session wall clock, deliberately not serialized across restarts.
    std::chrono::steady_clock::time_point acceptedAt{};
    std::unique_ptr<PendingMultiframeCapture> capture;
    rawrcam::encoding::dng::DngCaptureContext baseDng;
    rawrcam::encoding::dng::DngCaptureContext mergedDng;
    rawrcam::capture::JpegCaptureRequest mergedJpeg;
    bool jpegRequested = false;
    bool dumpRzslRequested = false;
    tonemap::TonemapParams frozenTonemap{};
    bool filmEnabled = false;
    spektrafilm_native::FilmLook filmLook{};
    rawrcam::capture::multiframe::MultiframeTuning tuning{};
    rawrcam::capture::multiframe::MultiframeBaseFrameMode baseFrameMode{};
    // Per-frame GPU sharpness scores, index-aligned with capture->frames.
    // Populated by spool-time selection in Sharpest mode; empty otherwise
    // (Middle mode, scorer fallback). Persisted for RZSL audit + replay.
    std::vector<float> sharpnessScores{};
    // Scoring pass wall time (ms), for EXIF. Zero when unscored.
    double sharpnessMs = 0.0;
};

}  // namespace rawrcam::capture::multiframe
