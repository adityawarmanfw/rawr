#pragma once

// EXIF/TIFF Software tag shared by capture and Renderer outputs. The
// version comes from Gradle's versionName via CMake.
#ifndef RAWR_VERSION_NAME
#define RAWR_VERSION_NAME "dev"
#endif

namespace rawrcam::encoding {
inline constexpr const char* kSoftwareName = "Rawr v" RAWR_VERSION_NAME;
// First line of every ImageDescription; the Renderer swaps the verb.
inline constexpr const char* kCapturedWith = "Captured with Rawr v" RAWR_VERSION_NAME;
inline constexpr const char* kRenderedWith = "Rendered with Rawr v" RAWR_VERSION_NAME;
}
