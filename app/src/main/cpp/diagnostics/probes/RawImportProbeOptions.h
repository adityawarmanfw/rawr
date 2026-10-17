#pragma once
#include <cstdint>

#include "diagnostics/probes/ZeroCopyImportProbe.h"
#include "vulkan/RawImportOptions.h"
namespace rawrcam::diagnostics {
inline vulkan::RawImportOptions rawImportProbeOptions(uint32_t mode) noexcept {
    return {&logZeroCopyImportEvidence, &importZeroCopyCandidates, mode == 15u};
}
}  // namespace rawrcam::diagnostics
