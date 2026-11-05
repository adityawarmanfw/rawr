// Spike for the Java camera backend: native-owned readers feed a Java Camera2 session and Java capture
// results are read back through the NDK metadata API. Debug launcher: JavaBackendSpikeActivity.
#include <android/log.h>
#include <android/native_window_jni.h>
#include <camera/NdkCameraMetadata.h>
#include <jni.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>

#include <atomic>
#include <mutex>
#include <set>
#include <string>

namespace {
struct Spike {
    AImageReader* raw = nullptr;
    AImageReader* companion = nullptr;
    std::atomic<int> rawImages{0}, companionImages{0}, results{0}, matched{0}, conversionFailures{0};
    std::mutex mu;
    std::set<int64_t> resultStamps;
    std::set<int64_t> imageStamps;
    size_t tagCount = 0;
    int64_t exposureNs = 0;
    int32_t iso = 0;
    int32_t width = 0, height = 0;
};

void onRaw(void* context, AImageReader* reader) {
    auto* s = static_cast<Spike*>(context);
    AImage* image = nullptr;
    if (AImageReader_acquireNextImage(reader, &image) != AMEDIA_OK || !image) return;
    int64_t ts = 0;
    AImage_getTimestamp(image, &ts);
    {
        std::lock_guard<std::mutex> lock(s->mu);
        s->imageStamps.insert(ts);
        if (s->resultStamps.count(ts)) s->matched++;
    }
    s->rawImages++;
    AImage_delete(image);
}

void onCompanion(void* context, AImageReader* reader) {
    auto* s = static_cast<Spike*>(context);
    AImage* image = nullptr;
    if (AImageReader_acquireNextImage(reader, &image) != AMEDIA_OK || !image) return;
    s->companionImages++;
    AImage_delete(image);
}

AImageReader* makeReader(int w, int h, int format, int maxImages, AImageReader_ImageListener listener, void* context) {
    AImageReader* reader = nullptr;
    if (AImageReader_new(w, h, format, maxImages, &reader) != AMEDIA_OK) return nullptr;
    listener.context = context;
    AImageReader_setImageListener(reader, &listener);
    return reader;
}
}  // namespace

extern "C" JNIEXPORT jlong JNICALL Java_com_rawr_camera_integration_JavaBackendSpike_nativeCreate(
    JNIEnv*, jobject, jint rawW, jint rawH, jint rawFormat, jint compW, jint compH) {
    auto* s = new Spike();
    s->width = rawW;
    s->height = rawH;
    s->raw = makeReader(rawW, rawH, rawFormat, 6, {nullptr, onRaw}, s);
    if (compW > 0) s->companion = makeReader(compW, compH, AIMAGE_FORMAT_YUV_420_888, 4, {nullptr, onCompanion}, s);
    if (!s->raw || (compW > 0 && !s->companion)) {
        if (s->raw) AImageReader_delete(s->raw);
        if (s->companion) AImageReader_delete(s->companion);
        delete s;
        return 0;
    }
    return reinterpret_cast<jlong>(s);
}

extern "C" JNIEXPORT jobject JNICALL Java_com_rawr_camera_integration_JavaBackendSpike_nativeSurface(
    JNIEnv* env, jobject, jlong handle, jboolean companion) {
    auto* s = reinterpret_cast<Spike*>(handle);
    AImageReader* reader = companion ? s->companion : s->raw;
    ANativeWindow* window = nullptr;
    if (!reader || AImageReader_getWindow(reader, &window) != AMEDIA_OK || !window) return nullptr;
    return ANativeWindow_toSurface(env, window);
}

extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_JavaBackendSpike_nativeOnResult(
    JNIEnv* env, jobject, jlong handle, jobject result) {
    auto* s = reinterpret_cast<Spike*>(handle);
    ACameraMetadata* metadata = ACameraMetadata_fromCameraMetadata(env, result);
    if (!metadata) {
        s->conversionFailures++;
        return;
    }
    ACameraMetadata_const_entry e{};
    int64_t ts = 0;
    if (ACameraMetadata_getConstEntry(metadata, ACAMERA_SENSOR_TIMESTAMP, &e) == ACAMERA_OK && e.count == 1) ts = e.data.i64[0];
    int64_t exposure = 0;
    if (ACameraMetadata_getConstEntry(metadata, ACAMERA_SENSOR_EXPOSURE_TIME, &e) == ACAMERA_OK && e.count == 1) exposure = e.data.i64[0];
    int32_t iso = 0;
    if (ACameraMetadata_getConstEntry(metadata, ACAMERA_SENSOR_SENSITIVITY, &e) == ACAMERA_OK && e.count == 1) iso = e.data.i32[0];
    int32_t tagCount = 0;
    const uint32_t* tags = nullptr;
    ACameraMetadata_getAllTags(metadata, &tagCount, &tags);
    {
        std::lock_guard<std::mutex> lock(s->mu);
        s->resultStamps.insert(ts);
        if (s->imageStamps.count(ts)) s->matched++;
        s->tagCount = static_cast<size_t>(tagCount);
        s->exposureNs = exposure;
        s->iso = iso;
    }
    s->results++;
    ACameraMetadata_free(metadata);
}

extern "C" JNIEXPORT jstring JNICALL Java_com_rawr_camera_integration_JavaBackendSpike_nativeReport(JNIEnv* env, jobject,
                                                                                                 jlong handle) {
    auto* s = reinterpret_cast<Spike*>(handle);
    std::lock_guard<std::mutex> lock(s->mu);
    std::string text = "rawImages=" + std::to_string(s->rawImages.load()) +
                       " companionImages=" + std::to_string(s->companionImages.load()) +
                       " results=" + std::to_string(s->results.load()) +
                       " imageResultMatches=" + std::to_string(s->matched.load()) +
                       " conversionFailures=" + std::to_string(s->conversionFailures.load()) +
                       " resultTags=" + std::to_string(s->tagCount) + " exposureNs=" + std::to_string(s->exposureNs) +
                       " iso=" + std::to_string(s->iso);
    return env->NewStringUTF(text.c_str());
}

extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_integration_JavaBackendSpike_nativeDestroy(JNIEnv*, jobject,
                                                                                                jlong handle) {
    auto* s = reinterpret_cast<Spike*>(handle);
    if (!s) return;
    if (s->raw) AImageReader_delete(s->raw);
    if (s->companion) AImageReader_delete(s->companion);
    delete s;
}
