#pragma once
#include "vulkan/RawImportOptions.h"
namespace rawrcam::diagnostics {
// Zero-copy diagnostic evidence for the camera AHB import (diagnostic modes
// 11-15). Both act only on the zero-copy usages or advanced candidates, so
// production imports are untouched.
// Logs format, external-memory and geometry facts for the AHB.
void logZeroCopyImportEvidence(const vulkan::RawImportFacts& facts);
// Adds the alternate images of the same AHB the zero-copy probes compare:
// external-format, DRM-modifier and copy-buffer candidates (advanced), the
// TRANSFER_SRC reference alias (STORAGE-only candidate), and the LINEAR alias.
// Throws when the reference alias is required but unavailable.
void importZeroCopyCandidates(const vulkan::RawImportFacts& facts, vulkan::ImportedRaw& raw,
                              vulkan::DeviceObjects& objects);
}  // namespace rawrcam::diagnostics
