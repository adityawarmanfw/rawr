#pragma once
#include "develop/render/RenderCommandSession.h"
#include "develop/render/RenderResources.h"
namespace rawrcam::develop::rendered {
bool dumpRenderImage(const RenderDeviceContext& context, VkImage image, uint32_t width, uint32_t height,
                     const std::string& path, const std::array<uint32_t, 4>& roi);
void completeReadback(RenderResources& resources, RenderCommandSession& commands, const RenderedStillContext& rendered,
                      VkImage sourceImage, const std::string& filesDir, bool preWbOk, const char* clipEvidence,
                      bool gainmapClipUsed, float gainmapClipBoost,
                      std::chrono::steady_clock::time_point tFence, RenderedStillCompletion& done,
                      const RenderResources::Diagnostic& emit);
}  // namespace rawrcam::develop::rendered
