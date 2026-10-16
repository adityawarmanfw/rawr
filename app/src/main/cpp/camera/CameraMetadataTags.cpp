#include "CameraMetadataTags.h"

#include <dlfcn.h>
#include <mutex>

namespace rawrcam::camera {
namespace {
std::mutex resolverMutex;
JavaVM* resolverVm = nullptr;
jobject javaResolver = nullptr;
jmethodID resolveMethod = nullptr;

std::optional<uint32_t> resolveLegacy(const char* cameraId, const char* name, std::string* error) {
    std::lock_guard<std::mutex> lock(resolverMutex);
    if (error) *error = "Camera2 name lookup unavailable; use a verified numeric tag ID";
    if (!resolverVm || !javaResolver || !resolveMethod) return std::nullopt;
    JNIEnv* env = nullptr;
    bool attached = false;
    const auto status = resolverVm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (status == JNI_EDETACHED) {
        if (resolverVm->AttachCurrentThread(&env, nullptr) != JNI_OK) return std::nullopt;
        attached = true;
    } else if (status != JNI_OK) return std::nullopt;
    jlong tag = -1;
    if (env->PushLocalFrame(4) == JNI_OK) {
        auto id = env->NewStringUTF(cameraId);
        auto key = env->NewStringUTF(name);
        if (id && key && !env->ExceptionCheck())
            tag = env->CallLongMethod(javaResolver, resolveMethod, id, key);
        if (env->ExceptionCheck()) { env->ExceptionClear(); tag = -1; }
        env->PopLocalFrame(nullptr);
    } else if (env->ExceptionCheck()) env->ExceptionClear();
    if (attached) resolverVm->DetachCurrentThread();
    if (tag < 0 || tag > UINT32_MAX) return std::nullopt;
    if (error) error->clear();
    return static_cast<uint32_t>(tag);
}
using GetTagFromNameFn = camera_status_t (*)(const ACameraMetadata*, const char*, uint32_t*);
}

void installCameraTagResolver(JNIEnv* env, jobject resolver) {
    std::lock_guard<std::mutex> lock(resolverMutex);
    if (javaResolver) return; // Application context only, retained for the process lifetime.
    if (!resolver) return;
    auto cls = env->GetObjectClass(resolver);
    if (!cls) return;
    auto method = env->GetMethodID(cls, "resolve", "(Ljava/lang/String;Ljava/lang/String;)J");
    env->DeleteLocalRef(cls);
    if (!method || env->ExceptionCheck()) { env->ExceptionClear(); return; }
    auto global = env->NewGlobalRef(resolver);
    if (!global) return;
    env->GetJavaVM(&resolverVm);
    javaResolver = global;
    resolveMethod = method;
}

std::optional<uint32_t> resolveMetadataTagByNameCompat(const ACameraMetadata* metadata, const char* cameraId, const char* name,
                                                       std::string* error) {
    void* handle = dlopen("libcamera2ndk.so", RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        if (error) *error = "libcamera2ndk.so unavailable";
        return std::nullopt;
    }
    auto* fn = reinterpret_cast<GetTagFromNameFn>(dlsym(handle, "ACameraMetadata_getTagFromName"));
    if (!fn) {
        dlclose(handle);
        return resolveLegacy(cameraId, name, error);
    }
    uint32_t tag = 0;
    const camera_status_t status = fn(metadata, name, &tag);
    dlclose(handle);
    if (status != ACAMERA_OK) {
        if (error) *error = "vendor tag lookup failed status=" + std::to_string(status);
        return std::nullopt;
    }
    return tag;
}

bool containsI32Tag(const ACameraMetadata* metadata, uint32_t listTag, uint32_t wanted) {
    ACameraMetadata_const_entry e{};
    if (ACameraMetadata_getConstEntry(metadata, listTag, &e) != ACAMERA_OK || !e.data.i32) return false;
    for (uint32_t i = 0; i < e.count; ++i) {
        if (static_cast<uint32_t>(e.data.i32[i]) == wanted) return true;
    }
    return false;
}
}  // namespace rawrcam::camera
