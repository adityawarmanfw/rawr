#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "capture/persistence/AcquiredRawJournal.h"
#include "capture/persistence/CaptureJob.h"
#include "capture/persistence/ProcessingRecipe.h"
#include "color/DcpProfile.h"
using namespace rawrcam::capture::persistence;
int main(int argc, char** argv) {
    if (argc == 2) {
        const auto legacy = load(argv[1]);
        assert(legacy.frame.raw16 == std::vector<uint8_t>(384, 7));
        assert(legacy.jpegRequested && legacy.filmEnabled && legacy.film.film == 14);
        assert(legacy.tone.exposureEV == -1.5f && legacy.dng.processingRecipe.empty());
        assert(legacy.tone.renderTransform == tonemap::RenderTransform::Existing);
        assert(legacy.tone.adaptiveTone.liftEv == 0.0f);
        assert(!legacy.dng.outputFd.valid() && !legacy.jpeg.output.outputFd.valid());
        assert(!legacy.dng.gps && !legacy.mergedDng.gps && !legacy.jpeg.output.gps);
    }
    std::string directory = (std::filesystem::temp_directory_path() / "rawr-capture-job-XXXXXX").string();
    assert(mkdtemp(directory.data()));
    const std::string root = directory;
    CaptureJob j;
    j.frame.width = 16;
    j.frame.height = 12;
    j.frame.packedRowStrideBytes = 32;
    j.frame.raw16.resize(384);
    for (size_t i = 0; i < j.frame.raw16.size(); ++i) j.frame.raw16[i] = uint8_t(i);
    auto camera = std::make_shared<rawrcam::metadata::CameraContextMetadata>();
    camera->cameraId = "3";
    camera->lensId = "wide";
    camera->lensDistortion = {1, 2, 3, 4, 5};
    j.frame.metadata.cameraContext = camera;
    j.frame.metadata.exposureTimeNs = 123456;
    j.frame.metadata.sensorNoiseProfile = {.001, .00002};
    j.frame.metadata.lensShadingMap = {1, 2, 3, 4};
    j.frame.metadata.requestedSensitivity = 3200;
    j.frame.metadata.suppressPreview = true;  // Transient startup state must not survive recovery.
    j.frame.colorState.source = "frozen";
    j.frame.colorState.baselineWbRggb = {2, 1, 1, 3};
    // Real descriptors: the contexts own and close them.
    j.dng.outputFd = rawrcam::encoding::OutputFd(::open("/dev/null", O_WRONLY));
    assert(j.dng.outputFd.valid());
    j.dng.displayName = "capture.dng";
    j.dng.gps = rawrcam::metadata::makeGps(0, 0, -5432.1, std::nullopt, 1793059200123LL);
    j.mergedDng.gps = j.dng.gps;
    j.jpeg.output.gps = j.dng.gps;
    j.dng.deviceModel = "V2562";
    j.dng.deviceDisplayModel = "X300 Ultra";
    j.mergedDng.deviceDisplayModel = "X300 Ultra";
    j.jpeg.output.outputFd = rawrcam::encoding::OutputFd(::open("/dev/null", O_WRONLY));
    j.jpegRequested = true;
    j.jpeg.output.displayName = "capture.jpg";
    j.jpeg.output.filmDescription = "Frozen C200";
    j.jpeg.output.quality = 99;
    j.tone.exposureEV = -1.5f;
    j.tone.adaptiveTone = {1.25f, -5.0f, -0.5f};
    j.tone.renderTransform = tonemap::RenderTransform::Rec709;
    j.tone.agx.whiteEv = 9.25f;
    j.tone.localTone = {2.5f, 1.5f, 3.5f, 8u, 1u};
    j.filmEnabled = true;
    j.film.film = 14;
    j.film.paper = 5;
    j.film.filmExposureEv = .75f;
    j.dng.processingRecipe = R"({"version":1,"filmSimEnabled":true})";
    j.dng.resolvedRecipe = resolvedRecipe(j.tone, 1.25f, j.filmEnabled, j.film);
    j.jpeg.develop.defringeStrength = .37f;
    j.jpeg.develop.fccEdgeSigma = .12f;
    j.jpeg.output.ultraHdr.enabled = true;
    j.jpeg.output.ultraHdr.mapBlurSigma = 1.5f;
    j.jpeg.develop.highlightReconstructionMethod = 1;
    j.jpeg.develop.highlightThreshold = .9f;
    j.jpeg.develop.multiframeChromaDenoise = true;
    j.dng.compression = rawrcam::encoding::dng::DngCompression::Uncompressed;
    j.mergedDng.compression = rawrcam::encoding::dng::DngCompression::Uncompressed;
    j.tuning.mergeAlgorithm = 3;
    j.tuning.hdrplusStrength = 18.0f;
    j.tuning.hdrplusTileSize = 16;
    j.tuning.bracketEv = -3.5f;
    j.tuning.bracketFrames = 4;
    auto path = jobPath(root, j.dng.displayName);
    save(path, j);
    auto restored = load(path);
    assert(restored.dng.gps && restored.mergedDng.gps && restored.jpeg.output.gps);
    assert(restored.dng.gps->altitude[0] == j.dng.gps->altitude[0]);
    assert(restored.mergedDng.gps->altitude[0] == j.dng.gps->altitude[0]);
    assert(restored.jpeg.output.gps->altitude[0] == j.dng.gps->altitude[0]);
    assert(!restored.jpeg.output.gps->has_horizontal_error);
    // Profile-aware jobs written before adaptive tone, and before AgX, used
    // the same POD prefix. Recreate those on-disk layouts and ensure the newer
    // fields keep their defaults.
    for (const uint32_t oldSize : {uint32_t(offsetof(tonemap::TonemapParams, adaptiveTone)),
                                   uint32_t(offsetof(tonemap::TonemapParams, agx)),
                                   uint32_t(offsetof(tonemap::TonemapParams, localTone)),
                                   uint32_t(offsetof(tonemap::TonemapParams, localTone) + offsetof(tonemap::LocalToneParams, sigma))}) {
        std::ifstream in(path, std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(in)), {});
        uint32_t fullSize = sizeof(j.tone);
        std::string record(reinterpret_cast<const char*>(&fullSize), sizeof(fullSize));
        record.append(reinterpret_cast<const char*>(&j.tone), sizeof(j.tone));
        const auto pos = bytes.find(record);
        assert(pos != std::string::npos && bytes.find(record, pos + 1) == std::string::npos);
        bytes.erase(pos + sizeof(fullSize) + oldSize, fullSize - oldSize);
        std::memcpy(bytes.data() + pos, &oldSize, sizeof(oldSize));
        const auto oldPath = root + "/old-profile.job";
        {
            std::ofstream out(oldPath, std::ios::binary);
            out.write(bytes.data(), bytes.size());
        }
        const auto old = load(oldPath);
        assert(old.tone.renderTransform == j.tone.renderTransform && old.tone.exposureEV == j.tone.exposureEV);
        assert(old.tone.adaptiveTone.liftEv ==
               (oldSize >= offsetof(tonemap::TonemapParams, agx) ? j.tone.adaptiveTone.liftEv : 0.0f));
        assert(old.tone.agx.whiteEv ==
               (oldSize >= offsetof(tonemap::TonemapParams, localTone) ? j.tone.agx.whiteEv : tonemap::AgxParams{}.whiteEv));
        const bool hadLocal = oldSize > offsetof(tonemap::TonemapParams, localTone);
        assert(old.tone.localTone.shadowsEv == (hadLocal ? 2.5f : 0.0f));
        assert(old.tone.localTone.highlightsEv == (hadLocal ? 1.5f : 2.0f));
        assert(old.tone.localTone.sigma == 5.0f && old.tone.localTone.coarsestMip == 6u && !old.tone.localTone.boostLocalContrast);
        std::filesystem::remove(oldPath);
    }

    assert(restored.dng.deviceModel == "V2562");
    assert(restored.dng.deviceDisplayModel == "X300 Ultra");
    assert(restored.mergedDng.deviceDisplayModel == "X300 Ultra");
    assert(restored.frame.raw16 == j.frame.raw16);
    assert(restored.frame.metadata.cameraContext->lensDistortion == camera->lensDistortion);
    assert(restored.frame.metadata.requestedSensitivity == 3200);
    assert(!restored.frame.metadata.suppressPreview);
    assert(restored.frame.metadata.lensShadingMap == j.frame.metadata.lensShadingMap);
    assert(restored.frame.colorState.source == "frozen");
    assert(!restored.dng.outputFd.valid() && !restored.jpeg.output.outputFd.valid());
    // Copies are snapshots: they never share the original's descriptor, which stays open.
    {
        const int original = j.dng.outputFd.get();
        auto snapshot = j.dng;
        assert(!snapshot.outputFd.valid() && j.dng.outputFd.get() == original);
        auto moved = std::move(j.dng);
        assert(moved.outputFd.get() == original && !j.dng.outputFd.valid());
        j.dng = std::move(moved);
        assert(::fcntl(original, F_GETFD) != -1);
    }
    assert(restored.jpeg.output.filmDescription == "Frozen C200" && restored.jpeg.output.quality == 99);
    assert(restored.film.film == 14 && restored.film.filmExposureEv == .75f && restored.tone.exposureEV == -1.5f);
    assert(restored.tone.renderTransform == tonemap::RenderTransform::Rec709 && restored.tone.agx.whiteEv == 9.25f);
    assert(restored.tone.localTone.shadowsEv == 2.5f && restored.tone.localTone.highlightsEv == 1.5f);
    assert(restored.tone.localTone.sigma == 3.5f && restored.tone.localTone.coarsestMip == 8u && restored.tone.localTone.boostLocalContrast == 1u);
    assert(restored.tone.adaptiveTone.liftEv == 1.25f && restored.tone.adaptiveTone.keyEv == -5.0f &&
           restored.tone.adaptiveTone.highlightEv == -0.5f);
    assert(restored.sharpnessScores.empty());
    assert(restored.dng.processingRecipe == j.dng.processingRecipe);
    assert(restored.dng.resolvedRecipe == j.dng.resolvedRecipe);
    assert(restored.dng.resolvedRecipe.find("\"aePostGain\":1.25") != std::string::npos);
    assert(restored.dng.resolvedRecipe.find("\"autoKeyEv\":1.25") != std::string::npos);
    assert(resolvedRecipe(j.tone, 2.5f, j.filmEnabled, j.film).find("\"autoKeyEv\":1.25") != std::string::npos);
    assert(restored.dng.resolvedRecipe.find("\"localTone\":{\"version\":3,\"method\":\"exposureFusion\",\"shadowsEv\":2.5,\"highlightsEv\":1.5") != std::string::npos);
    assert(restored.jpeg.develop.defringeStrength == .37f && restored.jpeg.develop.fccEdgeSigma == .12f);
    assert(restored.jpeg.output.ultraHdr.enabled && restored.jpeg.output.ultraHdr.mapBlurSigma == 1.5f);
    assert(restored.jpeg.develop.highlightReconstructionMethod == 1 && restored.jpeg.develop.highlightThreshold == .9f);
    assert(restored.jpeg.develop.multiframeChromaDenoise);
    // v8: DNG compression survives the journal (multiframe DNGs honour "Uncompressed").
    assert(restored.dng.compression == rawrcam::encoding::dng::DngCompression::Uncompressed);
    assert(restored.mergedDng.compression == rawrcam::encoding::dng::DngCompression::Uncompressed);
    assert(restored.tuning.mergeAlgorithm == 3 && restored.tuning.hdrplusStrength == 18.0f &&
           restored.tuning.hdrplusTileSize == 16 && restored.tuning.bracketEv == -3.5f &&
           restored.tuning.bracketFrames == 4);
    markFilmFallback(path);
    assert(std::filesystem::exists(path + ".fallback"));
    assert(load(path).dng.processingRecipe == j.dng.processingRecipe);
    j.multiframe = true;
    j.baseFrameMode = rawrcam::capture::multiframe::MultiframeBaseFrameMode::Sharpest;
    j.sharpnessScores = {0.5f, 2.25f};
    j.sharpnessMs = 18.25;
    j.referenceIndex = 1;
    j.metadata = {j.frame.metadata, j.frame.metadata};
    j.parameters.resize(2);
    auto burstPath = jobPath(root, "burst.dng");
    save(burstPath, j, [&](size_t i) {
        auto raw = j.frame.raw16;
        raw[0] = i + 7;
        return raw;
    });
    size_t frames = 0;
    auto burst = load(burstPath, [&](CaptureJob& header, size_t i, const std::vector<uint8_t>& raw) {
        assert(header.multiframe);
        assert(raw.size() == 384 && raw[0] == i + 7);
        ++frames;
    });
    assert(frames == 2 && burst.referenceIndex == 1 && burst.frame.raw16.empty());
    assert(burst.baseFrameMode == rawrcam::capture::multiframe::MultiframeBaseFrameMode::Sharpest);
    assert(burst.sharpnessScores.size() == 2 && burst.sharpnessScores[0] == 0.5f &&
           burst.sharpnessScores[1] == 2.25f);
    assert(burst.sharpnessMs == 18.25);
    // Failed replacement never destroys the previously committed RAW input.
    bool failed = false;
    try {
        save(burstPath, j, [](size_t) -> std::vector<uint8_t> { throw std::runtime_error("disk/readback failure"); });
    } catch (...) {
        failed = true;
    }
    assert(failed && !std::filesystem::exists(burstPath + ".tmp"));
    assert(load(burstPath).multiframe);
    std::filesystem::resize_file(path, 32);
    failed = false;
    try {
        (void)load(path);
    } catch (...) {
        failed = true;
    }
    assert(failed);
    // Burst length is independent of the six concurrently leased camera buffers.
    // Acquisition appends and releases one frame at a time, including bursts above six.
    for (uint32_t count : {16u, 30u}) {
        auto large = j;
        large.metadata.resize(count, j.frame.metadata);
        large.parameters.resize(count);
        large.sharpnessScores.resize(count, 1.0f);
        const auto largePath = jobPath(root, "large-" + std::to_string(count) + ".dng");
        AcquiredRawJournal acquired(root, 384, count);
        for (uint32_t i = 0; i < count; ++i) {
            auto pixels = std::vector<uint8_t>(384, static_cast<uint8_t>(i));
            acquired.append(pixels);
        }
        acquired.commit(largePath, large);
        size_t received = 0;
        load(largePath, [&](CaptureJob&, size_t i, const std::vector<uint8_t>& raw) {
            assert(raw.size() == 384 && raw.front() == i && raw.back() == i);
            ++received;
        });
        assert(received == count);
    }
    // Acquired RAW becomes the journal in place. Old v1/v9 jobs above still load.
    const auto directPath = jobPath(root, "direct.dng");
    std::string pendingPath;
    struct stat before{}, after{};
    {
        AcquiredRawJournal acquired(root, 384, 2);
        pendingPath = acquired.pendingPath();
        assert(::stat(pendingPath.c_str(), &before) == 0);
        auto pixels = std::vector<uint8_t>(384, 17);
        acquired.append(pixels);
        failed = false;
        try {
            acquired.commit(directPath, j);
        } catch (const std::runtime_error&) {
            failed = true;
        }
        assert(failed && !std::filesystem::exists(directPath));
        failed = false;
        try {
            uint64_t pendingOffset = 0;
            (void)loadHeader(pendingPath, pendingOffset);
        } catch (const std::runtime_error&) {
            failed = true;
        }
        assert(failed);  // partial input is never recoverable
        failed = false;
        try {
            acquired.append(std::vector<uint8_t>(383));
        } catch (const std::runtime_error&) {
            failed = true;
        }
        assert(failed);
        // Cleanup discards an unlocked process-death remnant, keeps live files.
        const auto orphan = root + "/still_jobs/.acquiring-orphan.tmp";
        {
            std::ofstream out(orphan);
            out << "partial";
        }
        std::string aborted;
        {
            AcquiredRawJournal other(root, 384, 2);
            aborted = other.pendingPath();
            assert(!std::filesystem::exists(orphan));
            assert(std::filesystem::exists(pendingPath));
        }
        assert(!std::filesystem::exists(aborted));
        std::fill(pixels.begin(), pixels.end(), 29);
        acquired.append(pixels);
        acquired.commit(directPath, j);
        assert(acquired.pendingPath().empty());
        assert(::stat(directPath.c_str(), &after) == 0);
        assert(before.st_dev == after.st_dev && before.st_ino == after.st_ino);  // rename, no second payload file
        failed = false;
        try {
            acquired.commit(directPath, j);
        } catch (const std::runtime_error&) {
            failed = true;
        }
        assert(failed);
    }
    assert(!std::filesystem::exists(pendingPath) && std::filesystem::exists(directPath));
    uint64_t directOffset = 0;
    const auto direct = loadHeader(directPath, directOffset);
    assert(direct.dng.gps && direct.mergedDng.gps && direct.jpeg.output.gps);
    assert(direct.dng.gps->altitude[0] == j.dng.gps->altitude[0]);
    assert(directOffset == 24 && direct.referenceIndex == j.referenceIndex);
    assert(direct.dng.deviceDisplayModel == "X300 Ultra");
    assert(direct.dng.resolvedRecipe == j.dng.resolvedRecipe);
    assert(direct.sharpnessScores == j.sharpnessScores);
    frames = 0;
    (void)load(directPath, [&](CaptureJob&, size_t i, const std::vector<uint8_t>& pixels) {
        assert(pixels == std::vector<uint8_t>(384, i == 0 ? 17 : 29));
        ++frames;
    });
    assert(frames == 2);
    // Sharpening survives ordinary burst journals and acquisition journals,
    // including the header-only path used by lazy multiframe loading.
    for (uint32_t iterations : {0u, 2u, 3u, 4u, 5u, 6u, 7u}) {
        j.jpeg.develop.captureSharpenIterations = iterations;
        for (bool acquisition : {false, true}) {
            const auto sharpeningPath = jobPath(root, acquisition ? "sharp-acquired.dng" : "sharp-burst.dng");
            if (acquisition) {
                AcquiredRawJournal acquired(root, 384, 2);
                acquired.append(j.frame.raw16);
                acquired.append(j.frame.raw16);
                acquired.commit(sharpeningPath, j);
            } else {
                save(sharpeningPath, j, [&](size_t) { return j.frame.raw16; });
            }
            uint64_t offset = 0;
            assert(loadHeader(sharpeningPath, offset).jpeg.develop.captureSharpenIterations == iterations);
            size_t consumed = 0;
            const auto loaded = load(sharpeningPath, [&](CaptureJob& header, size_t, const std::vector<uint8_t>& pixels) {
                assert(header.jpeg.develop.captureSharpenIterations == iterations);
                assert(pixels == j.frame.raw16);
                ++consumed;
            });
            assert(consumed == 2 && loaded.jpeg.develop.captureSharpenIterations == iterations);

            // Recreate v15/v16 by removing the mirror, then v13/v14 by removing the empty camera profile,
            // v11/v12 by also removing the (absent) lens distortion, then
            // v9/v10 by also removing sharpening. Existing journals keep
            // their original behavior and frame addressing.
            std::ifstream input(sharpeningPath, std::ios::binary);
            std::string bytes((std::istreambuf_iterator<char>(input)), {});
            constexpr size_t footerBytes = sizeof(uint32_t) + sizeof(uint64_t);
            constexpr size_t sharpeningBytes = 2 * sizeof(uint32_t);
            constexpr size_t distortionBytes = sizeof(uint32_t) + sizeof(bool);  // absent optional
            constexpr size_t dcpBytes = sizeof(uint32_t) + sizeof(uint64_t);     // empty string
            size_t fieldEnd = acquisition ? bytes.size() - footerBytes : offset;
            const size_t gpsBytes = sizeof(uint32_t) + sizeof(bool) + sizeof(uint32_t) + sizeof(tinydng_gps);
            bytes.erase(fieldEnd - gpsBytes, gpsBytes);
            fieldEnd -= gpsBytes;
            {
                auto v17 = bytes;
                const uint64_t version = acquisition ? 0x524157524a4f4212ull : 0x524157524a4f4211ull;
                std::memcpy(v17.data() + sizeof(uint32_t), &version, sizeof(version));
                const auto legacyPath = jobPath(root, "v17-no-gps.dng");
                {
                    std::ofstream output(legacyPath, std::ios::binary);
                    output.write(v17.data(), v17.size());
                }
                const auto legacy = loadHeader(legacyPath, offset);
                assert(!legacy.dng.gps && !legacy.mergedDng.gps && !legacy.jpeg.output.gps);
                size_t count = 0;
                (void)load(legacyPath, [&](CaptureJob&, size_t, const std::vector<uint8_t>& pixels) {
                    assert(pixels == j.frame.raw16);
                    ++count;
                });
                assert(count == 2);
            }
            constexpr size_t mirrorBytes = 2 * sizeof(uint32_t);
            bytes.erase(fieldEnd - mirrorBytes, mirrorBytes);
            fieldEnd -= mirrorBytes;
            {
                auto v15 = bytes;
                const uint64_t version = acquisition ? 0x524157524a4f4210ull : 0x524157524a4f420full;
                std::memcpy(v15.data() + sizeof(uint32_t), &version, sizeof(version));
                const auto path = jobPath(root, "v15-profile.dng");
                {
                    std::ofstream output(path, std::ios::binary);
                    output.write(v15.data(), v15.size());
                }
                assert(loadHeader(path, offset).frame.metadata.cameraContext->profileMirror == 0);
                size_t count = 0;
                (void)load(path, [&](CaptureJob& header, size_t, const std::vector<uint8_t>& pixels) {
                    assert(header.frame.metadata.cameraContext->profileMirror == 0);
                    assert(pixels == j.frame.raw16);
                    ++count;
                });
                assert(count == 2);
            }
            {
                auto v13 = bytes;
                v13.erase(fieldEnd - dcpBytes, dcpBytes);
                const uint64_t version = acquisition ? 0x524157524a4f420eull : 0x524157524a4f420dull;
                std::memcpy(v13.data() + sizeof(uint32_t), &version, sizeof(version));
                const auto v13Path = jobPath(root, "v13-distortion.dng");
                {
                    std::ofstream output(v13Path, std::ios::binary);
                    output.write(v13.data(), v13.size());
                }
                assert(loadHeader(v13Path, offset).jpeg.develop.captureSharpenIterations == iterations);
                size_t v13Frames = 0;
                (void)load(v13Path, [&](CaptureJob&, size_t, const std::vector<uint8_t>& pixels) {
                    assert(pixels == j.frame.raw16);
                    ++v13Frames;
                });
                assert(v13Frames == 2);
            }
            bytes.erase(fieldEnd - dcpBytes, dcpBytes);
            fieldEnd -= dcpBytes;
            {
                auto v11 = bytes;
                v11.erase(fieldEnd - distortionBytes, distortionBytes);
                const uint64_t version = acquisition ? 0x524157524a4f420cull : 0x524157524a4f420bull;
                std::memcpy(v11.data() + sizeof(uint32_t), &version, sizeof(version));
                const auto v11Path = jobPath(root, "v11-sharpening.dng");
                {
                    std::ofstream output(v11Path, std::ios::binary);
                    output.write(v11.data(), v11.size());
                }
                assert(loadHeader(v11Path, offset).jpeg.develop.captureSharpenIterations == iterations);
                size_t v11Frames = 0;
                (void)load(v11Path, [&](CaptureJob&, size_t, const std::vector<uint8_t>& pixels) {
                    assert(pixels == j.frame.raw16);
                    ++v11Frames;
                });
                assert(v11Frames == 2);
            }
            bytes.erase(fieldEnd - distortionBytes - sharpeningBytes, distortionBytes + sharpeningBytes);
            const uint64_t oldVersion = acquisition ? 0x524157524a4f420aull : 0x524157524a4f4209ull;
            std::memcpy(bytes.data() + sizeof(uint32_t), &oldVersion, sizeof(oldVersion));
            const auto oldPath = jobPath(root, "old-sharpening.dng");
            {
                std::ofstream output(oldPath, std::ios::binary);
                output.write(bytes.data(), bytes.size());
            }
            assert(loadHeader(oldPath, offset).jpeg.develop.captureSharpenIterations == 0);
            consumed = 0;
            (void)load(oldPath, [&](CaptureJob& header, size_t, const std::vector<uint8_t>& pixels) {
                assert(header.jpeg.develop.captureSharpenIterations == 0);
                assert(pixels == j.frame.raw16);
                ++consumed;
            });
            assert(consumed == 2);
        }
    }
    // A mirror alone must survive both journal formats and lazy header loading.
    for (uint32_t mirror : {0u, 1u, 2u, 3u}) {
        for (bool acquisition : {false, true}) {
            auto mirrored = j;
            auto context = std::make_shared<rawrcam::metadata::CameraContextMetadata>(*camera);
            context->profileMirror = mirror;
            mirrored.frame.metadata.cameraContext = context;
            for (auto& m : mirrored.metadata) m.cameraContext = context;
            const auto path = jobPath(root, "mirror.dng");
            if (acquisition) {
                AcquiredRawJournal acquired(root, 384, 2);
                acquired.append(j.frame.raw16);
                acquired.append(j.frame.raw16);
                acquired.commit(path, mirrored);
            } else {
                save(path, mirrored, [&](size_t) { return j.frame.raw16; });
            }
            uint64_t offset = 0;
            const auto header = loadHeader(path, offset);
            assert(header.frame.metadata.cameraContext->profileMirror == mirror);
            for (const auto& m : header.metadata) assert(m.cameraContext->profileMirror == mirror);
            size_t count = 0;
            const auto loaded = load(path, [&](CaptureJob& h, size_t, const std::vector<uint8_t>& pixels) {
                assert(h.frame.metadata.cameraContext->profileMirror == mirror);
                assert(pixels == j.frame.raw16);
                ++count;
            });
            assert(count == 2);
            for (const auto& m : loaded.metadata) assert(m.cameraContext->profileMirror == mirror);
        }
    }
    // A lens profile calibration rides along once per job and comes back on every frame's context.
    for (bool acquisition : {false, true}) {
        rawrcam::geometry::LensDistortionModel lens;
        lens.width = 4096;
        lens.height = 3072;
        lens.fx = lens.fy = 1698.0;
        lens.cx = 2047.5;
        lens.cy = 1535.5;
        lens.k = {-0.05, 0.015, 0.0001, 0.0003, 0, 0, -0.02, 0.014};
        lens.outputScale = 0.97;
        // A minimal single-illuminant camera profile ("IIRC", ColorMatrix1 + D65).
        std::vector<uint8_t> dcpFile = {'I', 'I', 0x52, 0x43, 8, 0, 0, 0, 2, 0};
        const auto put16 = [&](uint16_t v) { dcpFile.insert(dcpFile.end(), {uint8_t(v), uint8_t(v >> 8)}); };
        const auto put32 = [&](uint32_t v) {
            dcpFile.insert(dcpFile.end(), {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)});
        };
        put16(50721), put16(10), put32(9), put32(8 + 2 + 2 * 12 + 4);
        put16(50778), put16(3), put32(1), put16(21), put16(0);
        put32(0);
        for (int i = 0; i < 9; ++i) put32(i % 4 == 0 ? 1 : 0), put32(1);
        auto dcp = rawrcam::color::parseDcp(dcpFile);
        assert(dcp);
        const auto dcpProfile = std::make_shared<const rawrcam::color::DcpProfile>(std::move(*dcp));
        auto withLens = j;
        const auto attach = [&](rawrcam::metadata::CameraContextMetadataPtr& context) {
            auto copy = std::make_shared<rawrcam::metadata::CameraContextMetadata>(
                context ? *context : rawrcam::metadata::CameraContextMetadata{});
            copy->profileDistortion = lens;
            copy->dcpProfile = dcpProfile;
            context = copy;
        };
        attach(withLens.frame.metadata.cameraContext);
        for (auto& m : withLens.metadata) attach(m.cameraContext);
        const auto lensPath = jobPath(root, acquisition ? "lens-acquired.dng" : "lens-burst.dng");
        if (acquisition) {
            AcquiredRawJournal acquired(root, 384, 2);
            acquired.append(j.frame.raw16);
            acquired.append(j.frame.raw16);
            acquired.commit(lensPath, withLens);
        } else {
            save(lensPath, withLens, [&](size_t) { return j.frame.raw16; });
        }
        const auto loaded = load(lensPath, [](CaptureJob&, size_t, const std::vector<uint8_t>&) {});
        const auto same = [&](const rawrcam::metadata::CameraContextMetadataPtr& context) {
            return context && context->profileDistortion && context->profileDistortion->fx == lens.fx &&
                   context->profileDistortion->k == lens.k && context->profileDistortion->outputScale == lens.outputScale &&
                   context->dcpProfile && context->dcpProfile->bytes == dcpFile &&
                   context->dcpProfile->illuminant1 == 21;
        };
        assert(same(loaded.frame.metadata.cameraContext));
        for (const auto& m : loaded.metadata) assert(same(m.cameraContext));
    }
    // Strip loader's exact addressing contract still reads rows from either frame.
    {
        std::ifstream input(directPath, std::ios::binary);
        input.seekg(std::streamoff(directOffset + 384 + 4 * 32));
        std::vector<uint8_t> rows(3 * 32);
        input.read(reinterpret_cast<char*>(rows.data()), rows.size());
        assert(rows == std::vector<uint8_t>(3 * 32, 29));
    }
    auto damaged = jobPath(root, "damaged.dng");
    std::filesystem::copy_file(directPath, damaged);
    {
        std::fstream file(damaged, std::ios::in | std::ios::out | std::ios::binary);
        file.seekp(16);  // serialized header-offset value, after the size prefix
        uint64_t badOffset = UINT64_MAX;
        file.write(reinterpret_cast<const char*>(&badOffset), sizeof(badOffset));
    }
    failed = false;
    try {
        (void)loadHeader(damaged, directOffset);
    } catch (const std::runtime_error&) {
        failed = true;
    }
    assert(failed);
    std::filesystem::resize_file(directPath, std::filesystem::file_size(directPath) - 1);
    failed = false;
    try {
        (void)loadHeader(directPath, directOffset);
    } catch (const std::runtime_error&) {
        failed = true;
    }
    assert(failed);
    // No lifetime shot count: the transient budget returns after every spool.
    for (int i = 0; i < 40; ++i) {
        auto token = reserve(root, 25 * 1024 * 1024, true);
        assert(token);
    }
    std::vector<std::shared_ptr<void>> held;
    while (auto token = reserve(root, 25 * 1024 * 1024, true)) {
        held.push_back(token);
        assert(held.size() < 100);
    }
    assert(!held.empty());
    held.clear();
    assert(reserve(root, 25 * 1024 * 1024, true));
    assert(!reserve(root, 31ull * 1024 * 1024 * 1024, false));
    // A real 30-frame F85 input exceeds 8 GiB. Reservation does not allocate/write the payload.
    constexpr uint64_t f85BurstBytes = 16320ull * 12288 * 2 * 30;
    struct statvfs disk{};
    assert(statvfs(root.c_str(), &disk) == 0);
    if (uint64_t(disk.f_bavail) * disk.f_frsize > f85BurstBytes * 2 + 1024ull * 1024 * 1024) {
        auto large = reserve(root, f85BurstBytes, false);
        assert(large);
    }
    std::filesystem::remove_all(root);
    std::cout << "Capture job roundtrip, burst, truncation, atomic replacement and budget tests passed\n";
}
