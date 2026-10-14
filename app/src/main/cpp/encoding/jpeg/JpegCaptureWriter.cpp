#include "encoding/SoftwareName.h"
#include "encoding/jpeg/JpegCaptureWriter.h"

#include <android/log.h>
#include <gainmap/UltrahdrMux.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <turbojpeg.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#include "diagnostics/logging/RuntimeTraceRecorder.h"
#include "encoding/jpeg/JpegTimingsFormat.h"
#include "encoding/jpeg/UltraHdrMapFit.h"

namespace rawrcam::encoding::jpeg {
namespace {

bool writeAll(int fd, const unsigned char* data, std::size_t size) {
    while (size > 0) {
        const ssize_t n = ::write(fd, data, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        data += static_cast<std::size_t>(n);
        size -= static_cast<std::size_t>(n);
    }
    return true;
}

void put16(std::vector<unsigned char>& out, std::uint16_t v) {
    out.push_back(static_cast<unsigned char>(v & 0xffu));
    out.push_back(static_cast<unsigned char>((v >> 8u) & 0xffu));
}
void put32(std::vector<unsigned char>& out, std::uint32_t v) {
    out.push_back(static_cast<unsigned char>(v & 0xffu));
    out.push_back(static_cast<unsigned char>((v >> 8u) & 0xffu));
    out.push_back(static_cast<unsigned char>((v >> 16u) & 0xffu));
    out.push_back(static_cast<unsigned char>((v >> 24u) & 0xffu));
}
void patch32(std::vector<unsigned char>& out, std::size_t pos, std::uint32_t v) {
    out[pos + 0] = static_cast<unsigned char>(v & 0xffu);
    out[pos + 1] = static_cast<unsigned char>((v >> 8u) & 0xffu);
    out[pos + 2] = static_cast<unsigned char>((v >> 16u) & 0xffu);
    out[pos + 3] = static_cast<unsigned char>((v >> 24u) & 0xffu);
}

struct IfdEntry {
    std::uint16_t tag = 0;
    std::uint16_t type = 0;
    std::uint32_t count = 0;
    std::vector<unsigned char> data;
};

std::vector<unsigned char> asciiData(const std::string& s) {
    std::vector<unsigned char> out(s.begin(), s.end());
    out.push_back(0);
    return out;
}
std::vector<unsigned char> shortData(std::uint16_t v) {
    std::vector<unsigned char> out;
    put16(out, v);
    return out;
}
std::vector<unsigned char> longData(std::uint32_t v) {
    std::vector<unsigned char> out;
    put32(out, v);
    return out;
}
std::vector<unsigned char> rationalData(double value, std::uint32_t denominator = 1000000u) {
    if (!std::isfinite(value) || value < 0.0) value = 0.0;
    const double scaled = std::round(value * static_cast<double>(denominator));
    const auto numerator = static_cast<std::uint32_t>(
        std::clamp(scaled, 0.0, static_cast<double>(std::numeric_limits<std::uint32_t>::max())));
    std::vector<unsigned char> out;
    put32(out, numerator);
    put32(out, denominator);
    return out;
}

std::string exifDateTime(std::int64_t millis, std::int16_t offsetMinutes) {
    if (millis <= 0) return {};
    const time_t localSeconds = static_cast<time_t>(millis / 1000 + static_cast<std::int64_t>(offsetMinutes) * 60);
    struct tm local{};
    if (!gmtime_r(&localSeconds, &local)) return {};
    char buf[20]{};
    if (std::strftime(buf, sizeof(buf), "%Y:%m:%d %H:%M:%S", &local) == 0) return {};
    return std::string(buf);
}

std::string exifOffset(std::int16_t offsetMinutes) {
    const int total = static_cast<int>(offsetMinutes);
    const char sign = total < 0 ? '-' : '+';
    const int absMinutes = std::abs(total);
    char buf[8]{};
    const int hours = std::min(absMinutes / 60, 99);
    const int minutes = std::min(absMinutes % 60, 59);
    std::snprintf(buf, sizeof(buf), "%c%02d:%02d", sign, hours, minutes);
    return std::string(buf);
}

// Builds a compact EXIF APP1 payload. TIFF offsets are relative to the TIFF header
// immediately following the six-byte "Exif\0\0" signature.
std::vector<unsigned char> buildExif(const JpegCaptureContext& c, std::uint32_t width, std::uint32_t height) {
    const std::string dateTime = exifDateTime(c.wallClockUnixMillis, c.utcOffsetMinutes);
    const std::string offset = exifOffset(c.utcOffsetMinutes);
    const std::string subsec =
        c.wallClockUnixMillis > 0 ? std::to_string(c.wallClockUnixMillis % 1000 + 1000).substr(1) : std::string{};

    std::vector<IfdEntry> ifd0;
    if (!c.imageDescription.empty()) {
        ifd0.push_back(
            {0x010E, 2, static_cast<std::uint32_t>(c.imageDescription.size() + 1), asciiData(c.imageDescription)});
    }
    if (!c.deviceMake.empty())
        ifd0.push_back({0x010F, 2, static_cast<std::uint32_t>(c.deviceMake.size() + 1), asciiData(c.deviceMake)});
    if (!c.deviceModel.empty())
        ifd0.push_back({0x0110, 2, static_cast<std::uint32_t>(c.deviceModel.size() + 1), asciiData(c.deviceModel)});
    ifd0.push_back({0x0112, 3, 1, shortData(c.exifOrientation)});
    const std::string software = rawrcam::encoding::kSoftwareName;
    ifd0.push_back({0x0131, 2, static_cast<std::uint32_t>(software.size() + 1), asciiData(software)});
    if (!dateTime.empty()) ifd0.push_back({0x0132, 2, 20, asciiData(dateTime)});
    // ExifIFDPointer placeholder; value is patched after IFD0 layout is known.
    ifd0.push_back({0x8769, 4, 1, longData(0)});

    std::vector<IfdEntry> exif;
    if (c.exposureTimeNs > 0)
        exif.push_back({0x829A, 5, 1, rationalData(static_cast<double>(c.exposureTimeNs) / 1.0e9, 1000000u)});
    if (c.aperture && *c.aperture > 0.0f) exif.push_back({0x829D, 5, 1, rationalData(*c.aperture, 10000u)});
    if (c.sensitivity > 0 && c.sensitivity <= 65535)
        exif.push_back({0x8827, 3, 1, shortData(static_cast<std::uint16_t>(c.sensitivity))});
    exif.push_back({0x9000, 7, 4, std::vector<unsigned char>{'0', '2', '3', '2'}});
    if (!dateTime.empty()) {
        exif.push_back({0x9003, 2, 20, asciiData(dateTime)});
        exif.push_back({0x9004, 2, 20, asciiData(dateTime)});
        exif.push_back({0x9011, 2, static_cast<std::uint32_t>(offset.size() + 1), asciiData(offset)});
    }
    if (!subsec.empty()) exif.push_back({0x9291, 2, static_cast<std::uint32_t>(subsec.size() + 1), asciiData(subsec)});
    if (c.focalLengthMm && *c.focalLengthMm > 0.0f)
        exif.push_back({0x920A, 5, 1, rationalData(*c.focalLengthMm, 10000u)});
    exif.push_back({0xA001, 3, 1, shortData(1)});  // sRGB
    exif.push_back({0xA002, 4, 1, longData(width)});
    exif.push_back({0xA003, 4, 1, longData(height)});

    std::vector<unsigned char> tiff;
    tiff.insert(tiff.end(), {'I', 'I'});
    put16(tiff, 42);
    put32(tiff, 8);

    auto appendIfd = [&](const std::vector<IfdEntry>& entries) -> std::uint32_t {
        const std::uint32_t ifdOffset = static_cast<std::uint32_t>(tiff.size());
        put16(tiff, static_cast<std::uint16_t>(entries.size()));
        const std::size_t tableStart = tiff.size();
        tiff.resize(tiff.size() + entries.size() * 12u + 4u, 0);
        std::vector<std::pair<std::size_t, std::vector<unsigned char>>> external;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const auto& e = entries[i];
            const std::size_t p = tableStart + i * 12u;
            tiff[p + 0] = static_cast<unsigned char>(e.tag & 0xffu);
            tiff[p + 1] = static_cast<unsigned char>(e.tag >> 8u);
            tiff[p + 2] = static_cast<unsigned char>(e.type & 0xffu);
            tiff[p + 3] = static_cast<unsigned char>(e.type >> 8u);
            patch32(tiff, p + 4, e.count);
            if (e.data.size() <= 4u) {
                std::copy(e.data.begin(), e.data.end(), tiff.begin() + static_cast<std::ptrdiff_t>(p + 8));
            } else {
                external.emplace_back(p + 8, e.data);
            }
        }
        for (auto& [offsetPos, bytes] : external) {
            patch32(tiff, offsetPos, static_cast<std::uint32_t>(tiff.size()));
            tiff.insert(tiff.end(), bytes.begin(), bytes.end());
            if ((tiff.size() & 1u) != 0u) tiff.push_back(0);
        }
        return ifdOffset;
    };

    std::sort(ifd0.begin(), ifd0.end(), [](const IfdEntry& a, const IfdEntry& b) { return a.tag < b.tag; });
    std::sort(exif.begin(), exif.end(), [](const IfdEntry& a, const IfdEntry& b) { return a.tag < b.tag; });
    const std::uint32_t ifd0Offset = appendIfd(ifd0);
    const std::uint32_t exifOffset = appendIfd(exif);
    // Locate ExifIFDPointer entry in IFD0 and patch its value.
    const std::size_t countPos = ifd0Offset;
    const std::uint16_t count =
        static_cast<std::uint16_t>(tiff[countPos] | (static_cast<std::uint16_t>(tiff[countPos + 1]) << 8u));
    for (std::uint16_t i = 0; i < count; ++i) {
        const std::size_t p = countPos + 2u + static_cast<std::size_t>(i) * 12u;
        const std::uint16_t tag = static_cast<std::uint16_t>(tiff[p] | (static_cast<std::uint16_t>(tiff[p + 1]) << 8u));
        if (tag == 0x8769u) {
            patch32(tiff, p + 8u, exifOffset);
            break;
        }
    }

    std::vector<unsigned char> payload{'E', 'x', 'i', 'f', 0, 0};
    payload.insert(payload.end(), tiff.begin(), tiff.end());
    return payload;
}

std::string xmlEscape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 64);
    for (char ch : text) {
        switch (ch) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            case '\'':
                out += "&apos;";
                break;
            default:
                out += ch;
                break;
        }
    }
    return out;
}

std::vector<unsigned char> buildXmp(const std::string& description) {
    static constexpr char kXmpId[] = "http://ns.adobe.com/xap/1.0/";
    const std::string escaped = xmlEscape(description);
    const std::string xml =
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">"
        "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
        "<rdf:Description xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
        "<dc:description><rdf:Alt><rdf:li xml:lang=\"x-default\">" +
        escaped +
        "</rdf:li></rdf:Alt></dc:description>"
        "</rdf:Description></rdf:RDF></x:xmpmeta>";
    std::vector<unsigned char> payload;
    payload.insert(payload.end(), kXmpId, kXmpId + sizeof(kXmpId));  // includes required NUL
    payload.insert(payload.end(), xml.begin(), xml.end());
    return payload;
}

// Sub-items of Demosaic / Render setup (already counted there): how much of
// each was engine building (pipeline compile + allocation) vs GPU work.
std::string setupBreakdown(const JpegCaptureContext& c) {
    if (!(c.demosaicSetupMs > 0.0) && !(c.renderEngineBuildMs > 0.0)) return {};
    std::ostringstream out;
    out << "- Engine build (in stages above): demosaic " << formatMs1(c.demosaicSetupMs) << " ms, render "
        << formatMs1(c.renderEngineBuildMs) << " ms\n";
    return out.str();
}

std::string appendProcessingTimes(const JpegCaptureContext& c, double jpegEncodingMs, double gainmapEncodeMs = 0.0,
                                  bool ultraHdr = false) {
    std::ostringstream out;
    out << c.imageDescription;
    // Same rule as formatImageProcessing: the tail counts only when its line prints.
    const double tailMs = c.highlightStage >= 0 && *postTailLabel(c.highlightStage, c.defringeEnabled)
                              ? std::max(c.postTailMs, 0.0)
                              : 0.0;
    // UltraHDR mux runs after EXIF assembly, like file write + fsync: measured
    // into done.encodeMs (logcat-exact) but excluded from the printed total.
    const std::string note = ultraHdr ? " ms (sum of stages below; mux + final sync in device log)\n"
                                      : " ms (sum of stages below; final sync in device log)\n";
    if (!c.hasMultiframeTimings) {
        // Single-frame: same unified accounting, no multiframe/output lines.
        // Replaces the legacy "Processing Time" block so every file's total
        // equals the sum of its printed stages.
        const double imageTotal = imageProcessingTotalMs(
            0.0, 0.0, c.demosaicMs, c.renderSetupMs, c.highlightReconstructionMs, c.refinementMs, c.colorProcessingMs,
            c.tonemapMs, c.queueGapsMs, c.readbackMs, c.gainmapMs, c.denoiseMs, c.galoshYuvMs, tailMs);
        const double total = overallTotalMs(0.0, imageTotal, jpegEncodingMs);
        out << "\nTIMINGS: " << formatMs1(total) << note
            << formatImageProcessing(0.0, 0.0, false, c.demosaicMs, 0.0, c.renderSetupMs, c.highlightReconstructionMs,
                                     c.appliedLensShadingCorrection, c.appliedFccSteps, c.refinementMs,
                                     c.colorProcessingMs, c.tonemapMs, c.filmRendered, c.queueGapsMs, c.readbackMs,
                                     false, c.gainmapMs, c.denoiseMs, c.galoshYuvMs, c.highlightStage, c.postTailMs,
                                     c.defringeEnabled)
            << setupBreakdown(c) << formatJpegEncoding(jpegEncodingMs);
        if (ultraHdr) {
            out << "- UltraHDR gain map JPEG: " << formatMs1(gainmapEncodeMs) << " ms (included in JPEG encoding)\n";
        }
        return out.str();
    }
    {
        // Multiframe path (unchanged): preformatted multiframe block plus the
        // full image-processing section and top-level encode line.
        // Unified TIMINGS section (multiframe path only): TOTAL is the exact
        // sum of the printed stages. File write + fsync complete after EXIF
        // assembly and stay logcat/trace-only.
        const double demosaicOrPrep = c.mfDirectRgb ? c.mfRgbPrepMs : c.demosaicMs;
        const double imageTotal =
            imageProcessingTotalMs(c.mfBaseReadMs, c.mfProjectMs, demosaicOrPrep, c.renderSetupMs,
                                   c.highlightReconstructionMs, c.refinementMs, c.colorProcessingMs, c.tonemapMs,
                                   c.queueGapsMs, c.readbackMs, c.gainmapMs, c.denoiseMs, c.galoshYuvMs, tailMs);
        const double total = overallTotalMs(c.multiframeTotalMs, imageTotal, jpegEncodingMs);
        out << "\nTIMINGS: " << formatMs1(total) << note << c.multiframeTimingsBlock
            << formatImageProcessing(c.mfBaseReadMs, c.mfProjectMs, c.mfDirectRgb, c.demosaicMs, c.mfRgbPrepMs,
                                     c.renderSetupMs, c.highlightReconstructionMs, c.appliedLensShadingCorrection,
                                     c.appliedFccSteps, c.refinementMs, c.colorProcessingMs, c.tonemapMs,
                                     c.filmRendered, c.queueGapsMs, c.readbackMs, true, c.gainmapMs, c.denoiseMs,
                                     c.galoshYuvMs, c.highlightStage, c.postTailMs, c.defringeEnabled)
            << setupBreakdown(c) << formatJpegEncoding(jpegEncodingMs);
        if (ultraHdr) {
            out << "- UltraHDR gain map JPEG: " << formatMs1(gainmapEncodeMs) << " ms (included in JPEG encoding)\n";
        }
        return out.str();
    }
}

int tjSubsampling(ChromaSubsampling s) {
    switch (s) {
        case ChromaSubsampling::Yuv444:
            return TJSAMP_444;
        case ChromaSubsampling::Yuv422:
            return TJSAMP_422;
        case ChromaSubsampling::Yuv420:
            return TJSAMP_420;
    }
    return TJSAMP_420;
}

}  // namespace

JpegCaptureWriter::~JpegCaptureWriter() { reset(); }

namespace {
// Pure byte mux (MPF + GContainer/hdrgm XMP + ISO 21496-1); ported constants
// live in gainmap::UltrahdrMux.
 bool assembleUltraHdr(const unsigned char* baseJpeg, std::size_t baseSize, const unsigned char* mapJpeg,
                       std::size_t mapSize, const std::vector<unsigned char>& exif, const JpegCaptureContext& context,
                       std::vector<unsigned char>& out, std::string& error) {
     gainmap::UltrahdrMuxParams params;
     params.gainMapMinLog2 = context.ultraHdr.gainMapMinLog2;
     params.gainMapMaxLog2 = context.ultraHdr.gainMapMaxLog2;
     params.gamma = context.ultraHdr.gamma;
     params.offsetSdr = context.ultraHdr.offsetSdr;
     params.offsetHdr = context.ultraHdr.offsetHdr;
     params.hdrCapacityMinLog2 = context.ultraHdr.hdrCapacityMinLog2;
     params.hdrCapacityMaxLog2 = context.ultraHdr.hdrCapacityMaxLog2;
     params.multiChannel = context.ultraHdr.multiChannelMap;
    gainmap::UltrahdrMuxResult result = gainmap::UltrahdrMux::assemble(
        baseJpeg, baseSize, mapJpeg, mapSize, exif.empty() ? nullptr : exif.data(), exif.size(), params, out);
    if (!result.ok) {
        error = result.error.empty() ? "ultrahdr_mux_failed" : result.error;
        return false;
    }
    return true;
}

// Legacy SDR fallback when the UltraHDR assembly fails (gain map encode or
// mux error, oversize metadata). Writes the already-encoded base JPEG with
// EXIF + XMP so the shot survives as SDR instead of a 0-byte file. Returns
// false only if the fallback itself cannot be written.
bool writeUltraHdrFallback(int fd, const unsigned char* baseJpeg, std::size_t baseSize, JpegCaptureContext& context,
                           std::uint32_t width, std::uint32_t height, double baseMs, std::uint64_t requestId,
                           const std::string& originalError, double& encodeMsOut) {
    // Honest timings: the file is SDR, so the description drops the gainmap
    // stage instead of claiming UltraHDR.
    context.imageDescription = appendProcessingTimes(context, baseMs);
    const auto exif = buildExif(context, width, height);
    const auto xmp = buildXmp(context.imageDescription);
    if (exif.size() + 2u > 65535u || xmp.size() + 2u > 65535u) return false;
    const unsigned char exifHeader[4] = {
        0xFFu,
        0xE1u,
        static_cast<unsigned char>(((exif.size() + 2u) >> 8u) & 0xffu),
        static_cast<unsigned char>((exif.size() + 2u) & 0xffu),
    };
    const unsigned char xmpHeader[4] = {
        0xFFu,
        0xE1u,
        static_cast<unsigned char>(((xmp.size() + 2u) >> 8u) & 0xffu),
        static_cast<unsigned char>((xmp.size() + 2u) & 0xffu),
    };
    if (!writeAll(fd, baseJpeg, 2u) || !writeAll(fd, exifHeader, sizeof(exifHeader)) ||
        !writeAll(fd, exif.data(), exif.size()) || !writeAll(fd, xmpHeader, sizeof(xmpHeader)) ||
        !writeAll(fd, xmp.data(), xmp.size()) || !writeAll(fd, baseJpeg + 2u, baseSize - 2u)) {
        return false;
    }
    encodeMsOut = baseMs;
    __android_log_print(ANDROID_LOG_WARN, "RawrCamNative",
                        "JPEG_UHDR_FALLBACK requestId=%llu original_error=%s fallback=legacy_sdr",
                        (unsigned long long)requestId, originalError.c_str());
    return true;
}
}  // namespace

bool JpegCaptureWriter::busy() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return busy_;
}

bool JpegCaptureWriter::start(std::uint64_t requestId, const void* rgba8, std::size_t rgbaBytes, std::uint32_t width,
                              std::uint32_t height, JpegCaptureContext context) {
    if (requestId == 0 || !rgba8 || width == 0 || height == 0 || context.outputFd < 0 || context.quality < 95 ||
        context.quality > 100) {
        __android_log_print(ANDROID_LOG_WARN, "RawrCamNative", "JPEG_WRITER_REJECTED requestId=%llu invalid_args",
                            (unsigned long long)requestId);
        if (context.outputFd >= 0) ::close(context.outputFd);
        return false;
    }
    const std::uint64_t required = static_cast<std::uint64_t>(width) * height * 4u;
    if (required > std::numeric_limits<std::size_t>::max() || rgbaBytes != static_cast<std::size_t>(required)) {
        __android_log_print(ANDROID_LOG_WARN, "RawrCamNative", "JPEG_WRITER_REJECTED requestId=%llu size_mismatch",
                            (unsigned long long)requestId);
        ::close(context.outputFd);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (busy_ || completion_) {
            __android_log_print(ANDROID_LOG_WARN, "RawrCamNative", "JPEG_WRITER_REJECTED requestId=%llu busy=%d",
                                (unsigned long long)requestId, busy_ ? 1 : 0);
            ::close(context.outputFd);
            return false;
        }
        busy_ = true;
    }
    joinWorker();
    const auto* pixels = static_cast<const unsigned char*>(rgba8);
    const int ownedFd = context.outputFd;
    try {
        worker_ = std::thread([this, requestId, pixels, width, height, context = std::move(context)]() mutable {
            // Background priority: file encoding must never preempt camera or
            // preview threads, especially on throttled silicon.
            ::setpriority(PRIO_PROCESS, 0, 10);
            JpegWriteCompletion done{};
            done.requestId = requestId;
            done.displayName = context.displayName;
            done.filmFallbackMemory = context.filmFallbackMemory;
            const int fd = context.outputFd;
            diagnostics::RuntimeTraceRecorder::instance().record(diagnostics::RuntimeTraceStage::JpegEncodeBegin, 0,
                                                                 requestId);
            const auto t0 = std::chrono::steady_clock::now();
            tjhandle handle = tj3Init(TJINIT_COMPRESS);
            unsigned char* jpegBuf = nullptr;
            std::size_t jpegSize = 0;
            try {
                if (!handle) {
                    done.error = "tj3Init failed";
                } else if (tj3Set(handle, TJPARAM_QUALITY, context.quality) != 0 ||
                           tj3Set(handle, TJPARAM_SUBSAMP, tjSubsampling(context.subsampling)) != 0 ||
                           // Explicitly select libjpeg-turbo's accurate integer DCT (ISLOW-equivalent).
                           // This is already the TurboJPEG default, but setting FASTDCT=0 makes the
                           // production contract and device diagnostics independent of library defaults.
                           tj3Set(handle, TJPARAM_FASTDCT, 0) != 0) {
                    done.error = tj3GetErrorStr(handle);
                } else if (tj3Compress8(handle, pixels, static_cast<int>(width), 0, static_cast<int>(height), TJPF_RGBA,
                                        &jpegBuf, &jpegSize) != 0) {
                    done.error = tj3GetErrorStr(handle);
                } else if (jpegSize < 2 || jpegBuf[0] != 0xFFu || jpegBuf[1] != 0xD8u) {
                    done.error = "libjpeg-turbo returned invalid JPEG SOI";
                } else {
                    const auto encodedAt = std::chrono::steady_clock::now();
                    done.encodeMs = std::chrono::duration<double, std::milli>(encodedAt - t0).count();
                    context.imageDescription = appendProcessingTimes(context, done.encodeMs);
                    const auto exif = buildExif(context, width, height);
                    const auto xmp = buildXmp(context.imageDescription);
                    if (exif.size() + 2u > 65535u || xmp.size() + 2u > 65535u) {
                        done.error = "metadata APP1 payload too large";
                    } else {
                        const unsigned char exifHeader[4] = {
                            0xFFu,
                            0xE1u,
                            static_cast<unsigned char>(((exif.size() + 2u) >> 8u) & 0xffu),
                            static_cast<unsigned char>((exif.size() + 2u) & 0xffu),
                        };
                        const unsigned char xmpHeader[4] = {
                            0xFFu,
                            0xE1u,
                            static_cast<unsigned char>(((xmp.size() + 2u) >> 8u) & 0xffu),
                            static_cast<unsigned char>((xmp.size() + 2u) & 0xffu),
                        };
                        if (!writeAll(fd, jpegBuf, 2u) || !writeAll(fd, exifHeader, sizeof(exifHeader)) ||
                            !writeAll(fd, exif.data(), exif.size()) || !writeAll(fd, xmpHeader, sizeof(xmpHeader)) ||
                            !writeAll(fd, xmp.data(), xmp.size()) || !writeAll(fd, jpegBuf + 2u, jpegSize - 2u)) {
                            done.error = "JPEG write failed: " + std::string(std::strerror(errno));
                        }
                    }
                }
            } catch (const std::exception& error) {
                done.error = error.what();
            } catch (...) {
                done.error = "jpeg_encoding_failed";
            }
            if (jpegBuf) tj3Free(jpegBuf);
            if (handle) tj3Destroy(handle);
            if (done.error.empty()) {
                const auto fs0 = std::chrono::steady_clock::now();
                if (::fsync(fd) != 0) done.error = "fsync failed: " + std::string(std::strerror(errno));
                const auto fs1 = std::chrono::steady_clock::now();
                done.fsyncMs = std::chrono::duration<double, std::milli>(fs1 - fs0).count();
            }
            struct stat st{};
            if (done.error.empty() && ::fstat(fd, &st) == 0 && st.st_size >= 0)
                done.fileBytes = static_cast<std::uint64_t>(st.st_size);
            if (::close(fd) != 0 && done.error.empty())
                done.error = "close failed: " + std::string(std::strerror(errno));
            done.success = done.error.empty();
            if (done.success) {
                __android_log_print(ANDROID_LOG_INFO, "RawrCamNative",
                                    "JPEG_WRITE_DONE requestId=%llu bytes=%llu encodeMs=%.1f fsyncMs=%.1f",
                                    (unsigned long long)done.requestId, (unsigned long long)done.fileBytes,
                                    done.encodeMs, done.fsyncMs);
            } else {
                __android_log_print(ANDROID_LOG_ERROR, "RawrCamNative", "JPEG_WRITE_FAIL requestId=%llu error=%s",
                                    (unsigned long long)done.requestId, done.error.c_str());
            }
            diagnostics::RuntimeTraceRecorder::instance().record(diagnostics::RuntimeTraceStage::JpegEncodeEnd, 0,
                                                                 requestId, -1, 0, 0, done.success ? 1u : 0u);
            std::lock_guard<std::mutex> lock(mutex_);
            completion_ = std::move(done);
            busy_ = false;
        });
    } catch (...) {
        ::close(ownedFd);
        std::lock_guard<std::mutex> lock(mutex_);
        busy_ = false;
        return false;
    }
    return true;
}

bool JpegCaptureWriter::startUltraHdr(std::uint64_t requestId, const void* rgba8, std::size_t rgbaBytes,
                                      std::uint32_t width, std::uint32_t height, const void* mapRgba8,
                                      std::size_t mapBytes, std::uint32_t mapWidth, std::uint32_t mapHeight,
                                      JpegCaptureContext context) {
    if (!context.ultraHdr.enabled) {
        return start(requestId, rgba8, rgbaBytes, width, height, std::move(context));
    }
    if (requestId == 0 || !rgba8 || width == 0 || height == 0 || !mapRgba8 || mapWidth == 0 || mapHeight == 0 ||
        context.outputFd < 0 || context.quality < 95 || context.quality > 100) {
        __android_log_print(ANDROID_LOG_WARN, "RawrCamNative", "JPEG_UHDR_REJECTED requestId=%llu invalid_args",
                            (unsigned long long)requestId);
        if (context.outputFd >= 0) ::close(context.outputFd);
        return false;
    }
    const std::uint64_t required = static_cast<std::uint64_t>(width) * height * 4u;
    const std::uint64_t mapRequired = static_cast<std::uint64_t>(mapWidth) * mapHeight * 4u;
    if (required > std::numeric_limits<std::size_t>::max() || rgbaBytes != static_cast<std::size_t>(required) ||
        mapRequired > std::numeric_limits<std::size_t>::max() / 2u ||
        (mapBytes != static_cast<std::size_t>(mapRequired) && mapBytes != static_cast<std::size_t>(mapRequired * 2u))) {
        __android_log_print(ANDROID_LOG_WARN, "RawrCamNative", "JPEG_UHDR_REJECTED requestId=%llu size_mismatch",
                            (unsigned long long)requestId);
        ::close(context.outputFd);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (busy_ || completion_) {
            __android_log_print(ANDROID_LOG_WARN, "RawrCamNative", "JPEG_UHDR_REJECTED requestId=%llu busy=%d",
                                (unsigned long long)requestId, busy_ ? 1 : 0);
            ::close(context.outputFd);
            return false;
        }
        busy_ = true;
    }
    joinWorker();
    const auto* pixels = static_cast<const unsigned char*>(rgba8);
    const auto* mapPixels = static_cast<const unsigned char*>(mapRgba8);
    const int ownedFd = context.outputFd;
    const bool hasCaptureColor = mapBytes == mapRequired * 2u;
    const int mapQuality = hasCaptureColor ? 95 : std::clamp(context.ultraHdr.gainmapQuality, 1, 100);
    try {
        worker_ = std::thread([this, requestId, pixels, width, height, mapPixels, mapWidth, mapHeight, mapQuality,
                               hasCaptureColor, context = std::move(context)]() mutable {
            ::setpriority(PRIO_PROCESS, 0, 10);
            JpegWriteCompletion done{};
            done.requestId = requestId;
            done.displayName = context.displayName;
            done.filmFallbackMemory = context.filmFallbackMemory;
            const int fd = context.outputFd;
            diagnostics::RuntimeTraceRecorder::instance().record(diagnostics::RuntimeTraceStage::JpegEncodeBegin, 0,
                                                                 requestId);
            const auto t0 = std::chrono::steady_clock::now();
            tjhandle baseHandle = tj3Init(TJINIT_COMPRESS);
            tjhandle mapHandle = tj3Init(TJINIT_COMPRESS);
            unsigned char* baseBuf = nullptr;
            std::size_t baseSize = 0;
            unsigned char* mapBuf = nullptr;
            std::size_t mapSize = 0;
            double baseMs = 0.0;
            double mapMs = 0.0;
            try {
                if (!baseHandle || !mapHandle) {
                    done.error = "tj3Init failed";
                } else if (tj3Set(baseHandle, TJPARAM_QUALITY, context.quality) != 0 ||
                           tj3Set(baseHandle, TJPARAM_SUBSAMP, tjSubsampling(context.subsampling)) != 0 ||
                           tj3Set(baseHandle, TJPARAM_FASTDCT, 0) != 0) {
                    done.error = tj3GetErrorStr(baseHandle);
                 } else if (tj3Set(mapHandle, TJPARAM_QUALITY, mapQuality) != 0 ||
                            // Gain map is multi-channel RGB: 4:4:4 keeps
                            // chroma full-res so subsample ringing can't
                            // print a border along boost shoulders. The
                            // writer fits the ceiling + shoulder + chroma
                            // knee on a copy just above (fitAndRemapMap).
                            tj3Set(mapHandle, TJPARAM_SUBSAMP, TJSAMP_444) != 0 ||
                           tj3Set(mapHandle, TJPARAM_FASTDCT, 0) != 0) {
                    done.error = tj3GetErrorStr(mapHandle);
                } else {
                     // The base encode runs beside the fit + map encode: both
                     // only read `pixels`, so the file is byte-identical to
                     // the serial order and the critical path drops by the
                     // base encode.
                     std::string baseError;
                     std::thread baseWorker([&] {
                         ::setpriority(PRIO_PROCESS, 0, 10);
                         if (tj3Compress8(baseHandle, pixels, static_cast<int>(width), 0, static_cast<int>(height),
                                          TJPF_RGBA, &baseBuf, &baseSize) != 0) {
                             baseError = tj3GetErrorStr(baseHandle);
                         } else if (baseSize < 2 || baseBuf[0] != 0xFFu || baseBuf[1] != 0xD8u) {
                             baseError = "libjpeg-turbo returned invalid base JPEG SOI";
                         }
                         baseMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                                      .count();
                     });
                     struct JoinOnExit {
                         std::thread& thread;
                         ~JoinOnExit() {
                             if (thread.joinable()) thread.join();
                         }
                     } joinBase{baseWorker};
                     std::string uhError;
                     // Single-pass adaptive ceiling: fit + shoulder + chroma
                     // knee on a worker-local copy (caller's readback stays
                     // untouched for telemetry). Metadata follows the fit.
                     std::vector<unsigned char> fittedMap;
                     float fittedMax = context.ultraHdr.gainMapMaxLog2;
                     const unsigned char* mapEncodePixels = mapPixels;
                     const auto fitT0 = std::chrono::steady_clock::now();
                     const bool didFit =
                         fitAndRemapMap(mapPixels, mapWidth, mapHeight, context.ultraHdr, fittedMap, fittedMax,
                                        pixels, width, height);
                     bool colorRestored = false;
                     if (didFit) {
                         mapEncodePixels = fittedMap.data();
                         context.ultraHdr.gainMapMaxLog2 = fittedMax;
                         context.ultraHdr.hdrCapacityMaxLog2 = fittedMax;
                         if (hasCaptureColor) {
                             const auto* chromaticity = mapPixels + size_t(mapWidth) * mapHeight * 4;
                             colorRestored = restoreHighlightColor(
                                 fittedMap, chromaticity, mapWidth, mapHeight, pixels, width, height,
                                 context.ultraHdr, fittedMax, context.ultraHdr.gainMapMinLog2,
                                 context.ultraHdr.gainMapMaxLog2);
                         }
                     }
                     const auto fitT1 = std::chrono::steady_clock::now();
                     __android_log_print(ANDROID_LOG_INFO, "RawrCamNative",
                                         "JPEG_UHDR_FIT requestId=%llu fit=%d max=%.2f base=%.2f/%.2f fitMs=%.1f mapPx=%ux%u color=%d range=%.2f/%.2f",
                                         (unsigned long long)requestId, didFit ? 1 : 0, fittedMax,
                                         context.ultraHdr.baseShare, context.ultraHdr.baseExponent,
                                         std::chrono::duration<double, std::milli>(fitT1 - fitT0).count(), mapWidth,
                                         mapHeight, colorRestored ? 1 : 0, context.ultraHdr.gainMapMinLog2,
                                         context.ultraHdr.gainMapMaxLog2);
                     if (tj3Compress8(mapHandle, mapEncodePixels, static_cast<int>(mapWidth), 0,
                                      static_cast<int>(mapHeight), TJPF_RGBA, &mapBuf, &mapSize) != 0) {
                        uhError = tj3GetErrorStr(mapHandle);
                    } else if (mapSize < 2 || mapBuf[0] != 0xFFu || mapBuf[1] != 0xD8u) {
                        uhError = "libjpeg-turbo returned invalid gain map JPEG SOI";
                    }
                    mapMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - fitT0).count();
                    baseWorker.join();
                    const double bothMs =
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                    __android_log_print(ANDROID_LOG_INFO, "RawrCamNative",
                                        "JPEG_UHDR_ENCODE requestId=%llu baseMs=%.1f mapPathMs=%.1f wallMs=%.1f",
                                        (unsigned long long)requestId, baseMs, mapMs, bothMs);
                    if (!baseError.empty()) {
                        done.error = baseError;
                    } else if (uhError.empty()) {
                        // Printed JPEG number is the wall time of both turbo
                        // encodes (they overlap); the mux below folds into
                        // done.encodeMs (logcat-exact) and is excluded from
                        // the printed total by contract.
                        context.imageDescription = appendProcessingTimes(context, bothMs, mapMs, true);
                        const auto exif = buildExif(context, width, height);
                        if (exif.size() + 2u > 65535u) {
                            uhError = "metadata APP1 payload too large";
                        } else {
                            std::vector<unsigned char> file;
                            std::string muxError;
                            if (!assembleUltraHdr(baseBuf, baseSize, mapBuf, mapSize, exif, context, file, muxError)) {
                                uhError = muxError;
                            } else {
                                const auto encodedAt = std::chrono::steady_clock::now();
                                done.encodeMs = std::chrono::duration<double, std::milli>(encodedAt - t0).count();
                                if (!writeAll(fd, file.data(), file.size())) {
                                    uhError = "JPEG write failed: " + std::string(std::strerror(errno));
                                }
                            }
                        }
                    }
                    if (baseError.empty() && !uhError.empty()) {
                        // Degrade to SDR instead of losing the shot: the base
                        // pixels are already encoded, so splice EXIF + XMP
                        // around them exactly like the legacy path.
                        double fallbackEncodeMs = 0.0;
                        if (writeUltraHdrFallback(fd, baseBuf, baseSize, context, width, height, baseMs, requestId,
                                                  uhError, fallbackEncodeMs)) {
                            done.encodeMs = fallbackEncodeMs;
                        } else {
                            done.error = uhError;
                        }
                    }
                }
            } catch (const std::exception& error) {
                done.error = error.what();
            } catch (...) {
                done.error = "jpeg_encoding_failed";
            }
            if (baseBuf) tj3Free(baseBuf);
            if (mapBuf) tj3Free(mapBuf);
            if (baseHandle) tj3Destroy(baseHandle);
            if (mapHandle) tj3Destroy(mapHandle);
            if (done.error.empty()) {
                const auto fs0 = std::chrono::steady_clock::now();
                if (::fsync(fd) != 0) done.error = "fsync failed: " + std::string(std::strerror(errno));
                const auto fs1 = std::chrono::steady_clock::now();
                done.fsyncMs = std::chrono::duration<double, std::milli>(fs1 - fs0).count();
            }
            struct stat st{};
            if (done.error.empty() && ::fstat(fd, &st) == 0 && st.st_size >= 0)
                done.fileBytes = static_cast<std::uint64_t>(st.st_size);
            if (::close(fd) != 0 && done.error.empty())
                done.error = "close failed: " + std::string(std::strerror(errno));
            done.success = done.error.empty();
            if (done.success) {
                __android_log_print(ANDROID_LOG_INFO, "RawrCamNative",
                                    "JPEG_UHDR_DONE requestId=%llu bytes=%llu encodeMs=%.1f fsyncMs=%.1f",
                                    (unsigned long long)done.requestId, (unsigned long long)done.fileBytes,
                                    done.encodeMs, done.fsyncMs);
            } else {
                __android_log_print(ANDROID_LOG_ERROR, "RawrCamNative", "JPEG_UHDR_FAIL requestId=%llu error=%s",
                                    (unsigned long long)done.requestId, done.error.c_str());
            }
            diagnostics::RuntimeTraceRecorder::instance().record(diagnostics::RuntimeTraceStage::JpegEncodeEnd, 0,
                                                                 requestId, -1, 0, 0, done.success ? 1u : 0u);
            std::lock_guard<std::mutex> lock(mutex_);
            completion_ = std::move(done);
            busy_ = false;
        });
    } catch (...) {
        ::close(ownedFd);
        std::lock_guard<std::mutex> lock(mutex_);
        busy_ = false;
        return false;
    }
    return true;
}

std::optional<JpegWriteCompletion> JpegCaptureWriter::pollCompletion() {
    std::optional<JpegWriteCompletion> out;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!completion_) return std::nullopt;
        out = std::move(completion_);
        completion_.reset();
    }
    joinWorker();
    return out;
}

void JpegCaptureWriter::reset() noexcept {
    joinWorker();
    std::lock_guard<std::mutex> lock(mutex_);
    busy_ = false;
    completion_.reset();
}

void JpegCaptureWriter::joinWorker() {
    if (worker_.joinable()) worker_.join();
}

}  // namespace rawrcam::encoding::jpeg
