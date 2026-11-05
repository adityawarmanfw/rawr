#pragma once
#include <jni.h>

#include <memory>
#include <string>

#include "camera/CameraBackend.h"
#include "camera/CameraCallbacks.h"

namespace rawrcam::camera {

// The Java Camera2 backend: Kotlin opens the camera and runs the session (SessionConfiguration with an operating
// mode, OutputConfiguration stream use cases); native code keeps the readers, builds the requests as data and
// receives the results through ACameraMetadata_fromCameraMetadata.
void installJavaCameraBackend(JNIEnv* env, jobject backend);
bool javaCameraBackendInstalled();
// Opens cameraId through Camera2 and blocks until it is open or failed. status: an ACAMERA_* value.
std::unique_ptr<CameraDeviceDriver> openJavaCamera(const std::string& cameraId, CameraDeviceCallbackContext* context,
                                                   RequestStatus* status, std::string* error);

}  // namespace rawrcam::camera
