#include "develop/DevelopContextBuilder.h"

#include <algorithm>
namespace rawrcam::develop {
void applyDevelopSettings(rendered::RenderedStillContext& context, const DevelopSettings& settings) {
    context.colorRenderProfile = settings.colorRenderProfile;
    context.importedLutProfileId = settings.importedLutProfileId;
    context.fccSteps = std::clamp(settings.fccSteps, 1u, 8u);
    context.fccEdgeSigma = settings.fccEdgeSigma;
    context.fccChromaBound = settings.fccChromaBound;
    context.defringeStrength = settings.defringeStrength;
    context.defringeEdgeThreshold = settings.defringeEdgeThreshold;
    context.defringeLumaFloor = settings.defringeLumaFloor;
    context.highlightReconstructionEnabled = settings.highlightReconstructionEnabled;
    context.highlightReconstructionMethod = settings.highlightReconstructionMethod;
    context.highlightThreshold = settings.highlightThreshold;
    context.highlightCompression = settings.highlightCompression;
    context.captureSharpenIterations = settings.captureSharpenIterations;
}
}  // namespace rawrcam::develop
