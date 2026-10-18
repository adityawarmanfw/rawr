#pragma once
#include <functional>
#include <memory>
#include <string>

#include "capture/CaptureRequest.h"
#include "capture/multiframe/MultiframeWorkItem.h"
#include "imaging/RawSnapshot.h"
namespace rawrcam::vulkan {
class VulkanContext;
}
namespace rawrcam::capture::persistence {
// Versioned, app-private recovery input. Never serialize FDs or GPU handles.
struct CaptureJob {
    rawrcam::imaging::RawSnapshot frame;
    encoding::dng::DngCaptureContext dng, mergedDng;
    rawrcam::capture::JpegCaptureRequest jpeg;
    tonemap::TonemapParams tone{};
    float gain = 1;
    bool filmEnabled = false, jpegRequested = false, multiframe = false;
    spektrafilm_native::FilmLook film{};
    multiframe::MultiframeTuning tuning{};
    multiframe::MultiframeBaseFrameMode baseFrameMode = multiframe::MultiframeBaseFrameMode::Sharpest;
    // Per-frame sharpness scores, index-aligned with metadata/parameters.
    // Empty when unmeasured (Middle mode, fallback, old jobs).
    std::vector<float> sharpnessScores{};
    double sharpnessMs = 0.0;
    uint32_t referenceIndex = 0;
    std::vector<metadata::FrameMetadataSnapshot> metadata;
    std::vector<rawr::raw_gpu_pipeline::MultiframeFrameParameters> parameters;
};
void markFilmFallback(const std::string& path);
std::string jobPath(const std::string& filesDir, const std::string& name);
// Reservation covers not-yet-spooled input and output headroom; existing jobs
// are already included in filesystem free space. Released by RAII on failure.
std::shared_ptr<void> reserve(const std::string& filesDir, uint64_t rawBytes, bool cpuAcquisition);
void save(const std::string& path, CaptureJob& job, const std::function<std::vector<uint8_t>(size_t)>& readFrame = {});
// Bursts whose frames fill one reused buffer (no per-frame allocation).
void save(const std::string& path, CaptureJob& job,
          const std::function<void(size_t, std::vector<uint8_t>&)>& fillFrame);
CaptureJob load(const std::string& path,
                const std::function<void(CaptureJob&, size_t, const std::vector<uint8_t>&)>& consume = {});
// Header only (frames validated by extent, not read): frame i's packed RAW16
// starts at framesOffset + i * width * height * 2.
CaptureJob loadHeader(const std::string& path, uint64_t& framesOffset);
void saveBurst(const std::string& path, multiframe::MultiframeWorkItem& work, const vulkan::VulkanContext& vulkan,
               std::mutex& queueMutex);
// lazy: only the reference frame is uploaded; the rest load on demand
// (burst.loadFrame) and drop after the merge (burst.releaseFrame).
std::unique_ptr<multiframe::MultiframeWorkItem> loadBurst(const std::string& path, const vulkan::VulkanContext& vulkan,
                                                          std::mutex& queueMutex, bool lazy = false);
}  // namespace rawrcam::capture::persistence
