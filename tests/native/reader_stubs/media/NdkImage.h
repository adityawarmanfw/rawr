#pragma once
#include <cstdint>
#include <android/hardware_buffer.h>
using media_status_t = int;
inline constexpr int AMEDIA_OK = 0, AMEDIA_IMGREADER_MAX_IMAGES_ACQUIRED = -1;
inline constexpr int AIMAGE_FORMAT_RAW10 = 37, AIMAGE_FORMAT_RAW16 = 32;
struct AImageReader;
struct AImage { AImageReader* reader; int64_t timestamp; AHardwareBuffer buffer; };
void AImage_delete(AImage*);
inline int AImage_getTimestamp(AImage* i, int64_t* v) { *v = i->timestamp; return 0; }
inline int AImage_getHardwareBuffer(AImage* i, AHardwareBuffer** v) { *v = &i->buffer; return 0; }
inline int AImage_getFormat(AImage*, int32_t* v) { *v = AIMAGE_FORMAT_RAW16; return 0; }
inline int AImage_getWidth(AImage*, int32_t* v) { *v = 16; return 0; }
inline int AImage_getHeight(AImage*, int32_t* v) { *v = 12; return 0; }
inline int AImage_getNumberOfPlanes(AImage*, int32_t* v) { *v = 0; return 0; }
inline int AImage_getPlaneRowStride(AImage*, int, int32_t*) { return -1; }
inline int AImage_getPlanePixelStride(AImage*, int, int32_t*) { return -1; }
