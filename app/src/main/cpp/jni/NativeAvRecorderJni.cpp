#include <android/native_window_jni.h>
#include <jni.h>
#include <unistd.h>

#include <cstdint>
#include <exception>
#include <memory>
#include <string>

#include "video/NativeAvRecorder.h"

namespace {
using rawrcam::video::NativeAvRecorder;

NativeAvRecorder* recorder(jlong handle) noexcept {
    return reinterpret_cast<NativeAvRecorder*>(static_cast<intptr_t>(handle));
}

std::string javaString(JNIEnv* env, jstring value) {
    if (!value) return {};
    const char* chars = env->GetStringUTFChars(value, nullptr);
    std::string out = chars ? chars : "";
    if (chars) env->ReleaseStringUTFChars(value, chars);
    return out;
}

void throwJava(JNIEnv* env, const char* message) {
    jclass type = env->FindClass("java/lang/IllegalStateException");
    if (type) env->ThrowNew(type, message);
}
}  // namespace

extern "C" JNIEXPORT jlong JNICALL Java_com_rawr_camera_video_NativeAvEngine_nativeStart(
    JNIEnv* env, jobject, jint fd, jint width, jint height, jint fps, jint bitrate, jint intraFrames,
    jint rotationDegrees, jint bitrateMode, jint maxBFrames, jint audioChannels, jint audioBitrate, jint bitDepth,
    jint timestampPolicy, jstring deviceMake, jstring deviceModel, jstring software, jstring renderProfile, jstring gamut, jstring transfer, jboolean log) {
    bool ownsFd = false;
    try {
        NativeAvRecorder::Settings settings{};
        settings.width = width;
        settings.height = height;
        settings.fps = fps;
        settings.bitrate = bitrate;
        settings.intraFrames = intraFrames;
        settings.rotationDegrees = rotationDegrees;
        settings.bitrateMode = bitrateMode;
        settings.maxBFrames = maxBFrames;
        settings.audioChannels = audioChannels;
        settings.audioBitrate = audioBitrate;
        settings.bitDepth = bitDepth;
        settings.deviceMake = javaString(env, deviceMake);
        settings.deviceModel = javaString(env, deviceModel);
        settings.software = javaString(env, software);
        settings.renderProfile = javaString(env, renderProfile);
        settings.gamut = javaString(env, gamut);
        settings.transfer = javaString(env, transfer);
        settings.log = log;
        settings.timestampPolicy = static_cast<NativeAvRecorder::TimestampPolicy>(timestampPolicy);
        auto instance = std::make_unique<NativeAvRecorder>(fd, settings);
        ownsFd = true;
        instance->start();
        return static_cast<jlong>(reinterpret_cast<intptr_t>(instance.release()));
    } catch (const std::exception& error) {
        if (!ownsFd && fd >= 0) close(fd);
        throwJava(env, error.what());
        return 0;
    }
}

extern "C" JNIEXPORT jintArray JNICALL
Java_com_rawr_camera_video_NativeAvEngine_nativeSupportedTimestampPolicies(JNIEnv* env, jobject) {
    constexpr auto realTime = NativeAvRecorder::TimestampPolicy::PreserveRealtime;
    constexpr auto repeat = NativeAvRecorder::TimestampPolicy::FixedCadenceRepeat;
    jint values[2]{};
    jsize count = 0;
    if (NativeAvRecorder::supportsTimestampPolicy(realTime)) values[count++] = static_cast<jint>(realTime);
    if (NativeAvRecorder::supportsTimestampPolicy(repeat)) values[count++] = static_cast<jint>(repeat);
    jintArray result = env->NewIntArray(count);
    if (result) env->SetIntArrayRegion(result, 0, count, values);
    return result;
}

extern "C" JNIEXPORT jobject JNICALL Java_com_rawr_camera_video_NativeAvEngine_nativeSurface(JNIEnv* env, jobject,
                                                                                             jlong handle) {
    NativeAvRecorder* instance = recorder(handle);
    if (!instance || !instance->inputWindow()) {
        throwJava(env, "native video surface unavailable");
        return nullptr;
    }
    return ANativeWindow_toSurface(env, instance->inputWindow());
}

extern "C" JNIEXPORT jstring JNICALL Java_com_rawr_camera_video_NativeAvEngine_nativeStats(JNIEnv* env, jobject,
                                                                                           jlong handle) {
    NativeAvRecorder* instance = recorder(handle);
    if (!instance) {
        throwJava(env, "native recorder handle is null");
        return nullptr;
    }
    const std::string stats = instance->statsJson();
    return env->NewStringUTF(stats.c_str());
}

extern "C" JNIEXPORT jstring JNICALL Java_com_rawr_camera_video_NativeAvEngine_nativeStop(JNIEnv* env, jobject,
                                                                                          jlong handle) {
    std::unique_ptr<NativeAvRecorder> instance(recorder(handle));
    if (!instance) {
        throwJava(env, "native recorder handle is null");
        return nullptr;
    }
    const bool ok = instance->stop();
    const std::string result =
        std::string("{\"ok\":") + (ok ? "true" : "false") + ",\"stats\":" + instance->statsJson() + "}";
    return env->NewStringUTF(result.c_str());
}
