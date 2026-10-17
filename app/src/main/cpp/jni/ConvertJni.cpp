// JNI for the renderer's DNG library: probe, RAW thumbnail and DNG-to-DNG
// conversion. Paths may be /proc/self/fd/N for content URIs.
#include <jni.h>

#include <atomic>
#include <cstdint>
#include <sstream>
#include <string>

#include "convert/DngConverter.h"
#include "convert/DngProbe.h"

namespace {

struct ConvertJob {
    std::atomic<uint32_t> permille{0};
    std::atomic<bool> cancelled{false};
};

std::string str(JNIEnv* e, jstring s) {
    const char* chars = e->GetStringUTFChars(s, nullptr);
    std::string out(chars ? chars : "");
    if (chars) e->ReleaseStringUTFChars(s, chars);
    return out;
}

ConvertJob* job(jlong handle) { return reinterpret_cast<ConvertJob*>(handle); }

}  // namespace

// "ok\nwidth\nheight\nbits\ncompression\norientation\nrawr\nconvertible\nmake\nmodel\ndateTimeOriginal\noffsetTimeOriginal"
// or "error\n<message>".
extern "C" JNIEXPORT jstring JNICALL Java_com_rawr_camera_renderer_library_ConvertNative_probe(JNIEnv* e, jobject,
                                                                                                jstring path) {
    const auto info = rawrcam::convert::probeDng(str(e, path));
    std::ostringstream out;
    if (!info.ok) {
        out << "error\n" << info.error;
    } else {
        auto clean = [](std::string s) {
            for (char& c : s)
                if (c == '\n' || c == '\r') c = ' ';
            return s;
        };
        out << "ok\n"
            << info.width << '\n'
            << info.height << '\n'
            << info.bitsPerSample << '\n'
            << info.compression << '\n'
            << info.orientation << '\n'
            << (info.rawr ? 1 : 0) << '\n'
            << (info.convertible ? 1 : 0) << '\n'
            << clean(info.make) << '\n'
            << clean(info.model) << '\n'
            << clean(info.dateTimeOriginal) << '\n'
            << clean(info.offsetTimeOriginal);
    }
    return e->NewStringUTF(out.str().c_str());
}

// [width, height, argb...] unrotated, or null.
extern "C" JNIEXPORT jintArray JNICALL
Java_com_rawr_camera_renderer_library_ConvertNative_thumbnail(JNIEnv* e, jobject, jstring path, jint maxSide) {
    uint32_t w = 0, h = 0;
    const auto rgb = rawrcam::convert::thumbnailDng(str(e, path), uint32_t(maxSide), &w, &h);
    if (rgb.empty()) return nullptr;
    const jsize n = jsize(2 + size_t(w) * h);
    jintArray result = e->NewIntArray(n);
    if (!result) return nullptr;
    jint* out = e->GetIntArrayElements(result, nullptr);
    out[0] = jint(w);
    out[1] = jint(h);
    for (size_t i = 0; i < size_t(w) * h; ++i)
        out[2 + i] = jint(0xff000000u | (uint32_t(rgb[i * 3]) << 16) | (uint32_t(rgb[i * 3 + 1]) << 8) | rgb[i * 3 + 2]);
    e->ReleaseIntArrayElements(result, out, 0);
    return result;
}

extern "C" JNIEXPORT jlong JNICALL Java_com_rawr_camera_renderer_library_ConvertNative_newJob(JNIEnv*, jobject) {
    return reinterpret_cast<jlong>(new ConvertJob());
}

extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_renderer_library_ConvertNative_releaseJob(JNIEnv*, jobject,
                                                                                                  jlong handle) {
    delete job(handle);
}

extern "C" JNIEXPORT jint JNICALL Java_com_rawr_camera_renderer_library_ConvertNative_progress(JNIEnv*, jobject,
                                                                                                jlong handle) {
    return jint(job(handle)->permille.load());
}

extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_renderer_library_ConvertNative_cancel(JNIEnv*, jobject,
                                                                                              jlong handle) {
    job(handle)->cancelled = true;
}

// mode: 0 = 200MP lossless, 1 = 50MP, 2 = 25MP, 3 = 12.5MP (of the source pixel count).
// Returns "" on success, "cancelled", or an error message.
extern "C" JNIEXPORT jstring JNICALL Java_com_rawr_camera_renderer_library_ConvertNative_convert(
    JNIEnv* e, jobject, jlong handle, jstring source, jstring destination, jint mode, jstring offsetTime) {
    using namespace rawrcam::convert;
    ConvertJob* j = job(handle);
    j->permille = 0;
    const auto progress = [j](uint32_t done, uint32_t total) { j->permille = total ? done * 1000u / total : 0u; };
    const auto cancelled = [j] { return j->cancelled.load(); };
    const std::string src = str(e, source), dst = str(e, destination);
    ConvertOptions options;
    options.offsetTime = str(e, offsetTime);
    ConvertResult result;
    switch (mode) {
        case 0: result = recompressDng(src, dst, progress, cancelled, options); break;
        case 1: result = downscaleDng(src, dst, DngDownscale::Quarter, progress, cancelled, options); break;
        case 2: result = downscaleDng(src, dst, DngDownscale::Eighth, progress, cancelled, options); break;
        case 3: result = downscaleDng(src, dst, DngDownscale::Sixteenth, progress, cancelled, options); break;
        default: result.error = "Unknown conversion";
    }
    return e->NewStringUTF(result.ok ? "" : result.cancelled ? "cancelled" : result.error.c_str());
}
