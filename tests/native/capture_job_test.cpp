#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "capture/persistence/AcquiredRawJournal.h"
#include "capture/persistence/CaptureJob.h"
#include "capture/persistence/ProcessingRecipe.h"
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
    assert(!reserve(root, 9ull * 1024 * 1024 * 1024, false));
    std::filesystem::remove_all(root);
    std::cout << "Capture job roundtrip, burst, truncation, atomic replacement and budget tests passed\n";
}
