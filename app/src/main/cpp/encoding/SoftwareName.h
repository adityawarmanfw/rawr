#pragma once

// EXIF/TIFF Software tag shared by capture and Renderer outputs. The
// version comes from Gradle's versionName via CMake.
#ifndef RAWR_VERSION_NAME
#define RAWR_VERSION_NAME "dev"
#endif

namespace rawrcam::encoding {
inline constexpr const char* kSoftwareName = "Rawr v" RAWR_VERSION_NAME;
}
