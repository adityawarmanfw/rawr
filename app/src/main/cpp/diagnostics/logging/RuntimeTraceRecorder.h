#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

namespace rawrcam::diagnostics {

enum class RuntimeTraceStage : uint16_t {
    Metadata = 1,
    RawImage = 2,
    Paired = 3,
    SubmitBegin = 4,
    SlotUnavailable = 5,
    SwapAcquire = 6,
    RecordBegin = 7,
    RecordEnd = 8,
    QueueSubmitBegin = 9,
    QueueSubmitEnd = 10,
    PresentBegin = 11,
    PresentEnd = 12,
    FenceComplete = 13,
    ScopesBegin = 14,
    ScopesEnd = 15,
    AeBegin = 18,
    AeEnd = 19,
    ImageRelease = 20,
    FrameRetired = 21,
    CameraRequestSubmit = 22,
    AeWorkerBegin = 23,
    AeWorkerEnd = 24,
    AeWorkerDropped = 25,
    PostGain = 26,
    RenderP95 = 27,
    RenderP99 = 28,
    RenderClip = 29,
    AutoRenderBoost = 30,
    RenderP50 = 31,
    RenderBright90 = 32,
    RenderBright96 = 33,
    AeRawCellP50 = 34,
    AeRawCellP95 = 35,
    AeRawCellP99 = 36,
    AeRawCellP999 = 37,
    AeRawPhysicalP999 = 38,
    AeRawLogicalClip = 39,
    AeTarget = 40,
    AeSceneDistance = 41,
    AeCandidate = 42,
    AeCandidateIntent = 43,
    AeCandidateShutterCost = 44,
    AeCandidateQuality = 45,
    AeSafetyCeiling = 46,
    ExposureMode = 47,
    ZslRingState = 48,
    ZslRingReady = 49,
    ZslCodecReject = 50,
    ZslRingReject = 51,
    ZslRingFrame = 52,
    ZslRingPushFail = 53,
    ZslRingRetireFail = 54,
    ZslDumpSkip = 55,
    ZslDumpComplete = 56,
    ZslDumpFail = 57,
    ZslArtifactPublished = 58,
    ZslDumpStart = 59,
    ZslDumpProgress = 60,
    MultiframeTrigger = 61,
    MultiframeSnapshot = 62,
    MultiframeWorkerStart = 63,
    MultiframeArenaBegin = 64,
    MultiframeArenaEnd = 65,
    MultiframeRecordBegin = 66,
    MultiframeRecordEnd = 67,
    MultiframeSubmitBegin = 68,
    MultiframeSubmitEnd = 69,
    MultiframeFenceBegin = 70,
    MultiframeFenceEnd = 71,
    MultiframeComplete = 72,
    MultiframeFail = 73,
    MultiframeSnapshotRelease = 74,
    QueueMutexWait = 75,
    QueueMutexAcquired = 76,
    QueueMutexReleased = 77,
    MultiframeQueueMutexWait = 78,
    MultiframeQueueMutexAcquired = 79,
    MultiframeQueueMutexReleased = 80,
    MultiframeChunkBegin = 81,
    MultiframeChunkEnd = 82,
    StillAccepted = 83,
    StillDurable = 84,
    StillProcessingBegin = 85,
    StillProcessingEnd = 86,
    StillRecovered = 87,
    JpegEncodeBegin = 88,
    JpegEncodeEnd = 89,
    CameraSessionCreated = 90,
    CameraCaptureFailed = 91,
    CameraBufferLost = 92,
};

class RuntimeTraceRecorder {
   public:
    static RuntimeTraceRecorder& instance() noexcept;

    void configureOutputPath(std::string path);
    void setEnabled(bool enabled) noexcept;
    void clear() noexcept;
    void setRetainedRows(uint32_t rows) noexcept;
    void setCurrentCameraId(int32_t cameraId) noexcept { currentCameraId_.store(cameraId, std::memory_order_release); }
    [[nodiscard]] bool enabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }
    void record(RuntimeTraceStage stage, uint64_t cameraTimestampNs = 0, uint64_t frameOrdinal = 0, int32_t slot = -1,
                int64_t exposureTimeNs = 0, int32_t sensitivity = 0, uint32_t flags = 0, int64_t value = 0,
                int32_t cameraId = -1) noexcept;
    bool dumpToFile() noexcept;

   private:
    static constexpr uint64_t kCapacity = 32768;
    struct Event {
        std::atomic<uint64_t> sequence{UINT64_MAX};
        uint64_t monotonicNs = 0;
        uint64_t cameraTimestampNs = 0;
        uint64_t frameOrdinal = 0;
        int64_t exposureTimeNs = 0;
        int64_t value = 0;
        int32_t sensitivity = 0;
        int32_t slot = -1;
        int32_t cameraId = -1;
        uint32_t flags = 0;
        uint16_t stage = 0;
    };

    RuntimeTraceRecorder() = default;
    static uint64_t monotonicNowNs() noexcept;
    static const char* stageName(uint16_t stage) noexcept;

    std::array<Event, kCapacity> events_{};
    std::atomic<uint64_t> nextSequence_{0};
    std::atomic<bool> enabled_{false};
    std::atomic<uint32_t> retainedRows_{8192};
    std::atomic<int32_t> currentCameraId_{-1};
    std::string outputPath_;
};

}  // namespace rawrcam::diagnostics
