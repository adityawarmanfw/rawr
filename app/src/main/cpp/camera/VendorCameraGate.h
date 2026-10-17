#pragma once

#include <jni.h>

#include <string>

namespace rawrcam::camera {

// Vendor state some lens routes need while their streams are configured (for
// example vivo's full-size RAW permission, LensRoute::vendorGate). The app
// owns the state: it saves the value it finds before changing it and restores
// it on release, also after a crash. Acquire runs before the device opens (and
// before any later session on it); release runs as soon as the session has
// been configured, or when the device closes if configuration never happened.
void installVendorCameraGate(JNIEnv* env, jobject gate);

// False when no gate is installed, the gate is unknown, or the app could not
// take it; the route must not open then.
[[nodiscard]] bool acquireVendorCameraGate(const std::string& gate, std::string* error);
// Safe from any thread (device closes run detached).
void releaseVendorCameraGate(const std::string& gate);

}  // namespace rawrcam::camera
