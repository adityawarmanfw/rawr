#pragma once
#include <camera/NdkCameraMetadata.h>
#include <camera/NdkCaptureRequest.h>

#include <cstdint>
#include <jni.h>
#include <optional>
#include <string>

namespace rawrcam::camera {

void installCameraTagResolver(JNIEnv* env, jobject resolver);

std::optional<uint32_t> resolveMetadataTagByNameCompat(const ACameraMetadata* metadata, const char* cameraId, const char* name,
                                                       std::string* error);

bool containsI32Tag(const ACameraMetadata* metadata, uint32_t listTag, uint32_t wanted);

}  // namespace rawrcam::camera
