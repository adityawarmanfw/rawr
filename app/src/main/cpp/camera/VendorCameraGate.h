#pragma once

#include <jni.h>

#include <string>

namespace rawrcam::camera {

// Vendor state some lens routes need while their camera device is open (for
// example vivo's full-size RAW permission, LensRoute::vendorGate). The app
// owns the state: it saves the values it finds before changing them and
// restores them on release, also after a crash. Acquire runs right before the
// device opens; release runs once the device has closed.
void installVendorCameraGate(JNIEnv* env, jobject gate);

// False when no gate is installed, the gate is unknown, or the app could not
// take it; the route must not open then.
[[nodiscard]] bool acquireVendorCameraGate(const std::string& gate, std::string* error);
// Safe from any thread (device closes run detached).
void releaseVendorCameraGate(const std::string& gate);

}  // namespace rawrcam::camera
