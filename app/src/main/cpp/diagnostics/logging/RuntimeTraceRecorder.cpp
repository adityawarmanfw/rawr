#include "diagnostics/logging/RuntimeTraceRecorder.h"

#include <algorithm>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <utility>

namespace rawrcam::diagnostics {

RuntimeTraceRecorder& RuntimeTraceRecorder::instance() noexcept {
    static RuntimeTraceRecorder recorder;
    return recorder;
}

uint64_t RuntimeTraceRecorder::monotonicNowNs() noexcept {
    timespec ts{};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

void RuntimeTraceRecorder::configureOutputPath(std::string path) { outputPath_ = std::move(path); }

void RuntimeTraceRecorder::setEnabled(bool enabled) noexcept {
    if (enabled) {
        nextSequence_.store(0, std::memory_order_relaxed);
        for (auto& event : events_) event.sequence.store(UINT64_MAX, std::memory_order_relaxed);
    }
    enabled_.store(enabled, std::memory_order_release);
}

void RuntimeTraceRecorder::setRetainedRows(uint32_t rows) noexcept {
    if (rows != 4096 && rows != 8192 && rows != 16384 && rows != 32768) rows = 8192;
    retainedRows_.store(rows, std::memory_order_release);
}

void RuntimeTraceRecorder::clear() noexcept {
    nextSequence_.store(0, std::memory_order_relaxed);
    for (auto& event : events_) event.sequence.store(UINT64_MAX, std::memory_order_relaxed);
}

void RuntimeTraceRecorder::record(RuntimeTraceStage stage, uint64_t cameraTimestampNs, uint64_t frameOrdinal,
                                  int32_t slot, int64_t exposureTimeNs, int32_t sensitivity, uint32_t flags,
                                  int64_t value, int32_t cameraId) noexcept {
    if (!enabled_.load(std::memory_order_relaxed)) return;
    const uint64_t sequence = nextSequence_.fetch_add(1, std::memory_order_relaxed);
    auto& event = events_[sequence % kCapacity];
    event.monotonicNs = monotonicNowNs();
    event.cameraTimestampNs = cameraTimestampNs;
    event.frameOrdinal = frameOrdinal;
    event.exposureTimeNs = exposureTimeNs;
    event.value = value;
    event.sensitivity = sensitivity;
    event.slot = slot;
    event.cameraId = cameraId >= 0 ? cameraId : currentCameraId_.load(std::memory_order_acquire);
    event.flags = flags;
    event.stage = static_cast<uint16_t>(stage);
    std::atomic_thread_fence(std::memory_order_release);
    event.sequence.store(sequence, std::memory_order_release);
}

const char* RuntimeTraceRecorder::stageName(uint16_t stage) noexcept {
    switch (static_cast<RuntimeTraceStage>(stage)) {
        case RuntimeTraceStage::StillAccepted:
            return "STILL_ACCEPTED";
        case RuntimeTraceStage::StillDurable:
            return "STILL_DURABLE";
        case RuntimeTraceStage::StillProcessingBegin:
            return "STILL_PROCESSING_BEGIN";
        case RuntimeTraceStage::StillProcessingEnd:
            return "STILL_PROCESSING_END";
        case RuntimeTraceStage::StillRecovered:
            return "STILL_RECOVERED";
        case RuntimeTraceStage::JpegEncodeBegin:
            return "JPEG_ENCODE_BEGIN";
        case RuntimeTraceStage::JpegEncodeEnd:
            return "JPEG_ENCODE_END";
        case RuntimeTraceStage::CameraSessionCreated:
            return "CAMERA_SESSION_CREATED";
        case RuntimeTraceStage::CameraCaptureFailed:
            return "CAMERA_CAPTURE_FAILED";
        case RuntimeTraceStage::CameraBufferLost:
            return "CAMERA_BUFFER_LOST";
        case RuntimeTraceStage::Metadata:
            return "METADATA";
        case RuntimeTraceStage::RawImage:
            return "RAW_IMAGE";
        case RuntimeTraceStage::Paired:
            return "PAIRED";
        case RuntimeTraceStage::SubmitBegin:
            return "SUBMIT_BEGIN";
        case RuntimeTraceStage::SlotUnavailable:
            return "SLOT_UNAVAILABLE";
        case RuntimeTraceStage::SwapAcquire:
            return "SWAP_ACQUIRE";
        case RuntimeTraceStage::RecordBegin:
            return "RECORD_BEGIN";
        case RuntimeTraceStage::RecordEnd:
            return "RECORD_END";
        case RuntimeTraceStage::QueueSubmitBegin:
            return "QUEUE_SUBMIT_BEGIN";
        case RuntimeTraceStage::QueueSubmitEnd:
            return "QUEUE_SUBMIT_END";
        case RuntimeTraceStage::PresentBegin:
            return "PRESENT_BEGIN";
        case RuntimeTraceStage::PresentEnd:
            return "PRESENT_END";
        case RuntimeTraceStage::FenceComplete:
            return "FENCE_COMPLETE";
        case RuntimeTraceStage::ScopesBegin:
            return "SCOPES_BEGIN";
        case RuntimeTraceStage::ScopesEnd:
            return "SCOPES_END";
        case RuntimeTraceStage::AeBegin:
            return "AE_BEGIN";
        case RuntimeTraceStage::AeEnd:
            return "AE_END";
        case RuntimeTraceStage::ImageRelease:
            return "IMAGE_RELEASE";
        case RuntimeTraceStage::FrameRetired:
            return "FRAME_RETIRED";
        case RuntimeTraceStage::CameraRequestSubmit:
            return "CAMERA_REQUEST_SUBMIT";
        case RuntimeTraceStage::AeWorkerBegin:
            return "AE_WORKER_BEGIN";
        case RuntimeTraceStage::AeWorkerEnd:
            return "AE_WORKER_END";
        case RuntimeTraceStage::AeWorkerDropped:
            return "AE_WORKER_DROPPED";
        case RuntimeTraceStage::PostGain:
            return "POST_GAIN";
        case RuntimeTraceStage::RenderP95:
            return "RENDER_P95";
        case RuntimeTraceStage::RenderP99:
            return "RENDER_P99";
        case RuntimeTraceStage::RenderClip:
            return "RENDER_CLIP";
        case RuntimeTraceStage::AutoRenderBoost:
            return "AUTO_RENDER_BOOST";
        case RuntimeTraceStage::RenderP50:
            return "RENDER_P50";
        case RuntimeTraceStage::RenderBright90:
            return "RENDER_BRIGHT90";
        case RuntimeTraceStage::RenderBright96:
            return "RENDER_BRIGHT96";
        case RuntimeTraceStage::AeRawCellP50:
            return "AE_RAW_CELL_P50";
        case RuntimeTraceStage::AeRawCellP95:
            return "AE_RAW_CELL_P95";
        case RuntimeTraceStage::AeRawCellP99:
            return "AE_RAW_CELL_P99";
        case RuntimeTraceStage::AeRawCellP999:
            return "AE_RAW_CELL_P999";
        case RuntimeTraceStage::AeRawPhysicalP999:
            return "AE_RAW_PHYSICAL_P999";
        case RuntimeTraceStage::AeRawLogicalClip:
            return "AE_RAW_LOGICAL_CLIP";
        case RuntimeTraceStage::AeTarget:
            return "AE_TARGET";
        case RuntimeTraceStage::AeSceneDistance:
            return "AE_SCENE_DISTANCE";
        case RuntimeTraceStage::AeCandidate:
            return "AE_CANDIDATE";
        case RuntimeTraceStage::AeCandidateIntent:
            return "AE_CANDIDATE_INTENT";
        case RuntimeTraceStage::AeCandidateShutterCost:
            return "AE_CANDIDATE_SHUTTER_COST";
        case RuntimeTraceStage::AeCandidateQuality:
            return "AE_CANDIDATE_QUALITY";
        case RuntimeTraceStage::AeSafetyCeiling:
            return "AE_SAFETY_CEILING";
        case RuntimeTraceStage::ExposureMode:
            return "EXPOSURE_MODE";
        case RuntimeTraceStage::ZslRingState:
            return "ZSL_RING_STATE";
        case RuntimeTraceStage::ZslRingReady:
            return "ZSL_RING_READY";
        case RuntimeTraceStage::ZslCodecReject:
            return "ZSL_CODEC_REJECT";
        case RuntimeTraceStage::ZslRingReject:
            return "ZSL_RING_REJECT";
        case RuntimeTraceStage::ZslRingFrame:
            return "ZSL_RING_FRAME";
        case RuntimeTraceStage::ZslRingPushFail:
            return "ZSL_RING_PUSH_FAIL";
        case RuntimeTraceStage::ZslRingRetireFail:
            return "ZSL_RING_RETIRE_FAIL";
        case RuntimeTraceStage::ZslDumpSkip:
            return "ZSL_RING_DUMP_SKIP";
        case RuntimeTraceStage::ZslDumpComplete:
            return "ZSL_RING_DUMP_COMPLETE";
        case RuntimeTraceStage::ZslDumpFail:
            return "ZSL_RING_DUMP_FAIL";
        case RuntimeTraceStage::ZslArtifactPublished:
            return "ZSL_ARTIFACT_PUBLISHED";
        case RuntimeTraceStage::ZslDumpStart:
            return "ZSL_RING_DUMP_START";
        case RuntimeTraceStage::ZslDumpProgress:
            return "ZSL_RING_DUMP_PROGRESS";
        case RuntimeTraceStage::MultiframeTrigger:
            return "MULTIFRAME_TRIGGER";
        case RuntimeTraceStage::MultiframeSnapshot:
            return "MULTIFRAME_SNAPSHOT";
        case RuntimeTraceStage::MultiframeWorkerStart:
            return "MULTIFRAME_WORKER_START";
        case RuntimeTraceStage::MultiframeArenaBegin:
            return "MULTIFRAME_ARENA_BEGIN";
        case RuntimeTraceStage::MultiframeArenaEnd:
            return "MULTIFRAME_ARENA_END";
        case RuntimeTraceStage::MultiframeRecordBegin:
            return "MULTIFRAME_RECORD_BEGIN";
        case RuntimeTraceStage::MultiframeRecordEnd:
            return "MULTIFRAME_RECORD_END";
        case RuntimeTraceStage::MultiframeSubmitBegin:
            return "MULTIFRAME_SUBMIT_BEGIN";
        case RuntimeTraceStage::MultiframeSubmitEnd:
            return "MULTIFRAME_SUBMIT_END";
        case RuntimeTraceStage::MultiframeFenceBegin:
            return "MULTIFRAME_FENCE_BEGIN";
        case RuntimeTraceStage::MultiframeFenceEnd:
            return "MULTIFRAME_FENCE_END";
        case RuntimeTraceStage::MultiframeComplete:
            return "MULTIFRAME_COMPLETE";
        case RuntimeTraceStage::MultiframeFail:
            return "MULTIFRAME_FAIL";
        case RuntimeTraceStage::MultiframeSnapshotRelease:
            return "MULTIFRAME_SNAPSHOT_RELEASE";
        case RuntimeTraceStage::QueueMutexWait:
            return "QUEUE_MUTEX_WAIT";
        case RuntimeTraceStage::QueueMutexAcquired:
            return "QUEUE_MUTEX_ACQUIRED";
        case RuntimeTraceStage::QueueMutexReleased:
            return "QUEUE_MUTEX_RELEASED";
        case RuntimeTraceStage::MultiframeQueueMutexWait:
            return "MULTIFRAME_QUEUE_MUTEX_WAIT";
        case RuntimeTraceStage::MultiframeQueueMutexAcquired:
            return "MULTIFRAME_QUEUE_MUTEX_ACQUIRED";
        case RuntimeTraceStage::MultiframeQueueMutexReleased:
            return "MULTIFRAME_QUEUE_MUTEX_RELEASED";
        case RuntimeTraceStage::MultiframeChunkBegin:
            return "MULTIFRAME_CHUNK_BEGIN";
        case RuntimeTraceStage::MultiframeChunkEnd:
            return "MULTIFRAME_CHUNK_END";
    }
    return "UNKNOWN";
}

bool RuntimeTraceRecorder::dumpToFile() noexcept {
    if (outputPath_.empty()) return false;
    const bool wasEnabled = enabled_.exchange(false, std::memory_order_acq_rel);
    const uint64_t end = nextSequence_.load(std::memory_order_acquire);
    const uint64_t retained = std::min<uint64_t>(retainedRows_.load(std::memory_order_acquire), kCapacity);
    const uint64_t begin = end > retained ? end - retained : 0;
    std::ofstream out(outputPath_, std::ios::out | std::ios::trunc);
    if (!out) {
        enabled_.store(wasEnabled, std::memory_order_release);
        return false;
    }
    out << "RAWR INTERNAL RUNTIME TRACE v2\n";
    out << "events=" << (end - begin) << " retained=" << retained << " capacity=" << kCapacity << "\n";
    out << "columns: seq dt_ms stage frame camera_ts_ns camera_id slot ss_ns iso flags value\n";
    out << "exposure_mode: flags selected semantic 0=A 1=M 2=S 3=I; value actual Camera2 control 0=A 1=M 2=S 3=I; "
           "bit31=snapback\n";
    out << "zsl: STATE flags=enabled; READY flags=maxFrames value=rawPixels; FRAME flags=ringFrames value=gpuBytes; "
           "DUMP_START flags=frameCount value=gpuBytes; DUMP_PROGRESS flags=framesDone value=packetBytes; REJECT/SKIP "
           "flags=reason; DUMP_COMPLETE flags=frameCount value=bytes; ARTIFACT_PUBLISHED flags=success value=bytes\n";
    out << "multiframe: TRIGGER flags=status value=ringFrames; SNAPSHOT flags=frameCount value=ringBytes; "
           "ARENA/RECORD/SUBMIT/FENCE flags=stage payload value=imageMiB_x1000; COMPLETE flags=frameCount "
           "value=imageMiB_x1000; FAIL flags=reason; SNAPSHOT_RELEASE flags=frameCount\n";
    out << "camera: frame=generation; SESSION_CREATED flags=companionStream value=rawW<<32|rawH; CAPTURE_FAILED "
           "flags=reason(0=error 1=flushed) slot=sequenceId value=frameNumber; BUFFER_LOST value=frameNumber\n";
    uint64_t firstNs = 0;
    for (uint64_t sequence = begin; sequence < end; ++sequence) {
        const auto& event = events_[sequence % kCapacity];
        if (event.sequence.load(std::memory_order_acquire) != sequence) continue;
        if (firstNs == 0) firstNs = event.monotonicNs;
        const double deltaMs = static_cast<double>(event.monotonicNs - firstNs) / 1.0e6;
        out << sequence << ' ' << std::fixed << std::setprecision(3) << deltaMs << ' ' << stageName(event.stage) << ' '
            << event.frameOrdinal << ' ' << event.cameraTimestampNs << ' ' << event.cameraId << ' ' << event.slot << ' '
            << event.exposureTimeNs << ' ' << event.sensitivity << ' ' << event.flags << ' ' << event.value << '\n';
    }
    out.flush();
    enabled_.store(wasEnabled, std::memory_order_release);
    return static_cast<bool>(out);
}

}  // namespace rawrcam::diagnostics
