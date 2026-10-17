#include "imaging/Raw16CpuSnapshot.h"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>

namespace rawrcam::imaging {
namespace {
using Clock = std::chrono::steady_clock;

double elapsedMs(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

// Shared CPU copy: locks the AHB, decodes RAW16 or RAW10 rows into tightly
// packed RAW16. The source layout comes from the AImage when available.
// lockFenceFd goes to AHardwareBuffer_lock; before the lock it is returned
// untouched in gpuAcquireFenceFd, after a successful unlock that field holds
// the CPU release fence (-2 when the unlock failed).
// HAL_PIXEL_FORMAT_RAW16, the AHB format of a RAW_SENSOR camera buffer.
constexpr uint32_t kHalPixelFormatRaw16 = 0x20;

Raw16CpuSnapshotResult copyToPacked(AImage* image, AHardwareBuffer* ahb, int lockFenceFd, uint32_t width,
                                    uint32_t height, std::vector<uint8_t>& destination) noexcept {
    Raw16CpuSnapshotResult out{};
    out.gpuAcquireFenceFd = lockFenceFd;
    if (ahb == nullptr || width == 0 || height == 0) {
        out.error = "invalid_input";
        return out;
    }
    const uint64_t packedRowBytes = static_cast<uint64_t>(width) * sizeof(uint16_t);
    const uint64_t packedBytes = packedRowBytes * static_cast<uint64_t>(height);
    AHardwareBuffer_Desc desc{};
    AHardwareBuffer_describe(ahb, &desc);

    RawPixelFormat format = RawPixelFormat::Raw16;
    uint64_t sourceRowBytes = static_cast<uint64_t>(desc.stride) * sizeof(uint16_t);
    bool layoutOk = desc.width == width && desc.height == height;
    if (image) {
        int32_t imageWidth = 0;
        int32_t imageHeight = 0;
        int32_t planeCount = 0;
        const auto imageFormat = rawPixelFormatOf(image);
        layoutOk = layoutOk && imageFormat && AImage_getWidth(image, &imageWidth) == AMEDIA_OK &&
                   AImage_getHeight(image, &imageHeight) == AMEDIA_OK &&
                   AImage_getNumberOfPlanes(image, &planeCount) == AMEDIA_OK && planeCount == 1 &&
                   AImage_getPlaneRowStride(image, 0, &out.sourceRowStrideBytes) == AMEDIA_OK &&
                   imageWidth == static_cast<int32_t>(width) && imageHeight == static_cast<int32_t>(height) &&
                   out.sourceRowStrideBytes > 0;
        // Pixel stride is undefined for packed RAW10 (the NDK reports it as
        // unsupported); RAW16 must be exactly two bytes per pixel.
        if (layoutOk && imageFormat == RawPixelFormat::Raw16)
            layoutOk = AImage_getPlanePixelStride(image, 0, &out.sourcePixelStrideBytes) == AMEDIA_OK &&
                       out.sourcePixelStrideBytes == static_cast<int32_t>(sizeof(uint16_t));
        else
            out.sourcePixelStrideBytes = 0;
        if (layoutOk) {
            format = *imageFormat;
            sourceRowBytes = static_cast<uint64_t>(out.sourceRowStrideBytes);
        } else if (imageFormat == RawPixelFormat::Raw16 && desc.width == width && desc.height == height &&
                   desc.format == kHalPixelFormatRaw16 && desc.stride >= width &&
                   (desc.usage & AHARDWAREBUFFER_USAGE_CPU_READ_MASK) == 0) {
            // A reader without CPU usage (vivo full-size modes accept only
            // GPU_DATA_BUFFER): the NDK refuses the AImage plane queries, but the
            // buffer itself describes its RAW16 layout and locks for reading.
            layoutOk = true;
            out.sourceRowStrideBytes = static_cast<int32_t>(sourceRowBytes);
            out.sourcePixelStrideBytes = static_cast<int32_t>(sizeof(uint16_t));
        }
    } else {
        out.sourceRowStrideBytes = static_cast<int32_t>(
            std::min<uint64_t>(sourceRowBytes, static_cast<uint64_t>(std::numeric_limits<int32_t>::max())));
        out.sourcePixelStrideBytes = static_cast<int32_t>(sizeof(uint16_t));
    }
    if (packedBytes > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
        destination.size() != static_cast<size_t>(packedBytes)) {
        out.error = "destination_size_mismatch";
        return out;
    }
    if (!layoutOk || sourceRowBytes < minimumRowBytes(format, width) ||
        sourceRowBytes > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
        out.error = format == RawPixelFormat::Raw10 ? "unsupported_raw10_layout" : "unsupported_raw16_layout";
        return out;
    }

    void* address = nullptr;
    const auto lockBegin = Clock::now();
    const int lockResult =
        AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, lockFenceFd, nullptr, &address);
    out.lockMs = elapsedMs(lockBegin, Clock::now());
    if (lockResult != 0 || address == nullptr) {
        out.error = "ahb_cpu_lock_failed:" + std::to_string(lockResult);
        return out;
    }

    const auto copyBegin = Clock::now();
    const RawPixelSource source{static_cast<const uint8_t*>(address), format, width, height,
                                static_cast<size_t>(sourceRowBytes)};
    const bool copied = copyRawToPackedRaw16(source, reinterpret_cast<uint16_t*>(destination.data()));
    out.copyMs = elapsedMs(copyBegin, Clock::now());

    int cpuReleaseFenceFd = -1;
    const auto unlockBegin = Clock::now();
    const int unlockResult = AHardwareBuffer_unlock(ahb, &cpuReleaseFenceFd);
    out.unlockMs = elapsedMs(unlockBegin, Clock::now());
    if (unlockResult != 0) {
        if (cpuReleaseFenceFd >= 0) close(cpuReleaseFenceFd);
        out.gpuAcquireFenceFd = -2;
        out.error = "ahb_cpu_unlock_failed:" + std::to_string(unlockResult);
        return out;
    }
    out.gpuAcquireFenceFd = cpuReleaseFenceFd;
    if (!copied) {
        out.error = "raw_decode_failed";
        return out;
    }
    out.success = true;
    return out;
}
}  // namespace

Raw16CpuSnapshotResult copyRaw16AhbToPacked(AImage* image, AHardwareBuffer* ahb, int acquireFenceFd, uint32_t width,
                                            uint32_t height, std::vector<uint8_t>& destination) noexcept {
    if (image == nullptr) {
        Raw16CpuSnapshotResult out{};
        out.gpuAcquireFenceFd = acquireFenceFd;
        out.error = "invalid_input";
        return out;
    }
    return copyToPacked(image, ahb, acquireFenceFd, width, height, destination);
}

Raw16CpuSnapshotResult copyCompletedRaw16AhbToPacked(AHardwareBuffer* ahb, uint32_t width, uint32_t height,
                                                     std::vector<uint8_t>& destination, int acquireFenceFd,
                                                     AImage* image) noexcept {
    Raw16CpuSnapshotResult out = copyToPacked(image, ahb, acquireFenceFd, width, height, destination);
    // Worker copies never hand a fence onward; only a release fence created
    // by the unlock is ours to close.
    if (out.gpuAcquireFenceFd >= 0 && out.gpuAcquireFenceFd != acquireFenceFd) close(out.gpuAcquireFenceFd);
    out.gpuAcquireFenceFd = -1;
    return out;
}

std::string stillRawContentStatsLine(const RawSnapshot& snapshot) {
    RawContentStatsInput input;
    const auto& metadata = snapshot.metadata;
    if (metadata.cameraContext) input.cfa = static_cast<int>(metadata.cameraContext->rawPreviewCfa);
    input.blackLevel =
        *std::min_element(metadata.blackLevelPhysicalRggb.begin(), metadata.blackLevelPhysicalRggb.end());
    input.whiteLevel = metadata.effectiveWhiteLevel;
    const RawPixelSource source{snapshot.raw16.data(), RawPixelFormat::Raw16, snapshot.width, snapshot.height,
                                static_cast<size_t>(snapshot.width) * sizeof(uint16_t)};
    return "STILL " + rawContentStatsLine(source, input) + " requestId=" + std::to_string(snapshot.requestId) +
           " exposureTimeNs=" + std::to_string(metadata.exposureTimeNs) +
           " sensitivity=" + std::to_string(metadata.sensitivity);
}

std::optional<RawPixelFormat> rawPixelFormatOf(AImage* image) noexcept {
    int32_t format = 0;
    if (!image || AImage_getFormat(image, &format) != AMEDIA_OK) return std::nullopt;
    if (format == AIMAGE_FORMAT_RAW16) return RawPixelFormat::Raw16;
    if (format == AIMAGE_FORMAT_RAW10) return RawPixelFormat::Raw10;
    return std::nullopt;
}

std::string sampleRawContentStats(AImage* image, AHardwareBuffer* ahb, int acquireFenceFd,
                                  const RawContentStatsInput& input) {
    AHardwareBuffer_Desc desc{};
    if (ahb) AHardwareBuffer_describe(ahb, &desc);
    std::string ahbFacts;
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf), " ahbFormat=0x%x ahbUsage=0x%llx ahbStride=%u", desc.format,
                      static_cast<unsigned long long>(desc.usage), desc.stride);
        ahbFacts = buf;
    }
    const auto format = rawPixelFormatOf(image);
    int32_t rowStride = 0;
    if (!format || AImage_getPlaneRowStride(image, 0, &rowStride) != AMEDIA_OK || rowStride <= 0)
        return "RAW_CONTENT_STATS error=unsupported_image" + ahbFacts;
    if (!(desc.usage & AHARDWAREBUFFER_USAGE_CPU_READ_MASK))
        return "RAW_CONTENT_STATS error=reader_not_cpu_readable" + ahbFacts;

    const int fence = acquireFenceFd >= 0 ? dup(acquireFenceFd) : -1;
    void* address = nullptr;
    const int lockResult = AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, fence, nullptr, &address);
    if (lockResult != 0 || !address)
        return "RAW_CONTENT_STATS error=ahb_cpu_lock_failed:" + std::to_string(lockResult) + ahbFacts;
    const RawPixelSource source{static_cast<const uint8_t*>(address), *format, desc.width, desc.height,
                                static_cast<size_t>(rowStride)};
    std::string line = rawContentStatsLine(source, input) + ahbFacts;
    int releaseFence = -1;
    AHardwareBuffer_unlock(ahb, &releaseFence);
    if (releaseFence >= 0) close(releaseFence);
    return line;
}

}  // namespace rawrcam::imaging
