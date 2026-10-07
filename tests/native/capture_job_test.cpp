#include <unistd.h>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

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
        assert(legacy.dng.outputFd == -1 && legacy.jpeg.output.outputFd == -1);
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
    j.frame.colorState.source = "frozen";
    j.frame.colorState.baselineWbRggb = {2, 1, 1, 3};
    j.dng.outputFd = 123;
    j.dng.displayName = "capture.dng";
    j.jpeg.output.outputFd = 456;
    j.jpegRequested = true;
    j.jpeg.output.displayName = "capture.jpg";
    j.jpeg.output.filmDescription = "Frozen C200";
    j.jpeg.output.quality = 99;
    j.tone.exposureEV = -1.5f;
    j.tone.renderTransform = tonemap::RenderTransform::Rec709;
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
    assert(restored.frame.raw16 == j.frame.raw16);
    assert(restored.frame.metadata.cameraContext->lensDistortion == camera->lensDistortion);
    assert(restored.frame.metadata.requestedSensitivity == 3200);
    assert(restored.frame.metadata.lensShadingMap == j.frame.metadata.lensShadingMap);
    assert(restored.frame.colorState.source == "frozen");
    assert(restored.dng.outputFd == -1 && restored.jpeg.output.outputFd == -1);
    assert(restored.jpeg.output.filmDescription == "Frozen C200" && restored.jpeg.output.quality == 99);
    assert(restored.film.film == 14 && restored.film.filmExposureEv == .75f && restored.tone.exposureEV == -1.5f);
    assert(restored.tone.renderTransform == tonemap::RenderTransform::Rec709);
    assert(restored.sharpnessScores.empty());
    assert(restored.dng.processingRecipe == j.dng.processingRecipe);
    assert(restored.dng.resolvedRecipe == j.dng.resolvedRecipe);
    assert(restored.dng.resolvedRecipe.find("\"aePostGain\":1.25") != std::string::npos);
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
