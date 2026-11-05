#pragma once

#include <cstdint>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>

#include "color/FrameColorTransform.h"
#include "metadata/CameraContextMetadata.h"
#include "metadata/FrameMetadataSnapshot.h"

namespace rawrcam::diagnostics {

// Durable, diagnostic-only JSONL writer for completed, frame-correlated capture
// evidence. It owns only persistence policy: sparse baseline sampling. It never
// derives camera/color state and never talks to Camera2, Vulkan, JNI, or the UI.
class FrameAuditWriter {
   public:
    explicit FrameAuditWriter(std::string path);

    bool available() const noexcept { return enabled_ && stream_.is_open(); }
    void setEnabled(bool enabled);
    // Off the frame path only: makes the file complete for an export.
    void flush();
    void recordCameraContext(const metadata::CameraContextMetadata& context);
    void recordCompletedFrame(const metadata::FrameMetadataSnapshot& frame,
                              const color::FrameColorTransform& colorState);

   private:
    struct CompletedEvidence {
        metadata::FrameMetadataSnapshot frame;
        color::FrameColorTransform colorState;
    };

    static constexpr std::uint32_t kSparseIntervalFrames = 15;

    bool shouldSparseRecord(const metadata::FrameMetadataSnapshot& frame) const noexcept;
    static std::uint64_t frameKey(const metadata::FrameMetadataSnapshot& frame) noexcept;
    void writeEvidence(const CompletedEvidence& evidence, const char* auditReason);
    void appendLine(const std::string& line);
    bool startFile();
    void resetGeneration(std::uint64_t generation);

    std::string path_;
    mutable std::mutex streamMutex_;
    std::mutex policyMutex_;
    std::ofstream stream_;
    std::deque<std::uint64_t> recentWrittenKeys_;
    std::uint64_t activeGeneration_ = 0;
    std::uint64_t bytesWritten_ = 0;
    bool enabled_ = false;
};

}  // namespace rawrcam::diagnostics
