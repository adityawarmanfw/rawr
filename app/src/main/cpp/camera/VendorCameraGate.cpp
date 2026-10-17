#include "camera/VendorCameraGate.h"

#include <mutex>

namespace rawrcam::camera {
namespace {
std::mutex gateMutex;
JavaVM* gateVm = nullptr;
jobject javaGate = nullptr;
jmethodID acquireMethod = nullptr;
jmethodID releaseMethod = nullptr;

// Calls one of the gate's (String) methods on this thread, attaching it when needed.
template <typename Call>
bool withGateEnv(const std::string& gate, Call&& call) {
    std::lock_guard<std::mutex> lock(gateMutex);
    if (!gateVm || !javaGate) return false;
    JNIEnv* env = nullptr;
    bool attached = false;
    const auto status = gateVm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (status == JNI_EDETACHED) {
        if (gateVm->AttachCurrentThread(&env, nullptr) != JNI_OK) return false;
        attached = true;
    } else if (status != JNI_OK) {
        return false;
    }
    bool result = false;
    if (env->PushLocalFrame(2) == JNI_OK) {
        if (jstring name = env->NewStringUTF(gate.c_str()); name && !env->ExceptionCheck())
            result = call(env, name);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            result = false;
        }
        env->PopLocalFrame(nullptr);
    } else if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }
    if (attached) gateVm->DetachCurrentThread();
    return result;
}
}  // namespace

void installVendorCameraGate(JNIEnv* env, jobject gate) {
    std::lock_guard<std::mutex> lock(gateMutex);
    if (javaGate || !gate) return;  // Application scope, retained for the process lifetime.
    jclass cls = env->GetObjectClass(gate);
    if (!cls) return;
    jmethodID acquire = env->GetMethodID(cls, "acquire", "(Ljava/lang/String;)Z");
    jmethodID release = env->GetMethodID(cls, "release", "(Ljava/lang/String;)V");
    env->DeleteLocalRef(cls);
    if (!acquire || !release || env->ExceptionCheck()) {
        env->ExceptionClear();
        return;
    }
    jobject global = env->NewGlobalRef(gate);
    if (!global) return;
    env->GetJavaVM(&gateVm);
    javaGate = global;
    acquireMethod = acquire;
    releaseMethod = release;
}

bool acquireVendorCameraGate(const std::string& gate, std::string* error) {
    const bool ok = withGateEnv(gate, [](JNIEnv* env, jstring name) {
        return env->CallBooleanMethod(javaGate, acquireMethod, name) == JNI_TRUE;
    });
    if (!ok && error) *error = javaGate ? "gate refused" : "no vendor gate installed";
    return ok;
}

void releaseVendorCameraGate(const std::string& gate) {
    (void)withGateEnv(gate, [](JNIEnv* env, jstring name) {
        env->CallVoidMethod(javaGate, releaseMethod, name);
        return true;
    });
}

}  // namespace rawrcam::camera
