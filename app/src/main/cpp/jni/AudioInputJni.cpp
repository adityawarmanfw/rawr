#include <jni.h>

#include <cstdint>
#include <exception>
#include <memory>

#include "video/AudioInput.h"

namespace {
using rawrcam::video::AudioInput;

std::shared_ptr<AudioInput>* holder(jlong handle) noexcept {
    return reinterpret_cast<std::shared_ptr<AudioInput>*>(static_cast<intptr_t>(handle));
}
}  // namespace

// Holds a reference to the shared video-mode microphone; 0 if it cannot open.
extern "C" JNIEXPORT jlong JNICALL Java_com_rawr_camera_video_AudioLevelMonitor_nativeOpen(JNIEnv*, jobject,
                                                                                         jint channels) {
    try {
        return static_cast<jlong>(
            reinterpret_cast<intptr_t>(new std::shared_ptr<AudioInput>(AudioInput::acquire(channels))));
    } catch (const std::exception&) {
        return 0;
    }
}

// Fills [channels, failed, clips, peakL, rmsL, peakR, rmsR]; peaks are held since the last call.
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_video_AudioLevelMonitor_nativeLevels(JNIEnv* env, jobject,
                                                                                          jlong handle,
                                                                                          jfloatArray out) {
    auto* input = holder(handle);
    if (!input || !out || env->GetArrayLength(out) < 7) return;
    const AudioInput::Levels levels = (*input)->takeLevels();
    const jfloat values[7] = {static_cast<jfloat>(levels.channels), levels.failed ? 1.0f : 0.0f,
                              static_cast<jfloat>(levels.clips),     levels.peak[0],
                              levels.rms[0],                         levels.peak[1],
                              levels.rms[1]};
    env->SetFloatArrayRegion(out, 0, 7, values);
}

extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_video_AudioLevelMonitor_nativeClose(JNIEnv*, jobject,
                                                                                         jlong handle) {
    delete holder(handle);
}
