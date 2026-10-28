#include "camera/JavaCameraDriver.h"

#include <android/log.h>
#include <android/native_window_jni.h>
#include <camera/NdkCameraError.h>
#include <camera/NdkCameraMetadata.h>

#include <atomic>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>

namespace rawrcam::camera {
namespace {
constexpr const char* kTag = "RawrJavaCamera";

struct Bridge {
    JavaVM* vm = nullptr;
    jobject backend = nullptr;  // JavaCameraBackend
    jmethodID open = nullptr;
    // JavaCameraDevice
    jmethodID deviceStatus = nullptr, deviceError = nullptr, deviceConfigure = nullptr, deviceClose = nullptr;
    // JavaCameraSession
    jmethodID setRepeating = nullptr, capture = nullptr, stopRepeating = nullptr, sessionClose = nullptr,
              sessionError = nullptr;
    jclass surfaceClass = nullptr, stringClass = nullptr, byteArrayClass = nullptr;
};
std::mutex gBridgeMutex;
Bridge gBridge;

// Attaches the calling thread for the rest of its life; detaches when it exits.
JNIEnv* env() {
    struct Attachment {
        JNIEnv* env = nullptr;
        bool attached = false;
        ~Attachment() {
            if (attached && gBridge.vm) gBridge.vm->DetachCurrentThread();
        }
    };
    thread_local Attachment attachment;
    if (attachment.env) return attachment.env;
    JNIEnv* e = nullptr;
    const jint status = gBridge.vm->GetEnv(reinterpret_cast<void**>(&e), JNI_VERSION_1_6);
    if (status == JNI_EDETACHED) {
        if (gBridge.vm->AttachCurrentThread(&e, nullptr) != JNI_OK) return nullptr;
        attachment.attached = true;
    } else if (status != JNI_OK) {
        return nullptr;
    }
    attachment.env = e;
    return e;
}

bool clearException(JNIEnv* e, const char* what) {
    if (!e->ExceptionCheck()) return false;
    __android_log_print(ANDROID_LOG_WARN, kTag, "exception in %s", what);
    e->ExceptionDescribe();
    e->ExceptionClear();
    return true;
}

std::string javaString(JNIEnv* e, jstring s) {
    if (!s) return {};
    const char* chars = e->GetStringUTFChars(s, nullptr);
    std::string out = chars ? chars : "";
    if (chars) e->ReleaseStringUTFChars(s, chars);
    return out;
}

// One request as bytes for JavaCameraSession (little-endian): template, request id, target output indices, then
// per entry tag, type, count, name (empty: the camera's own key for the tag) and the raw values.
std::vector<uint8_t> pack(const DataRequest& r, int64_t requestId, const std::vector<ANativeWindow*>& windows,
                          bool* unknownTarget) {
    std::vector<uint8_t> out;
    const auto put = [&](const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        out.insert(out.end(), b, b + n);
    };
    const auto putI32 = [&](int32_t v) { put(&v, 4); };
    putI32(r.requestTemplate());
    put(&requestId, 8);
    std::vector<int32_t> targets;
    for (auto* w : r.targets()) {
        int32_t index = -1;
        for (size_t i = 0; i < windows.size(); ++i)
            if (windows[i] == w) index = int32_t(i);
        if (index < 0) *unknownTarget = true;
        else targets.push_back(index);
    }
    putI32(int32_t(targets.size()));
    for (int32_t t : targets) putI32(t);
    putI32(int32_t(r.entries().size()));
    for (const auto& [tag, value] : r.entries()) {
        putI32(int32_t(tag));
        putI32(int32_t(value.type));
        putI32(int32_t(value.count));
        const auto name = r.tagNames().find(tag);
        const std::string n = name == r.tagNames().end() ? std::string() : name->second;
        putI32(int32_t(n.size()));
        put(n.data(), n.size());
        const uint32_t bytes = value.count * requestTypeSize(value.type);
        putI32(int32_t(bytes));
        put(value.words.data(), bytes);
    }
    return out;
}

jbyteArray toByteArray(JNIEnv* e, const std::vector<uint8_t>& bytes) {
    jbyteArray array = e->NewByteArray(jsize(bytes.size()));
    if (array) e->SetByteArrayRegion(array, 0, jsize(bytes.size()), reinterpret_cast<const jbyte*>(bytes.data()));
    return array;
}

class JavaSessionDriver final : public CameraSessionDriver {
   public:
    JavaSessionDriver(std::vector<ANativeWindow*> windows, CameraSessionCallbackContext* context)
        : windows_(std::move(windows)), context_(context) {}
    ~JavaSessionDriver() override {
        if (session_)
            if (JNIEnv* e = env()) e->DeleteGlobalRef(session_);
    }
    void attach(jobject session) { session_ = session; }
    RequestStatus setRepeating(CameraRequest& request, int* sequenceId) override {
        return submit({&request}, true, sequenceId);
    }
    RequestStatus capture(const std::vector<CameraRequest*>& requests, int* sequenceId) override {
        return submit(requests, false, sequenceId);
    }
    RequestStatus stopRepeating() override {
        JNIEnv* e = env();
        if (!e || !session_) return ACAMERA_ERROR_SESSION_CLOSED;
        const jint s = e->CallIntMethod(session_, gBridge.stopRepeating);
        return clearException(e, "stopRepeating") ? ACAMERA_ERROR_UNKNOWN : s;
    }
    void close() override {
        if (closed_) return;
        closed_ = true;
        JNIEnv* e = env();
        if (!e || !session_) return;
        e->CallVoidMethod(session_, gBridge.sessionClose);
        clearException(e, "closeSession");
    }
    const void* identity() const override { return this; }
    CameraSessionCallbackContext* context() const { return context_; }

    // The request a callback is about; captures are forgotten once they completed or failed.
    std::shared_ptr<const DataRequest> find(int64_t id, bool finished) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = submitted_.find(id);
        if (it == submitted_.end()) return nullptr;
        auto request = it->second.request;
        if (finished && !it->second.repeating) submitted_.erase(it);
        return request;
    }

   private:
    struct Submitted {
        std::shared_ptr<const DataRequest> request;
        bool repeating = false;
    };
    RequestStatus submit(const std::vector<CameraRequest*>& requests, bool repeating, int* sequenceId) {
        JNIEnv* e = env();
        if (!e || !session_) return ACAMERA_ERROR_SESSION_CLOSED;
        if (e->PushLocalFrame(int(requests.size()) + 8) != JNI_OK) return ACAMERA_ERROR_NOT_ENOUGH_MEMORY;
        std::vector<int64_t> ids;
        jobjectArray packed = e->NewObjectArray(jsize(requests.size()), gBridge.byteArrayClass, nullptr);
        bool unknownTarget = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (size_t i = 0; packed && i < requests.size(); ++i) {
                const auto* data = dynamic_cast<const DataRequest*>(requests[i]);
                if (!data) {
                    e->PopLocalFrame(nullptr);
                    return ACAMERA_ERROR_INVALID_PARAMETER;
                }
                const int64_t id = nextId_++;
                ids.push_back(id);
                submitted_[id] = {std::make_shared<const DataRequest>(*data), repeating};
                if (repeating) {
                    repeatingIds_.push_back(id);
                    // Frames of a replaced repeating request can still complete for a while.
                    while (repeatingIds_.size() > 16) {
                        submitted_.erase(repeatingIds_.front());
                        repeatingIds_.pop_front();
                    }
                }
                e->SetObjectArrayElement(packed, jsize(i), toByteArray(e, pack(*data, id, windows_, &unknownTarget)));
            }
        }
        jint result = ACAMERA_ERROR_INVALID_PARAMETER;
        if (packed && !unknownTarget) {
            result = repeating ? e->CallIntMethod(session_, gBridge.setRepeating, e->GetObjectArrayElement(packed, 0))
                               : e->CallIntMethod(session_, gBridge.capture, packed);
            if (clearException(e, repeating ? "setRepeating" : "capture")) result = ACAMERA_ERROR_UNKNOWN;
        }
        if (result < 0) {
            const std::string error = javaString(e, static_cast<jstring>(e->CallObjectMethod(session_, gBridge.sessionError)));
            clearException(e, "sessionError");
            __android_log_print(ANDROID_LOG_WARN, kTag, "submit failed status=%d %s%s", result, error.c_str(),
                                unknownTarget ? " target_not_configured" : "");
            std::lock_guard<std::mutex> lock(mutex_);
            for (int64_t id : ids) submitted_.erase(id);
        } else if (sequenceId) {
            *sequenceId = result;
        }
        e->PopLocalFrame(nullptr);
        return result < 0 ? result : ACAMERA_OK;
    }
    jobject session_ = nullptr;
    std::vector<ANativeWindow*> windows_;
    CameraSessionCallbackContext* context_;
    bool closed_ = false;
    std::mutex mutex_;
    int64_t nextId_ = 1;
    std::map<int64_t, Submitted> submitted_;
    std::deque<int64_t> repeatingIds_;
};

class JavaDeviceDriver final : public CameraDeviceDriver {
   public:
    explicit JavaDeviceDriver(jobject device) : device_(device) {}
    ~JavaDeviceDriver() override {
        close();
        if (JNIEnv* e = env()) e->DeleteGlobalRef(device_);
    }
    const char* backendName() const override { return "java"; }
    std::unique_ptr<CameraRequest> createRequest(RequestTemplate t, RequestStatus* status) override {
        if (status) *status = ACAMERA_OK;
        return std::make_unique<DataRequest>(int32_t(t));
    }
    std::unique_ptr<CameraSessionDriver> createSession(const std::vector<SessionOutput>& outputs,
                                                       const CameraRequest* sessionParameters, int32_t opMode,
                                                       CameraSessionCallbackContext* context,
                                                       RequestStatus* status) override {
        const auto failed = [&](RequestStatus s) -> std::unique_ptr<CameraSessionDriver> {
            if (status) *status = s;
            return nullptr;
        };
        JNIEnv* e = env();
        if (!e || closed_) return failed(ACAMERA_ERROR_CAMERA_DISCONNECTED);
        if (e->PushLocalFrame(int(outputs.size()) * 2 + 16) != JNI_OK) return failed(ACAMERA_ERROR_NOT_ENOUGH_MEMORY);
        const jsize n = jsize(outputs.size());
        jobjectArray surfaces = e->NewObjectArray(n, gBridge.surfaceClass, nullptr);
        jobjectArray physical = e->NewObjectArray(n, gBridge.stringClass, nullptr);
        jlongArray useCases = e->NewLongArray(n);
        std::vector<ANativeWindow*> windows;
        for (jsize i = 0; surfaces && physical && useCases && i < n; ++i) {
            const auto& o = outputs[size_t(i)];
            windows.push_back(o.window);
            e->SetObjectArrayElement(surfaces, i, ANativeWindow_toSurface(e, o.window));
            if (!o.physicalCameraId.empty())
                e->SetObjectArrayElement(physical, i, e->NewStringUTF(o.physicalCameraId.c_str()));
            const jlong useCase = o.streamUseCase;
            e->SetLongArrayRegion(useCases, i, 1, &useCase);
        }
        jbyteArray params = nullptr;
        if (const auto* data = dynamic_cast<const DataRequest*>(sessionParameters)) {
            bool unknownTarget = false;
            params = toByteArray(e, pack(*data, 0, windows, &unknownTarget));
        }
        auto session = std::make_unique<JavaSessionDriver>(windows, context);
        jobject created = clearException(e, "configure arguments")
                              ? nullptr
                              : e->CallObjectMethod(device_, gBridge.deviceConfigure, surfaces, physical, useCases,
                                                    jint(opMode), params, reinterpret_cast<jlong>(session.get()));
        if (clearException(e, "configure")) created = nullptr;
        RequestStatus s = ACAMERA_OK;
        if (!created) {
            s = e->CallIntMethod(device_, gBridge.deviceStatus);
            if (clearException(e, "deviceStatus") || s == ACAMERA_OK) s = ACAMERA_ERROR_STREAM_CONFIGURE_FAIL;
            const std::string error = javaString(e, static_cast<jstring>(e->CallObjectMethod(device_, gBridge.deviceError)));
            clearException(e, "deviceError");
            __android_log_print(ANDROID_LOG_WARN, kTag, "configure failed status=%d %s", s, error.c_str());
        } else {
            session->attach(e->NewGlobalRef(created));
        }
        e->PopLocalFrame(nullptr);
        if (!created) return failed(s);
        if (status) *status = ACAMERA_OK;
        return session;
    }
    void close() override {
        if (closed_) return;
        closed_ = true;
        JNIEnv* e = env();
        if (!e) return;
        e->CallVoidMethod(device_, gBridge.deviceClose);
        clearException(e, "closeDevice");
    }

   private:
    jobject device_;
    std::atomic<bool> closed_{false};
};

JavaSessionDriver* sessionOf(jlong handle) { return reinterpret_cast<JavaSessionDriver*>(handle); }
}  // namespace

void installJavaCameraBackend(JNIEnv* e, jobject backend) {
    std::lock_guard<std::mutex> lock(gBridgeMutex);
    if (gBridge.backend || !backend) return;  // application-scoped, kept for the process
    Bridge b;
    e->GetJavaVM(&b.vm);
    const auto global = [&](const char* name) {
        jclass local = e->FindClass(name);
        if (!local) return jclass(nullptr);
        auto g = static_cast<jclass>(e->NewGlobalRef(local));
        e->DeleteLocalRef(local);
        return g;
    };
    jclass backendClass = e->GetObjectClass(backend);
    jclass deviceClass = e->FindClass("com/rawr/camera/integration/JavaCameraDevice");
    jclass sessionClass = e->FindClass("com/rawr/camera/integration/JavaCameraSession");
    if (!backendClass || !deviceClass || !sessionClass) {
        clearException(e, "install classes");
        return;
    }
    b.open = e->GetMethodID(backendClass, "open", "(Ljava/lang/String;J)Lcom/rawr/camera/integration/JavaCameraDevice;");
    b.deviceStatus = e->GetMethodID(deviceClass, "status", "()I");
    b.deviceError = e->GetMethodID(deviceClass, "error", "()Ljava/lang/String;");
    b.deviceConfigure = e->GetMethodID(deviceClass, "configure",
                                       "([Landroid/view/Surface;[Ljava/lang/String;[JI[BJ)Lcom/rawr/camera/integration/JavaCameraSession;");
    b.deviceClose = e->GetMethodID(deviceClass, "close", "()V");
    b.setRepeating = e->GetMethodID(sessionClass, "setRepeating", "([B)I");
    b.capture = e->GetMethodID(sessionClass, "capture", "([[B)I");
    b.stopRepeating = e->GetMethodID(sessionClass, "stopRepeating", "()I");
    b.sessionClose = e->GetMethodID(sessionClass, "close", "()V");
    b.sessionError = e->GetMethodID(sessionClass, "error", "()Ljava/lang/String;");
    b.surfaceClass = global("android/view/Surface");
    b.stringClass = global("java/lang/String");
    b.byteArrayClass = global("[B");
    e->DeleteLocalRef(backendClass);
    e->DeleteLocalRef(deviceClass);
    e->DeleteLocalRef(sessionClass);
    if (clearException(e, "install methods") || !b.open || !b.surfaceClass || !b.stringClass || !b.byteArrayClass) return;
    b.backend = e->NewGlobalRef(backend);
    gBridge = b;
}

bool javaCameraBackendInstalled() {
    std::lock_guard<std::mutex> lock(gBridgeMutex);
    return gBridge.backend != nullptr;
}

std::unique_ptr<CameraDeviceDriver> openJavaCamera(const std::string& cameraId, CameraDeviceCallbackContext* context,
                                                   RequestStatus* status, std::string* error) {
    const auto failed = [&](RequestStatus s, const std::string& why) -> std::unique_ptr<CameraDeviceDriver> {
        if (status) *status = s;
        if (error) *error = why;
        return nullptr;
    };
    if (!javaCameraBackendInstalled()) return failed(ACAMERA_ERROR_UNSUPPORTED_OPERATION, "not_installed");
    JNIEnv* e = env();
    if (!e) return failed(ACAMERA_ERROR_UNKNOWN, "no_jni_env");
    jstring id = e->NewStringUTF(cameraId.c_str());
    jobject device = e->CallObjectMethod(gBridge.backend, gBridge.open, id, reinterpret_cast<jlong>(context));
    e->DeleteLocalRef(id);
    if (clearException(e, "open") || !device) return failed(ACAMERA_ERROR_UNKNOWN, "exception");
    const jint s = e->CallIntMethod(device, gBridge.deviceStatus);
    if (clearException(e, "deviceStatus") || s != ACAMERA_OK) {
        const std::string why = javaString(e, static_cast<jstring>(e->CallObjectMethod(device, gBridge.deviceError)));
        clearException(e, "deviceError");
        e->DeleteLocalRef(device);
        return failed(s != ACAMERA_OK ? s : ACAMERA_ERROR_UNKNOWN, why);
    }
    auto driver = std::make_unique<JavaDeviceDriver>(e->NewGlobalRef(device));
    e->DeleteLocalRef(device);
    if (status) *status = ACAMERA_OK;
    return driver;
}

}  // namespace rawrcam::camera

using rawrcam::camera::CameraCallbacks;
using rawrcam::camera::CameraDeviceCallbackContext;

extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_JavaCameraBackend_nativeOnDeviceError(
    JNIEnv*, jclass, jlong device, jint error, jboolean disconnected) {
    auto* context = reinterpret_cast<CameraDeviceCallbackContext*>(device);
    if (!context) return;
    if (disconnected) CameraCallbacks::deviceDisconnected(context);
    else CameraCallbacks::deviceError(context, error);
}

extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_JavaCameraBackend_nativeOnSessionClosed(JNIEnv*, jclass,
                                                                                                         jlong session) {
    auto* s = rawrcam::camera::sessionOf(session);
    if (s) CameraCallbacks::sessionClosed(s->context(), s);
}

extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_JavaCameraBackend_nativeOnCaptureStarted(
    JNIEnv*, jclass, jlong session, jlong requestId, jlong timestamp) {
    auto* s = rawrcam::camera::sessionOf(session);
    if (!s) return;
    const auto request = s->find(requestId, false);
    CameraCallbacks::captureStarted(s->context(), request.get(), timestamp);
}

extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_JavaCameraBackend_nativeOnCaptureCompleted(
    JNIEnv* env, jclass, jlong session, jlong requestId, jobject result) {
    auto* s = rawrcam::camera::sessionOf(session);
    if (!s || !result) return;
    const auto request = s->find(requestId, true);
    ACameraMetadata* metadata = ACameraMetadata_fromCameraMetadata(env, result);
    if (!metadata) return;
    CameraCallbacks::captureCompleted(s->context(), request.get(), metadata);
    ACameraMetadata_free(metadata);
}

extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_JavaCameraBackend_nativeOnCaptureFailed(
    JNIEnv*, jclass, jlong session, jlong requestId, jlong frameNumber, jint reason, jint sequenceId,
    jboolean bufferLost) {
    auto* s = rawrcam::camera::sessionOf(session);
    if (!s) return;
    const auto request = s->find(requestId, !bufferLost);
    CameraCallbacks::captureFailed(s->context(), bufferLost ? "CAMERA_BUFFER_LOST" : "CAMERA_CAPTURE_FAILED",
                                   frameNumber, bufferLost ? 0 : reason, bufferLost ? -1 : sequenceId, request.get());
}

extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_JavaCameraBackend_nativeOnSequenceEnded(
    JNIEnv*, jclass, jlong session, jint sequenceId, jboolean aborted) {
    auto* s = rawrcam::camera::sessionOf(session);
    if (s) CameraCallbacks::sequenceEnded(s->context(), sequenceId, aborted);
}
