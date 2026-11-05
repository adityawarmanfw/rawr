#pragma once

#include <android/hardware_buffer.h>
#include <media/NdkImage.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "imaging/RawPixelSource.h"
#include "imaging/RawSnapshot.h"

namespace rawrcam::imaging {

struct Raw16CpuSnapshotResult {
    bool success = false;
    int gpuAcquireFenceFd = -1;
    int32_t sourceRowStrideBytes = 0;
    int32_t sourcePixelStrideBytes = 0;
    double lockMs = 0.0;
    double copyMs = 0.0;
    double unlockMs = 0.0;
    std::string error;
};

// Low-level RAW (RAW16 or RAW10, decoded to packed RAW16) ownership transfer used by both the feasibility probe and
// production still snapshotting. The caller owns acquireFenceFd. On success,
// gpuAcquireFenceFd is the CPU-unlock release fence that subsequent Vulkan work
// must wait on (-1 means immediately available). Destination storage must
// already be sized to width * height * sizeof(uint16_t).
Raw16CpuSnapshotResult copyRaw16AhbToPacked(AImage* image, AHardwareBuffer* ahb, int acquireFenceFd, uint32_t width,
                                            uint32_t height, std::vector<uint8_t>& destination) noexcept;

// Worker still copy. The caller must retain the AImage lease throughout the read.
// After preview retirement pass -1; for an unsubmitted camera frame pass its
// acquire fence. Fence ownership remains with the caller. Pass the frame's
// AImage so RAW10 and padded rows are decoded from the image's plane layout.
Raw16CpuSnapshotResult copyCompletedRaw16AhbToPacked(AHardwareBuffer* ahb, uint32_t width, uint32_t height,
                                                     std::vector<uint8_t>& destination, int acquireFenceFd = -1,
                                                     AImage* image = nullptr) noexcept;

// RAW_CONTENT_STATS for a completed still snapshot (packed RAW16), logged so
// device logs show whether a dark still had dark sensor data.
[[nodiscard]] std::string stillRawContentStatsLine(const RawSnapshot& snapshot);

// Camera RAW pixel layout of an AImage, from its AHB format (RAW16 or RAW10).
[[nodiscard]] std::optional<RawPixelFormat> rawPixelFormatOf(AImage* image) noexcept;

// Read-only CPU sample of a live camera frame for RAW_CONTENT_STATS. Requires a
// CPU-readable reader; otherwise returns a line describing why it could not
// sample. acquireFenceFd stays owned by the caller.
[[nodiscard]] std::optional<std::array<float, 4>> sampleRawBlackFloor(
    AImage* image, AHardwareBuffer* ahb, int acquireFenceFd, int cfa);

[[nodiscard]] std::string sampleRawContentStats(AImage* image, AHardwareBuffer* ahb, int acquireFenceFd,
                                                const RawContentStatsInput& input);

}  // namespace rawrcam::imaging
