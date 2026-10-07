// Semantic test for the multiframe EXIF description blocks: totals must
// equal the sum of the printed stages, and mode-dependent lines must render.
#include <cassert>
#include <iostream>
#include <string>

#include "encoding/jpeg/JpegTimingsFormat.h"
#include "capture/multiframe/MultiframeDescription.h"

using namespace rawrcam::capture::multiframe;
using rawrcam::encoding::jpeg::formatImageProcessing;
using rawrcam::encoding::jpeg::formatJpegEncoding;
using rawrcam::encoding::jpeg::imageProcessingTotalMs;
using rawrcam::encoding::jpeg::overallTotalMs;

namespace {
bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

MultiframeStageTimings fixtureStages() {
    MultiframeStageTimings stages{};
    stages.baseReadMs = 24.8;
    stages.sharpnessMs = 18.3;
    stages.initMs = 45.2;
    stages.refPrepareMs = 12.4;
    stages.refStatsMs = 8.1;
    stages.companionPrepareMs = 96.3;
    stages.alignMs = 783.9;
    stages.kernelMs = 210.5;
    stages.robustnessMs = 1180.2;
    stages.accumMs = 745.8;
    stages.finalizeMs = 19.6;
    stages.projectMs = 31.2;
    stages.rgbPrepMs = 96.4;
    stages.initNote = "pipelines warm";
    return stages;
}
}  // namespace

int main() {
    const auto stages = fixtureStages();
    // Totals equal the sum of their parts (EXIF contract: TOTAL adds up).
    assert(stages.alignmentTotalMs() == 12.4 + 96.3 + 783.9);
    assert(stages.mergeTotalMs() == 8.1 + 210.5 + 1180.2 + 745.8 + 19.6);
    assert(stages.multiframeTotalMs() == 45.2 + 18.3 + stages.alignmentTotalMs() + stages.mergeTotalMs());
    assert(imageProcessingTotalMs(24.8, 31.2, 96.4, 6400.0, 210.7, 88.3, 342.9, 906.8, 12.5, 85.2) ==
           24.8 + 31.2 + 96.4 + 6400.0 + 210.7 + 88.3 + 342.9 + 906.8 + 12.5 + 85.2);
    assert(overallTotalMs(stages.multiframeTotalMs(), 1701.1, 142.6) ==
           stages.multiframeTotalMs() + 1701.1 + 142.6);
    assert(dngSubtotalMs(24.8, stages.multiframeTotalMs(), 31.2) == 24.8 + stages.multiframeTotalMs() + 31.2);

    MultiframeTuningView tuning{};
    tuning.outputScale = 1.264911f;
    tuning.kDetail = 0.08f;
    MultiframeOutputInfo output{};
    TonemapUiValues tone{};
    const std::string params =
        buildParametersBlock(tuning, output, tone, "Film\nStock: C200\n", false, 22, 5088, 3816);
    assert(contains(params, "Multiframe parameters:"));
    assert(contains(params, "Multiframe Reconstruction inputs:"));
    assert(contains(params, "JPEG output parameters:"));
    assert(contains(params, "Tonemap parameters:"));
    assert(contains(params, "Coverage Neff:"));
    assert(contains(params, "kDetail: 0.080"));
    assert(!contains(params, "Stock: C200"));
    const std::string filmParams =
        buildParametersBlock(tuning, output, tone, "Film\nStock: C200\n", true, 22, 5088, 3816);
    assert(contains(filmParams, "Stock: C200"));
    assert(!contains(filmParams, "Tonemap parameters:"));
    MultiframeTuningView defaults{};
    const std::string defaultParams = buildParametersBlock(defaults, output, tone, "", false, 30, 4096, 3072);
    MultiframeTuningView bracketed = defaults;
    bracketed.mergeAlgorithm = 3;
    bracketed.bracketEv = -2.5f;
    bracketed.bracketFrames = 3;
    bracketed.bracketLiftEv = 2.5f;
    const std::string bracketParams = buildParametersBlock(bracketed, output, tone, "", false, 19, 4096, 3072);
    assert(contains(bracketParams, "HDR+ bracketed"));
    assert(contains(bracketParams, "Bracket: 3 x -2.500 EV"));
    assert(contains(bracketParams, "Bracket lift: 2.500 EV"));
    bracketed.bracketLiftEv = 0.0f;
    assert(contains(buildParametersBlock(bracketed, output, tone, "", false, 16, 4096, 3072), "none (no dark frames)"));
    assert(contains(defaultParams, "Coverage Neff:"));
    assert(!contains(defaultParams, "Low support rescue"));

    const std::string middle = buildSharpnessSection(MultiframeBaseFrameMode::Middle, {}, 1, 4);
    assert(contains(middle, "Mode: middle"));
    assert(contains(middle, "frame 2 of 4"));
    const std::vector<float> scores{0.5f, 2.0f, 1.0f, 1.5f};
    const std::string sharp = buildSharpnessSection(MultiframeBaseFrameMode::Sharpest, scores, 1, 4);
    assert(contains(sharp, "Mode: sharpest"));
    assert(contains(sharp, "F2: 1.000 (base)"));
    assert(contains(sharp, "F1: 0.250"));
    const std::string fallback = buildSharpnessSection(MultiframeBaseFrameMode::Sharpest, {}, 2, 4);
    assert(contains(fallback, "middle fallback"));

    const std::string timings = formatMultiframeTimings(stages, MultiframeBaseFrameMode::Sharpest);
    assert(contains(timings, "Multiframe: 3120.3 ms"));
    assert(contains(timings, "Sharpness: 18.3 ms"));
    for (const char* label :
         {"Reference prepare:", "Reference stats:", "Companion prepare:", "Block/LK align:", "Noise estimate:",
          "Kernel stats:", "Robustness:", "Accumulation:", "Finalize:"}) {
        assert(contains(timings, label));
    }
    const std::string timingsMiddle = formatMultiframeTimings(stages, MultiframeBaseFrameMode::Middle);
    assert(contains(timingsMiddle, "skipped (middle)"));

    const std::string imageProc =
        formatImageProcessing(24.8, 31.2, true, 0.0, 96.4, 6400.0, 210.7, false, 1, 88.3, 342.9, 906.8, false, 12.5,
                              85.2);
    assert(contains(imageProc, "Image Processing: 8198.8 ms"));
    assert(contains(imageProc, "Merged RGB prep: 96.4 ms"));
    assert(contains(imageProc, "Render setup: 6400.0 ms"));
    assert(contains(imageProc, "Submit/fence gaps: 12.5 ms"));
    assert(contains(imageProc, "Readback: 85.2 ms"));
    assert(!contains(imageProc, "JPEG encoding"));
    assert(contains(formatJpegEncoding(142.6), "\nJPEG encoding: 142.6 ms"));
    // UltraHDR: GPU gain map stage joins the total and prints its own line.
    // Zero keeps legacy output byte-identical.
    assert(imageProcessingTotalMs(0.0, 0.0, 1113.5, 3598.2, 2.6, 16.7, 10.8, 553.7, 5.1, 87.9) ==
           1113.5 + 3598.2 + 2.6 + 16.7 + 10.8 + 553.7 + 5.1 + 87.9);
    assert(imageProcessingTotalMs(0.0, 0.0, 1113.5, 3598.2, 2.6, 16.7, 10.8, 553.7, 5.1, 87.9, 3.2) ==
           1113.5 + 3598.2 + 2.6 + 16.7 + 10.8 + 553.7 + 3.2 + 5.1 + 87.9);
    const std::string uhdr =
        formatImageProcessing(0.0, 0.0, false, 1113.5, 0.0, 3598.2, 2.6, false, 1, 16.7, 10.8, 553.7, false, 5.1,
                              87.9, false, 3.2);
    assert(contains(uhdr, "Image Processing: 5391.7 ms"));
    assert(contains(uhdr, "- Gain map (GPU): 3.2 ms"));
    const std::string remosaic = formatImageProcessing(24.8, 31.2, false, 400.0, 0.0, 35.0, 210.7, true, 2, 88.3,
                                                       342.9, 906.8, false, 4.1, 60.3);
    // Single-frame: no base/projection lines, totals still exact.
    const std::string single =
        formatImageProcessing(0.0, 0.0, false, 1113.5, 0.0, 3598.2, 2.6, false, 1, 16.7, 10.8, 553.7, true, 5.1,
                              87.9, false);
    assert(!contains(single, "Base readback:"));
    assert(!contains(single, "CFA projection:"));
    assert(!contains(single, "Gain map (GPU):"));
    assert(contains(single, "Image Processing: 5388.5 ms"));
    assert(contains(single, "Film record: 553.7 ms"));
    assert(contains(remosaic, "Demosaic: 400.0 ms"));
    assert(contains(remosaic, "on (time in Demosaic)"));
    assert(contains(remosaic, "2 steps"));

    // Described highlight stage: off prints "off", Inpaint Opposed points at
    // colour processing, and defringe/tone time gets its own line in the total.
    const std::string hlOff = formatImageProcessing(0.0, 0.0, false, 100.0, 0.0, 10.0, 0.4, true, 1, 20.0, 5.0,
                                                    30.0, false, 2.0, 8.0, false, 0.0, 0.0, 0.0, 0, 0.0, false);
    assert(contains(hlOff, "- HL reconstruction: off\n"));
    assert(!contains(hlOff, "Defringe"));
    assert(contains(hlOff, "Image Processing: 175.4 ms"));
    const std::string io = formatImageProcessing(0.0, 0.0, false, 100.0, 0.0, 10.0, 0.1, true, 1, 20.0, 6.0,
                                                 30.0, false, 2.0, 8.0, false, 0.0, 0.0, 0.0, 2, 3.5, true);
    assert(contains(io, "- HL reconstruction (Inpaint Opposed): in Color processing\n"));
    assert(contains(io, "- Color processing (WB + Inpaint Opposed): 6.0 ms"));
    assert(contains(io, "- Defringe + highlight tone: 3.5 ms"));
    assert(contains(io, "Image Processing: 179.6 ms"));
    const std::string propagation = formatImageProcessing(0.0, 0.0, false, 100.0, 0.0, 10.0, 4.2, true, 1, 20.0,
                                                          5.0, 30.0, false, 2.0, 8.0, false, 0.0, 0.0, 0.0, 1,
                                                          1.5, true);
    assert(contains(propagation, "- HL reconstruction (color propagation): 4.2 ms"));
    assert(contains(propagation, "- Defringe: 1.5 ms"));
    assert(contains(propagation, "- Color processing: 5.0 ms"));
    const std::string forced = formatImageProcessing(0.0, 0.0, false, 100.0, 0.0, 10.0, 0.1, true, 1, 20.0, 6.0,
                                                     30.0, false, 2.0, 8.0, false, 0.0, 0.0, 0.0, 2, 2.4, false,
                                                     true);
    assert(contains(forced, "- HL reconstruction (Inpaint Opposed, forced on by UltraHDR): in Color processing"));
    assert(contains(forced, "- Highlight tone (Inpaint Opposed): 2.4 ms"));

    assert(contains(formatOutputProjection(24.8, 31.2), "CFA projection: 31.2 ms"));
    assert(contains(buildExifHeader(), "Multiframe GPU merge"));

    std::cout << "Multiframe EXIF description tests passed\n";
}
