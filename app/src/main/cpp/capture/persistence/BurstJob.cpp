#include "capture/persistence/ProcessingRecipe.h"
#include <rawr/raw_multiframe_output/MultiframeOutputAdapter.h>

#include <cstring>
#include <chrono>
#include <android/log.h>
#include <stdexcept>
#include <vector>

#include "capture/multiframe/RawBurstUpload.h"
#include "capture/persistence/CaptureJob.h"
#include "vulkan/VulkanContext.h"

namespace rawrcam::capture::persistence {

namespace {
void check(VkResult r) {
    if (r != VK_SUCCESS) throw std::runtime_error("capture_job_vulkan_" + std::to_string(r));
}
}  // namespace
void saveBurst(const std::string& path, multiframe::MultiframeWorkItem& work, const vulkan::VulkanContext& vk,
               std::mutex& mutex) {
    const auto started = std::chrono::steady_clock::now();
    double readbackMs = 0.0;
    // Sharpest-reference selection already ran on the GPU in the spool thread
    // before this call (MfsrCaptureService::spoolLoop), so the persisted
    // reference is final here. Each frame is synchronously read back before
    // its bytes are written; this is not one batched GPU transfer.
    auto& burst = *work.capture;
    CaptureJob job;
    job.multiframe = true;
    job.frame.width = burst.frames.front().raw.ref.width;
    job.frame.height = burst.frames.front().raw.ref.height;
    job.frame.packedRowStrideBytes = job.frame.width * 2;
    job.frame.metadata = burst.referenceMetadata;
    job.frame.colorState = burst.referenceColor;
    job.frame.requestId = work.requestId;
    work.baseDng.resolvedRecipe = resolvedRecipe(work.frozenTonemap, work.frozenTonemap.aePostGain,
                                                work.filmEnabled, work.filmLook);
    work.mergedDng.resolvedRecipe = work.baseDng.resolvedRecipe;
    job.dng = work.baseDng;
    job.mergedDng = work.mergedDng;
    job.jpeg = work.mergedJpeg;
    job.jpegRequested = work.jpegRequested;
    job.tone = work.frozenTonemap;
    job.filmEnabled = work.filmEnabled;
    job.film = work.filmLook;
    job.tuning = work.tuning;
    job.baseFrameMode = work.baseFrameMode;
    // Never persist a score vector that disagrees with the burst; a size
    // mismatch means a bug upstream, and readers treat empty as "unmeasured".
    job.sharpnessScores =
        (work.sharpnessScores.size() == burst.frames.size()) ? work.sharpnessScores : std::vector<float>{};
    job.sharpnessMs = work.sharpnessMs;
    job.referenceIndex = burst.referenceIndex;
    job.metadata = burst.metadata;
    for (auto& f : burst.frames) job.parameters.push_back(f.parameters);
    // CPU-held frames are journaled straight from their source (no GPU readback).
    if (burst.cpuFrame) {
        double readMs = 0.0;
        save(path, job, std::function<void(size_t, std::vector<uint8_t>&)>([&](size_t i, std::vector<uint8_t>& pixels) {
            const auto begin = std::chrono::steady_clock::now();
            burst.cpuFrame(i, pixels);
            readMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        }));
        const double totalMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        __android_log_print(ANDROID_LOG_INFO, "RawrCamNative",
            "BURST_SAVE requestId=%llu frames=%zu source=cpu totalMs=%.1f readMs=%.1f",
            static_cast<unsigned long long>(work.requestId), burst.frames.size(), totalMs, readMs);
        return;
    }
    rawr::raw_multiframe_output::MultiframeOutputAdapter adapter;
    adapter.initialize(
        vk.physicalDevice(), vk.device(), vk.queueFamily(),
        [&](const VkSubmitInfo& info, VkFence fence) {
            std::lock_guard<std::mutex> lock(mutex);
            check(vkQueueSubmit(vk.queue(), 1, &info, fence));
        },
        job.frame.width, job.frame.height);
    save(path, job, [&](size_t i) {
        const auto begin = std::chrono::steady_clock::now();
        auto pixels = adapter.readBase(burst.frames[i].raw.image).pixels;
        readbackMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        return pixels;
    });
    const double totalMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    __android_log_print(ANDROID_LOG_INFO, "RawrCamNative",
        "BURST_SAVE requestId=%llu frames=%zu bytes=%llu totalMs=%.1f readbackMs=%.1f storageAndSetupMs=%.1f",
        static_cast<unsigned long long>(work.requestId), burst.frames.size(),
        static_cast<unsigned long long>(uint64_t(job.frame.width) * job.frame.height * 2 * burst.frames.size()),
        totalMs, readbackMs, totalMs - readbackMs);
}
std::unique_ptr<multiframe::MultiframeWorkItem> loadBurst(const std::string& path, const vulkan::VulkanContext& vk,
                                                          std::mutex& mutex) {
    const auto started = std::chrono::steady_clock::now();
    double uploadMs = 0.0;
    auto work = std::make_unique<multiframe::MultiframeWorkItem>();
    work->capture = std::make_unique<multiframe::PendingMultiframeCapture>();
    auto uploaded = std::make_shared<multiframe::RawBurstUpload>();
    auto job = load(path, [&](CaptureJob& j, size_t i, const std::vector<uint8_t>& raw) {
        if (!j.multiframe) throw std::runtime_error("capture_job_not_burst");
        const auto begin = std::chrono::steady_clock::now();
        if (i == 0) uploaded->initialize(vk, raw.size());
        rawr::raw_gpu_pipeline::BurstFrame frame;
        frame.raw = uploaded->add(vk, mutex, j.frame.width, j.frame.height, j.metadata[i].timestampNs, raw);
        frame.parameters = j.parameters[i];
        work->capture->frames.push_back(frame);
        work->capture->refs.push_back(frame.raw.ref);
        uploadMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    });
    auto& b = *work->capture;
    b.recoveryImages = uploaded;
    b.metadata = job.metadata;
    b.referenceIndex = job.referenceIndex;
    b.referenceMetadata = job.frame.metadata;
    b.referenceColor = job.frame.colorState;
    work->requestId = job.frame.requestId;
    work->baseDng = job.dng;
    work->mergedDng = job.mergedDng;
    work->mergedJpeg = job.jpeg;
    work->jpegRequested = job.jpegRequested;
    work->frozenTonemap = job.tone;
    work->filmEnabled = job.filmEnabled;
    work->filmLook = job.film;
    work->tuning = job.tuning;
    work->baseFrameMode = job.baseFrameMode;
    work->sharpnessScores = job.sharpnessScores;
    work->sharpnessMs = job.sharpnessMs;
    const double totalMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    __android_log_print(ANDROID_LOG_INFO, "RawrCamNative",
        "BURST_LOAD requestId=%llu frames=%zu totalMs=%.1f uploadMs=%.1f storageAndSetupMs=%.1f",
        static_cast<unsigned long long>(work->requestId), b.frames.size(), totalMs, uploadMs, totalMs - uploadMs);
    return work;
}
}  // namespace rawrcam::capture::persistence
