#include "capture/persistence/CaptureJob.h"

#include <fcntl.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <type_traits>

#include "capture/CaptureRequest.h"

namespace rawrcam::capture::persistence {
namespace {
constexpr uint64_t legacyMagic = 0x524157524a4f4201ull;
constexpr uint64_t magicV2 = 0x524157524a4f4202ull;
constexpr uint64_t magicV3 = 0x524157524a4f4203ull;
// v4 appended the wavelet luma force + band count to the JPEG tail.
constexpr uint64_t magicV4 = 0x524157524a4f4204ull;
// v5 drops the per-capture noise-model selection (multiframe always fits
// noise from the burst) and carries the fallback-cleanup tuning. Writes are
// v5; v4 and older load through their frozen head layout (readLegacyJob).
constexpr uint64_t magicV5 = 0x524157524a4f4205ull;
// v6 appends the UltraHDR intent and the highlight method/threshold/
// compression to the JPEG tail (v5 and older reloaded them as defaults, so
// queued multiframe JPEGs lost UltraHDR and the chosen HL method).
constexpr uint64_t magicV6 = 0x524157524a4f4206ull;
// v7 appends the multiframe chroma-denoise flag to the JPEG tail.
constexpr uint64_t magicV7 = 0x524157524a4f4207ull;
// v8 appends the base/merged DNG compression (v7 and older reloaded them as
// the lossless default, so "Uncompressed" was ignored for multiframe DNGs).
constexpr uint64_t magic = 0x524157524a4f4208ull;
constexpr uint64_t footer = 0x434f4d4d49545445ull;
constexpr uint64_t MiB = 1024ull * 1024;
std::mutex budgetMutex;
uint64_t reservedDisk = 0, reservedCpu = 0;
struct Reservation {
    uint64_t disk, cpu;
    ~Reservation() {
        std::lock_guard<std::mutex> lock(budgetMutex);
        reservedDisk -= disk;
        reservedCpu -= cpu;
    }
};
void syncFile(const std::string& path, bool directory = false) {
    int fd = open(path.c_str(), O_RDONLY | (directory ? O_DIRECTORY : 0));
    if (fd < 0) throw std::runtime_error("capture_job_sync_open_failed");
    int result = fsync(fd);
    close(fd);
    if (result != 0) throw std::runtime_error("capture_job_sync_failed");
}
template <class A>
void fields(A& a, metadata::CameraContextMetadata& v) {
    a(v.cameraContextGeneration, v.cameraId, v.lensId, v.geometry, v.rawPreviewCfa, v.camera2Cfa, v.lensFacing,
      v.baselineBlackLevelPhysicalRggb, v.baselineWhiteLevel, v.color, v.sensorPhysicalSizeMm,
      v.availableFocalLengthsMm, v.availableApertures, v.lensShadingMapWidth, v.lensShadingMapHeight,
      v.maxPostRawSensitivityBoost, v.hasLensShadingApplied, v.lensShadingApplied, v.maxAnalogSensitivity,
      v.hasGreenSplit, v.greenSplit, v.opticalBlackRegions, v.lensDistortion, v.lensIntrinsicCalibration,
      v.hasFlashInfoAvailable, v.flashInfoAvailable);
}
template <class A>
void fields(A& a, metadata::FrameMetadataSnapshot& v) {
    a(v.cameraContext, v.frameOrdinal, v.timestampNs, v.blackLevelPhysicalRggb, v.staticWhiteLevel,
      v.reportedDynamicWhiteLevel, v.effectiveWhiteLevel, v.effectiveWhiteLevelSource, v.forcedSensorModeId,
      v.colorCorrectionGainsRggb, v.neutralColorPoint, v.hasNeutralColorPoint, v.colorCorrectionTransform,
      v.colorCorrectionMode, v.awbMode, v.aeMode, v.aeState, v.awbState, v.postRawSensitivityBoost, v.scalerCropRegion,
      v.rawCropRegion, v.exposureTimeNs, v.sensitivity, v.requestedExposureTimeNs, v.requestedSensitivity,
      v.optimizedStillRequestId, v.aperture, v.focalLengthMm, v.focusDistanceDiopters, v.flashState, v.hotPixelMap,
      v.sensorNoiseProfile, v.lensShadingMapWidth, v.lensShadingMapHeight, v.lensShadingMap);
}
template <class A>
void fields(A& a, color::FrameColorTransform& v) {
    a(v.baselineWbRggb, v.cameraToLinearSrgbRowMajor, v.source, v.baselineWbAppliedByRawPreview, v.hasEstimatedWhite,
      v.estimatedWhiteX, v.estimatedWhiteY, v.estimatedCctKelvin, v.calibrationWeight1);
}
template <class A>
void fields(A& a, rawrcam::imaging::RawSnapshot& v) {
    a(v.requestId, v.timestampNs, v.width, v.height, v.packedRowStrideBytes, v.sourceRowStrideBytes,
      v.sourcePixelStrideBytes, v.metadata, v.colorState);
}
template <class A>
void fields(A& a, encoding::dng::DngCaptureContext& v) {
    a(v.deviceRotationDegrees, v.wallClockUnixMillis, v.utcOffsetMinutes, v.deviceMake, v.deviceModel, v.displayName,
      v.imageDescription, v.baselineExposureEV, v.reconstructedGeometry);
}
template <class A>
void fields(A& a, rawrcam::capture::JpegCaptureRequest& v) {
    a(v.output.deviceRotationDegrees, v.output.wallClockUnixMillis, v.output.utcOffsetMinutes, v.output.deviceMake,
      v.output.deviceModel, v.output.displayName, v.output.quality, v.output.subsampling,
      v.develop.pipelineDiagnosticsEnabled, v.develop.colorRenderProfile, v.develop.importedLutProfileId,
      v.output.rendererDisplayName, v.develop.demosaicAlgorithm, v.develop.dualAutoContrast,
      v.develop.dualContrastPercent, v.develop.fccSteps, v.develop.lensShadingCorrectionEnabled,
      v.develop.highlightReconstructionEnabled, v.develop.distortionCorrectionEnabled, v.output.exifOrientation,
      v.output.exposureTimeNs, v.output.sensitivity, v.output.aperture, v.output.focalLengthMm,
      v.output.imageDescription, v.output.filmDescription, v.output.demosaicMs, v.output.colorProcessingMs,
      v.output.highlightReconstructionMs, v.output.refinementMs, v.output.tonemapMs, v.output.renderTotalMs,
      v.output.filmRendered, v.output.renderSetupMs, v.output.queueGapsMs, v.output.readbackMs);
}
template <class A>
void fields(A& a, CaptureJob& v) {
    a(v.frame, v.dng, v.mergedDng, v.jpeg, v.tone, v.gain, v.filmEnabled, v.jpegRequested, v.multiframe, v.film,
      v.tuning, v.baseFrameMode, v.sharpnessScores, v.sharpnessMs, v.referenceIndex, v.metadata, v.parameters);
}
// Private same-schema archive. Size prefixes on PODs detect incompatible ABI/layout
// changes; incompatible files are retained, never interpreted as current data.
struct Writer {
    std::ostream& stream;
    template <class... T>
    void operator()(T&... v) {
        (one(v), ...);
    }
    void bytes(const void* p, size_t n) {
        stream.write(static_cast<const char*>(p), n);
        if (!stream) throw std::runtime_error("capture_job_write_failed");
    }
    template <class T>
    void one(T& v) {
        if constexpr (std::is_trivially_copyable_v<T>) {
            uint32_t n = sizeof(T);
            bytes(&n, sizeof(n));
            bytes(&v, n);
        } else
            fields(*this, v);
    }
    void one(std::string& v) {
        uint64_t n = v.size();
        one(n);
        bytes(v.data(), n);
    }
    template <class T>
    void one(std::vector<T>& v) {
        uint64_t n = v.size();
        one(n);
        for (auto& x : v) one(x);
    }
    template <class T>
    void one(std::optional<T>& v) {
        bool present = v.has_value();
        one(present);
        if (present) one(*v);
    }
    void one(metadata::CameraContextMetadataPtr& v) {
        bool present = bool(v);
        one(present);
        if (present) {
            auto copy = *v;
            fields(*this, copy);
        }
    }
};
struct Reader {
    std::istream& stream;
    template <class... T>
    void operator()(T&... v) {
        (one(v), ...);
    }
    void bytes(void* p, size_t n) {
        stream.read(static_cast<char*>(p), n);
        if (!stream) throw std::runtime_error("capture_job_truncated");
    }
    template <class T>
    void one(T& v) {
        if constexpr (std::is_trivially_copyable_v<T>) {
            uint32_t n = 0;
            bytes(&n, sizeof(n));
            if (n != sizeof(T)) throw std::runtime_error("capture_job_schema_mismatch offset=" + std::to_string(static_cast<long long>(stream.tellg())) + " stored=" + std::to_string(n) + " expected=" + std::to_string(sizeof(T)));
            bytes(&v, n);
        } else
            fields(*this, v);
    }
    // Render profiles appended three words to TonemapParams without changing the
    // journal version. Accept the two known layouts; never read a short POD into
    // the current struct or discard newly saved profile selection.
    void one(tonemap::TonemapParams& v) {
        uint32_t n = 0;
        bytes(&n, sizeof(n));
        if (n == sizeof(v)) {
            bytes(&v, sizeof(v));
        } else if (n == 11u * sizeof(float)) {
            std::array<float, 11> old{};
            bytes(old.data(), sizeof(old));
            v = {};
            v.exposureEV = old[0];
            v.blackPointEV = old[1];
            v.shadowLiftEV = old[2];
            v.midtoneLiftEV = old[3];
            v.contrast = old[4];
            v.shoulderStartEV = old[5];
            v.whitePointEV = old[6];
            v.highlightBiasEV = old[7];
            v.saturation = old[8];
            v.vibrance = old[9];
            v.aePostGain = old[10];
        } else {
            throw std::runtime_error("capture_job_tonemap_schema_mismatch");
        }
    }
    // Merge algorithm, HDR+ strength, tile size and the bracket fields were
    // appended to MultiframeTuning without a journal bump; older jobs carry a
    // shorter prefix (88 bytes: Wronski only, 96: before the tile size, 100:
    // before the bracket fields).
    void one(multiframe::MultiframeTuning& v) {
        constexpr uint32_t kPrefixBytes = 2u * sizeof(uint32_t) + 20u * sizeof(float);
        static_assert(offsetof(multiframe::MultiframeTuning, mergeAlgorithm) == kPrefixBytes,
                      "MultiframeTuning journal prefix layout");
        uint32_t n = 0;
        bytes(&n, sizeof(n));
        constexpr uint32_t kBeforeTileBytes = kPrefixBytes + 2u * sizeof(uint32_t);
        static_assert(offsetof(multiframe::MultiframeTuning, hdrplusTileSize) == kBeforeTileBytes,
                      "MultiframeTuning journal tile-size layout");
        constexpr uint32_t kBeforeBracketBytes = kBeforeTileBytes + sizeof(uint32_t);
        static_assert(offsetof(multiframe::MultiframeTuning, bracketEv) == kBeforeBracketBytes,
                      "MultiframeTuning journal bracket layout");
        if (n != sizeof(v) && n != kPrefixBytes && n != kBeforeTileBytes && n != kBeforeBracketBytes)
            throw std::runtime_error("capture_job_tuning_schema_mismatch");
        v = {};
        bytes(&v, n);
    }
    void one(std::string& v) {
        uint64_t n = 0;
        one(n);
        if (n > MiB) throw std::runtime_error("capture_job_string_invalid");
        v.resize(n);
        bytes(v.data(), n);
    }
    template <class T>
    void one(std::vector<T>& v) {
        uint64_t n = 0;
        one(n);
        if (n > 65536) throw std::runtime_error("capture_job_vector_invalid");
        v.resize(n);
        for (auto& x : v) one(x);
    }
    template <class T>
    void one(std::optional<T>& v) {
        bool present = false;
        one(present);
        if (present) {
            v.emplace();
            one(*v);
        } else
            v.reset();
    }
    void one(metadata::CameraContextMetadataPtr& v) {
        bool present = false;
        one(present);
        if (present) {
            auto copy = std::make_shared<metadata::CameraContextMetadata>();
            fields(*this, *copy);
            v = copy;
        }
    }
};
uint64_t payloadSize(const CaptureJob& job) {
    const auto& f = job.frame;
    uint64_t n = uint64_t(f.width) * f.height * 2;
    if (!f.width || !f.height || n > 256 * MiB || f.packedRowStrideBytes != f.width * 2 || !f.metadata.cameraContext)
        throw std::runtime_error("capture_job_geometry_invalid");
    if (job.multiframe && (job.metadata.size() < 2 || job.metadata.size() > 30 ||
                           job.parameters.size() != job.metadata.size() || job.referenceIndex >= job.metadata.size()))
        throw std::runtime_error("capture_job_burst_invalid");
    return n;
}
}  // namespace
void markFilmFallback(const std::string& path) {
    const auto marker = path + ".fallback";
    std::ofstream out(marker + ".tmp", std::ios::trunc);
    out << "film_memory\n";
    out.close();
    if (!out) throw std::runtime_error("capture_fallback_commit_failed");
    syncFile(marker + ".tmp");
    std::filesystem::rename(marker + ".tmp", marker);
    syncFile(std::filesystem::path(path).parent_path().string(), true);
}
std::string jobPath(const std::string& filesDir, const std::string& name) {
    if (name.empty() || name.find('/') != std::string::npos || name.find("..") != std::string::npos)
        throw std::runtime_error("capture_job_name_invalid");
    return filesDir + "/still_jobs/" + name + ".job";
}
std::shared_ptr<void> reserve(const std::string& filesDir, uint64_t rawBytes, bool cpuAcquisition) {
    std::lock_guard<std::mutex> lock(budgetMutex);
    struct statvfs disk{};
    const long pages = sysconf(_SC_PHYS_PAGES), pageSize = sysconf(_SC_PAGESIZE);
    if (!rawBytes || rawBytes > 8ull * 1024 * MiB || statvfs(filesDir.c_str(), &disk) != 0) return {};
    uint64_t cpuLimit = 128 * MiB;
    if (pages > 0 && pageSize > 0)
        cpuLimit = std::clamp(uint64_t(pages) * uint64_t(pageSize) / 32, 64 * MiB, 256 * MiB);
    uint64_t cpu = cpuAcquisition ? rawBytes : 0;
    uint64_t storage = rawBytes * 2 + 32 * MiB;
    const uint64_t free = uint64_t(disk.f_bavail) * disk.f_frsize;
    if (cpu + reservedCpu > cpuLimit || free < reservedDisk + storage + 512 * MiB) return {};
    auto token = std::shared_ptr<Reservation>(new Reservation{storage, cpu});
    reservedCpu += cpu;
    reservedDisk += storage;
    return token;
}
// Frozen pre-v5 head layout, read-only: the 20-value tuning, the resolved
// noise selection and the DNG noise-mode byte (both now unused, discarded).
struct LegacyMultiframeTuning {
    float outputScale = 1.0f;
    std::uint32_t lkIterations = 3;
    float hessianEpsilon, kDetail, kDenoise, dThreshold, dTransition, kStretch, kShrink, flatSigma, detailFloorSigma,
        scaleBandwidthGain, coverageNeffLo, coverageNeffHi, coverageMassLo, coverageMassHi, robustnessT, robustnessS1,
        robustnessS2, motionThreshold;
};
static_assert(sizeof(LegacyMultiframeTuning) == 80, "pre-v5 tuning layout");
void readLegacyDng(Reader& a, encoding::dng::DngCaptureContext& v) {
    std::uint8_t noiseProfileMode = 0;
    a(v.deviceRotationDegrees, v.wallClockUnixMillis, v.utcOffsetMinutes, v.deviceMake, v.deviceModel, v.displayName,
      v.imageDescription, v.baselineExposureEV, noiseProfileMode, v.reconstructedGeometry);
}
void readLegacyJob(Reader& a, CaptureJob& v) {
    LegacyMultiframeTuning t{};
    a(v.frame);
    readLegacyDng(a, v.dng);
    readLegacyDng(a, v.mergedDng);
    a(v.jpeg, v.tone, v.gain, v.filmEnabled, v.jpegRequested, v.multiframe, v.film, t, v.baseFrameMode,
      v.sharpnessScores, v.sharpnessMs);
    // v1 originally stored two calibrated profiles and knee values (120 bytes).
    // Later pre-v5 jobs stored one resolved profile (36 bytes). Both are obsolete;
    // consume only these known layouts, leaving the following fields aligned.
    uint32_t noiseBytes = 0;
    a.bytes(&noiseBytes, sizeof(noiseBytes));
    if (noiseBytes != 36 && noiseBytes != 120) throw std::runtime_error("capture_job_noise_schema_mismatch");
    std::array<uint8_t, 120> noise{};
    a.bytes(noise.data(), noiseBytes);
    a(v.referenceIndex, v.metadata, v.parameters);
    auto& n = v.tuning;
    n.outputScale = t.outputScale;
    n.lkIterations = t.lkIterations;
    n.hessianEpsilon = t.hessianEpsilon;
    n.kDetail = t.kDetail;
    n.kDenoise = t.kDenoise;
    n.dThreshold = t.dThreshold;
    n.dTransition = t.dTransition;
    n.kStretch = t.kStretch;
    n.kShrink = t.kShrink;
    n.flatSigma = t.flatSigma;
    n.detailFloorSigma = t.detailFloorSigma;
    n.scaleBandwidthGain = t.scaleBandwidthGain;
    n.coverageNeffLo = t.coverageNeffLo;
    n.coverageNeffHi = t.coverageNeffHi;
    n.coverageMassLo = t.coverageMassLo;
    n.coverageMassHi = t.coverageMassHi;
    n.robustnessT = t.robustnessT;
    n.robustnessS1 = t.robustnessS1;
    n.robustnessS2 = t.robustnessS2;
    n.motionThreshold = t.motionThreshold;
    (void)noise;  // multiframe now always fits noise from the burst
}
void save(const std::string& path, CaptureJob& job, const std::function<std::vector<uint8_t>(size_t)>& readFrame) {
    const uint64_t n = payloadSize(job);
    const auto dir = std::filesystem::path(path).parent_path();
    std::filesystem::create_directories(dir);
    const auto temporary = path + ".tmp";
    try {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        Writer w{stream};
        auto version = magic;
        w(version);
        fields(w, job);
        w(job.dng.processingRecipe, job.dng.resolvedRecipe, job.dng.sourceRole, job.mergedDng.processingRecipe,
          job.mergedDng.resolvedRecipe, job.mergedDng.sourceRole, job.jpeg.develop.fccEdgeSigma,
          job.jpeg.develop.fccChromaBound, job.jpeg.develop.defringeStrength, job.jpeg.develop.defringeEdgeThreshold,
          job.jpeg.develop.defringeLumaFloor, job.jpeg.develop.denoiseStrength, job.jpeg.develop.denoiseDetail,
          job.jpeg.develop.denoiseForceY, job.jpeg.develop.denoiseMaxScale, job.jpeg.develop.galoshRawMode,
          job.jpeg.develop.galoshStrength, job.jpeg.develop.galoshLuma, job.jpeg.develop.galoshChroma,
          job.jpeg.develop.galoshYuvMode, job.jpeg.develop.galoshYuvStrengthY, job.jpeg.develop.galoshYuvStrengthC,
          job.jpeg.output.ultraHdr, job.jpeg.develop.highlightReconstructionMethod, job.jpeg.develop.highlightThreshold,
          job.jpeg.develop.highlightCompression, job.jpeg.develop.multiframeChromaDenoise, job.dng.compression,
          job.mergedDng.compression);
        const size_t count = job.multiframe ? job.metadata.size() : 1;
        for (size_t i = 0; i < count; ++i) {
            if (readFrame) {
                auto raw = readFrame(i);
                if (raw.size() != n) throw std::runtime_error("capture_job_raw_size");
                w.bytes(raw.data(), raw.size());
            } else {
                if (job.frame.raw16.size() != n) throw std::runtime_error("capture_job_raw_size");
                w.bytes(job.frame.raw16.data(), n);
            }
        }
        auto end = footer;
        w(end);
        stream.flush();
        if (!stream) throw std::runtime_error("capture_job_flush_failed");
        stream.close();
        syncFile(temporary);
        std::filesystem::rename(temporary, path);
        syncFile(dir.string(), true);
    } catch (...) {
        std::error_code ec;
        std::filesystem::remove(temporary, ec);
        throw;
    }
}
CaptureJob load(const std::string& path,
                const std::function<void(CaptureJob&, size_t, const std::vector<uint8_t>&)>& consume) {
    std::ifstream stream(path, std::ios::binary);
    Reader r{stream};
    uint64_t version = 0;
    r(version);
    if (version != magic && version != magicV7 && version != magicV6 && version != magicV5 && version != magicV4 &&
        version != magicV3 && version != magicV2 && version != legacyMagic)
        throw std::runtime_error("capture_job_version_unsupported");
    CaptureJob job;
    if (version == magic || version == magicV7 || version == magicV6 || version == magicV5) {
        fields(r, job);
        // Same order as save().
        r(job.dng.processingRecipe, job.dng.resolvedRecipe, job.dng.sourceRole, job.mergedDng.processingRecipe,
          job.mergedDng.resolvedRecipe, job.mergedDng.sourceRole, job.jpeg.develop.fccEdgeSigma,
          job.jpeg.develop.fccChromaBound, job.jpeg.develop.defringeStrength, job.jpeg.develop.defringeEdgeThreshold,
          job.jpeg.develop.defringeLumaFloor, job.jpeg.develop.denoiseStrength, job.jpeg.develop.denoiseDetail,
          job.jpeg.develop.denoiseForceY, job.jpeg.develop.denoiseMaxScale, job.jpeg.develop.galoshRawMode,
          job.jpeg.develop.galoshStrength, job.jpeg.develop.galoshLuma, job.jpeg.develop.galoshChroma,
          job.jpeg.develop.galoshYuvMode, job.jpeg.develop.galoshYuvStrengthY, job.jpeg.develop.galoshYuvStrengthC);
        if (version == magic || version == magicV7 || version == magicV6)
            r(job.jpeg.output.ultraHdr, job.jpeg.develop.highlightReconstructionMethod,
              job.jpeg.develop.highlightThreshold, job.jpeg.develop.highlightCompression);
        if (version == magic || version == magicV7) r(job.jpeg.develop.multiframeChromaDenoise);
        if (version == magic) r(job.dng.compression, job.mergedDng.compression);
    } else {
        readLegacyJob(r, job);
        if (version == magicV4 || version == magicV3 || version == magicV2)
            r(job.dng.processingRecipe, job.dng.resolvedRecipe, job.dng.sourceRole, job.mergedDng.processingRecipe,
              job.mergedDng.resolvedRecipe, job.mergedDng.sourceRole, job.jpeg.develop.fccEdgeSigma,
              job.jpeg.develop.fccChromaBound, job.jpeg.develop.defringeStrength,
              job.jpeg.develop.defringeEdgeThreshold, job.jpeg.develop.defringeLumaFloor);
        // v4 save() wrote force/max-scale between the wavelet pair and the
        // GALOSH block; read in that order (the old reader read them last).
        if (version == magicV4)
            r(job.jpeg.develop.denoiseStrength, job.jpeg.develop.denoiseDetail, job.jpeg.develop.denoiseForceY,
              job.jpeg.develop.denoiseMaxScale);
        else if (version == magicV3)
            r(job.jpeg.develop.denoiseStrength, job.jpeg.develop.denoiseDetail);
        if (version == magicV4 || version == magicV3)
            r(job.jpeg.develop.galoshRawMode, job.jpeg.develop.galoshStrength, job.jpeg.develop.galoshLuma,
              job.jpeg.develop.galoshChroma, job.jpeg.develop.galoshYuvMode, job.jpeg.develop.galoshYuvStrengthY,
              job.jpeg.develop.galoshYuvStrengthC);
        if (version == legacyMagic && job.multiframe) {
            job.dng.sourceRole = "base";
            job.mergedDng.sourceRole = "merged";
        }
    }
    const auto n = payloadSize(job);
    const size_t count = job.multiframe ? job.metadata.size() : 1;
    // Validate file extent before allocating or uploading anything.
    auto pos = stream.tellg();
    stream.seekg(0, std::ios::end);
    auto end = stream.tellg();
    stream.seekg(pos);
    if (end - pos != std::streamoff(count * n + sizeof(uint32_t) + sizeof(uint64_t)))
        throw std::runtime_error("capture_job_extent_invalid");
    std::vector<uint8_t> raw(n);
    for (size_t i = 0; i < count; ++i) {
        r.bytes(raw.data(), n);
        if (consume) consume(job, i, raw);
    }
    if (!consume && !job.multiframe) job.frame.raw16 = std::move(raw);
    uint64_t marker = 0;
    r(marker);
    if (marker != footer) throw std::runtime_error("capture_job_commit_invalid");
    return job;
}
}  // namespace rawrcam::capture::persistence
