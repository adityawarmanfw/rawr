#include "diagnostics/logging/FrameAuditWriter.h"

#include "diagnostics/logging/DiagnosticFileRotation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace rawrcam::diagnostics {
namespace {

std::string jsonString(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (const unsigned char c : value) {
        switch (c) {
            case '"':
                out << "\\\"";
                break;
            case '\\':
                out << "\\\\";
                break;
            case '\b':
                out << "\\b";
                break;
            case '\f':
                out << "\\f";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                if (c < 0x20u) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned int>(c)
                        << std::dec << std::setfill(' ');
                } else {
                    out << static_cast<char>(c);
                }
                break;
        }
    }
    out << '"';
    return out.str();
}

void appendFloat(std::ostringstream& out, float value) {
    if (std::isfinite(value))
        out << value;
    else
        out << "null";
}

template <size_t N>
void appendArray(std::ostringstream& out, const std::array<float, N>& values) {
    out << '[';
    for (size_t i = 0; i < N; ++i) {
        if (i) out << ',';
        appendFloat(out, values[i]);
    }
    out << ']';
}

void appendMatrix(std::ostringstream& out, const metadata::Matrix3x3& matrix) {
    if (!matrix.valid) {
        out << "null";
        return;
    }
    appendArray(out, matrix.rowMajor);
}

void appendRect(std::ostringstream& out, const metadata::RectI& rect) {
    if (!rect.valid) {
        out << "null";
        return;
    }
    out << '[' << rect.left << ',' << rect.top << ',' << rect.right << ',' << rect.bottom << ']';
}

}  // namespace

FrameAuditWriter::FrameAuditWriter(std::string path) : path_(std::move(path)) {}

void FrameAuditWriter::setEnabled(bool enabled) {
    std::lock_guard<std::mutex> streamLock(streamMutex_);
    if (enabled_ == enabled) return;
    enabled_ = enabled;
    if (!enabled_) {
        if (stream_.is_open()) stream_.close();
        return;
    }
    if (!startFile()) enabled_ = false;
}

// Keeps the previous file as "<path>.1" (last session, or the part past the cap).
bool FrameAuditWriter::startFile() {
    if (stream_.is_open()) stream_.close();
    rotateDiagnosticFile(path_);
    stream_.open(path_, std::ios::out | std::ios::trunc);
    if (!stream_) return false;
    static constexpr char kHeader[] =
        "{\"record\":\"session\",\"schemaVersion\":2,\"cameraOwner\":\"NDK_CPP\",\"purpose\":\"sparse_frame_audit\"}\n";
    stream_ << kHeader;
    stream_.flush();
    bytesWritten_ = sizeof(kHeader) - 1;
    return true;
}

void FrameAuditWriter::recordCameraContext(const metadata::CameraContextMetadata& c) {
    if (!enabled_) return;
    {
        std::lock_guard<std::mutex> policyLock(policyMutex_);
        resetGeneration(c.cameraContextGeneration);
    }

    std::ostringstream out;
    out << std::setprecision(9) << "{\"record\":\"cameraContext\""
        << ",\"generation\":" << c.cameraContextGeneration << ",\"cameraId\":" << jsonString(c.cameraId)
        << ",\"lensId\":" << jsonString(c.lensId) << ",\"raw\":[" << c.geometry.rawBufferWidth << ','
        << c.geometry.rawBufferHeight << ']' << ",\"pixelArray\":[" << c.geometry.pixelArrayWidth << ','
        << c.geometry.pixelArrayHeight << ']' << ",\"preCorrectionActiveArray\":";
    appendRect(out, c.geometry.preCorrectionActiveArray);
    out << ",\"activeArray\":";
    appendRect(out, c.geometry.activeArray);
    out << ",\"sensorOrientationDegrees\":" << c.geometry.sensorOrientationDegrees << ",\"cfaCamera2\":" << c.camera2Cfa
        << ",\"cfaRawPreview\":" << c.rawPreviewCfa << ",\"baselineBlackRggb\":";
    appendArray(out, c.baselineBlackLevelPhysicalRggb);
    out << ",\"baselineWhite\":";
    appendFloat(out, c.baselineWhiteLevel);
    out << ",\"referenceIlluminants\":[" << c.color.referenceIlluminant1 << ',' << c.color.referenceIlluminant2 << ']'
        << ",\"colorTransform1\":";
    appendMatrix(out, c.color.colorTransform1);
    out << ",\"colorTransform2\":";
    appendMatrix(out, c.color.colorTransform2);
    out << ",\"calibrationTransform1\":";
    appendMatrix(out, c.color.calibrationTransform1);
    out << ",\"calibrationTransform2\":";
    appendMatrix(out, c.color.calibrationTransform2);
    out << ",\"forwardMatrix1\":";
    appendMatrix(out, c.color.forwardMatrix1);
    out << ",\"forwardMatrix2\":";
    appendMatrix(out, c.color.forwardMatrix2);
    out << '}';
    appendLine(out.str());
}

bool FrameAuditWriter::shouldSparseRecord(const metadata::FrameMetadataSnapshot& frame) const noexcept {
    return frame.frameOrdinal <= 3u || (frame.frameOrdinal % kSparseIntervalFrames) == 0u;
}

std::uint64_t FrameAuditWriter::frameKey(const metadata::FrameMetadataSnapshot& frame) noexcept {
    const std::uint64_t generation = frame.cameraContext ? frame.cameraContext->cameraContextGeneration : 0u;
    // Generation occupies the upper half; frame ordinals are tiny compared with
    // 2^32 in any diagnostic session.
    return (generation << 32u) ^ (frame.frameOrdinal & 0xffffffffull);
}

void FrameAuditWriter::recordCompletedFrame(const metadata::FrameMetadataSnapshot& frame,
                                            const color::FrameColorTransform& colorState) {
    if (!enabled_) return;
    if (!frame.cameraContext) return;
    std::lock_guard<std::mutex> policyLock(policyMutex_);
    const std::uint64_t generation = frame.cameraContext->cameraContextGeneration;
    if (generation != activeGeneration_) resetGeneration(generation);

    if (!shouldSparseRecord(frame)) return;
    CompletedEvidence evidence{frame, colorState};
    writeEvidence(evidence, "sparse");
}

void FrameAuditWriter::writeEvidence(const CompletedEvidence& e, const char* auditReason) {
    const auto& f = e.frame;
    const auto& state = e.colorState;
    const std::uint64_t key = frameKey(f);
    if (std::find(recentWrittenKeys_.begin(), recentWrittenKeys_.end(), key) != recentWrittenKeys_.end()) return;
    recentWrittenKeys_.push_back(key);
    while (recentWrittenKeys_.size() > 64u) recentWrittenKeys_.pop_front();

    std::ostringstream out;
    out << std::setprecision(9) << "{\"record\":\"frameEvidence\""
        << ",\"auditReason\":" << jsonString(auditReason)
        << ",\"generation\":" << f.cameraContext->cameraContextGeneration << ",\"frameOrdinal\":" << f.frameOrdinal
        << ",\"timestampNs\":" << f.timestampNs << ",\"cameraId\":" << jsonString(f.cameraContext->cameraId)
        << ",\"lensId\":" << jsonString(f.cameraContext->lensId) << ",\"raw\":["
        << f.cameraContext->geometry.rawBufferWidth << ',' << f.cameraContext->geometry.rawBufferHeight << ']'
        << ",\"blackRggb\":";
    appendArray(out, f.blackLevelPhysicalRggb);
    out << ",\"staticWhiteLevel\":";
    appendFloat(out, f.staticWhiteLevel);
    out << ",\"reportedDynamicWhiteLevel\":";
    if (f.reportedDynamicWhiteLevel)
        appendFloat(out, *f.reportedDynamicWhiteLevel);
    else
        out << "null";
    out << ",\"effectiveWhiteLevel\":";
    appendFloat(out, f.effectiveWhiteLevel);
    out << ",\"effectiveWhiteLevelSource\":"
        << jsonString(metadata::effectiveWhiteLevelSourceName(f.effectiveWhiteLevelSource));
    out << ",\"colorCorrectionGainsRggb\":";
    appendArray(out, f.colorCorrectionGainsRggb);
    out << ",\"neutralColorPoint\":";
    if (f.hasNeutralColorPoint)
        appendArray(out, f.neutralColorPoint);
    else
        out << "null";
    out << ",\"resultColorTransform\":";
    appendMatrix(out, f.colorCorrectionTransform);
    out << ",\"colorCorrectionMode\":" << f.colorCorrectionMode << ",\"awbMode\":" << f.awbMode
        << ",\"awbState\":" << f.awbState << ",\"exposureTimeNs\":" << f.exposureTimeNs
        << ",\"sensitivity\":" << f.sensitivity << ",\"scalerCropRegion\":";
    appendRect(out, f.scalerCropRegion);
    out << ",\"rawCropRegion\":";
    appendRect(out, f.rawCropRegion);

    out << ",\"colorSource\":" << jsonString(state.source)
        << ",\"wbAppliedByRawPreview\":" << (state.baselineWbAppliedByRawPreview ? "true" : "false")
        << ",\"rawPreviewWbRggb\":";
    appendArray(out, state.baselineWbRggb);
    out << ",\"estimatedWhiteXy\":";
    if (state.hasEstimatedWhite) {
        out << '[';
        appendFloat(out, state.estimatedWhiteX);
        out << ',';
        appendFloat(out, state.estimatedWhiteY);
        out << ']';
    } else {
        out << "null";
    }
    out << ",\"estimatedCctKelvin\":";
    if (state.hasEstimatedWhite)
        appendFloat(out, state.estimatedCctKelvin);
    else
        out << "null";
    out << ",\"calibrationWeight1\":";
    if (state.hasEstimatedWhite)
        appendFloat(out, state.calibrationWeight1);
    else
        out << "null";
    out << ",\"cameraToLinearSrgbRowMajor\":";
    appendArray(out, state.cameraToLinearSrgbRowMajor);
    out << '}';
    appendLine(out.str());
}

void FrameAuditWriter::flush() {
    std::lock_guard<std::mutex> lock(streamMutex_);
    if (stream_.is_open()) stream_.flush();
}

void FrameAuditWriter::appendLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(streamMutex_);
    if (!stream_) return;
    // Diagnostic persistence must never force a filesystem flush from the
    // realtime frame-retirement path. The stream buffer is flushed on close;
    // losing the newest diagnostic lines on a process crash is preferable to
    // stalling the live preview pipeline.
    if (bytesWritten_ >= kDiagnosticFileCapBytes && !startFile()) return;
    stream_ << line << '\n';
    bytesWritten_ += line.size() + 1;
}

void FrameAuditWriter::resetGeneration(std::uint64_t generation) {
    activeGeneration_ = generation;
    recentWrittenKeys_.clear();
}

}  // namespace rawrcam::diagnostics
