#pragma once
#include <cstdint>
struct AHardwareBuffer {};
struct AHardwareBuffer_Desc { uint32_t width, height, layers, format, stride; uint64_t usage; };
inline void AHardwareBuffer_describe(AHardwareBuffer*, AHardwareBuffer_Desc* d) { *d = {}; }
