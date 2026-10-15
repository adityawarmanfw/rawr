#include <android/asset_manager_jni.h>
#include <android/log.h>
#include <jni.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <type_traits>
#include <unordered_map>

#include "local_tone/ExposureFusion.h"
#include "capture/multiframe/mfsr/MfsrCaptureJob.h"
#include "capture/persistence/CaptureJob.h"
#include "color/AutoKeyExposure.h"
#include "color/ColorMath.h"
#include "color/FilmExposure.h"
#include "develop/DevelopContextBuilder.h"
#include "encoding/dng/DngCaptureWriter.h"
#include "encoding/jpeg/JpegCaptureWriter.h"
#include "encoding/jpeg/JpegPublicDescription.h"
#include "jni/DevelopSettingsReader.h"
#include "tonemap/ColorRenderProfile.h"
#include "renderer/ExifTranspose.h"
#include "renderer/RendererEngine.h"
#include "renderer/SceneKeyMeter.h"
#include "vulkan/VulkanDispatch.h"
namespace {
std::string str(JNIEnv* e, jstring s) {
    if (!s) return {};
    const char* p = e->GetStringUTFChars(s, nullptr);
    std::string result(p);
    e->ReleaseStringUTFChars(s, p);
    return result;
}
void fail(JNIEnv* e, const std::exception& x) {
    if (!e->ExceptionCheck()) e->ThrowNew(e->FindClass("java/lang/IllegalStateException"), x.what());
}
using rawrcam::jni::Json;
struct Session {
    rawrcam::renderer::DngSource source;
    // Scene key measured from this DNG, per shading/white balance (editor
    // shadow-lift modes on sources the camera did not meter).
    struct MeasuredKey {
        bool shading = false;
        std::array<float, 3> whiteBalance{};
        std::optional<rawrcam::renderer::SceneKey> key;
    };
    std::optional<MeasuredKey> measuredKey;
    std::unique_ptr<rawrcam::renderer::RendererEngine> engine;
    std::atomic<rawrcam::renderer::RendererEngine*> activeEngine{nullptr};
    std::string cache, files, lib;
    jobject assets;
    Session(JNIEnv* e, const std::string& path, const std::string& dir, const std::string& library, jobject a)
        : source(path), cache(path + ".linear"), files(dir), lib(library), assets(e->NewGlobalRef(a)) {}
};
std::mutex handlesMutex;
std::unordered_map<jlong, std::shared_ptr<Session>> handles;
std::atomic<jlong> next{1};
std::shared_ptr<Session> get(jlong id) {
    std::lock_guard<std::mutex> lock(handlesMutex);
    auto it = handles.find(id);
    if (it == handles.end()) throw std::runtime_error("Renderer session closed");
    return it->second;
}
// Editor shadow-lift mode: the camera's auto-key curve for that mode, from
// the stored key when the camera metered, else from this DNG's own meter.
// No override keeps the captured adaptive tone exactly.
void resolveAdaptiveTone(Session& s, rawrcam::renderer::RenderOptions& o) {
    // Local tone mapping replaces the adaptive curve, as at capture.
    if (rawr::local_tone::fusionActive(o.context.tonemapParams.localTone.shadowsEv)) {
        o.context.tonemapParams.adaptiveTone = {};
        return;
    }
    if (o.adaptiveToneMode.empty()) return;
    const auto mode = rawrcam::color::autoKeyModeFromName(o.adaptiveToneMode);
    if (!mode) throw std::runtime_error("Unsupported shadow lift mode");
    auto& tone = o.context.tonemapParams;
    if (*mode == rawrcam::color::AutoKeyMode::Off) {
        tone.adaptiveTone = {};
        return;
    }
    const auto policy = rawrcam::color::autoKeyPolicy(*mode);
    if (o.adaptiveToneMeasured) {
        // Stored key and highlight already include the capture's HAL gain.
        tone.adaptiveTone = rawrcam::color::autoKeyCurve(tone.adaptiveTone.keyEv, tone.adaptiveTone.highlightEv, 1.0f,
                                                        policy);
        return;
    }
    if (!s.measuredKey || s.measuredKey->shading != o.shading || s.measuredKey->whiteBalance != s.source.wb)
        s.measuredKey = Session::MeasuredKey{
            o.shading, s.source.wb,
            rawrcam::renderer::measureSceneKey(s.source, o.shading, s.source.wb,
                                               rawrcam::color::math::cameraToWorkingColumnMajor(s.source.cameraToSrgb))};
    tone.adaptiveTone = s.measuredKey->key ? rawrcam::color::autoKeyCurve(s.measuredKey->key->keyEv,
                                                                          s.measuredKey->key->highlightEv,
                                                                          std::max(tone.aePostGain, 1.0e-6f), policy)
                                           : tonemap::AdaptiveToneParams{};
}

rawrcam::renderer::RenderOptions options(JNIEnv* env, jstring recipe, int workingW, int workingH, int outputW,
                                         int outputH) {
    Json json(env, recipe), tone(env, json.child("tone")), film(env, json.child("film"));
    rawrcam::renderer::RenderOptions o;
    o.width = workingW;
    o.height = workingH;
    o.shading = json.flag("lensShadingCorrectionEnabled", true);
    o.distortion = json.flag("distortionCorrectionEnabled", true);
    auto& c = o.context;
    c.rendererStrict = true;
    c.requestId = 1;
    c.filmEnabled = json.flag("filmSimEnabled", false);
    c.timestampNs = uint64_t(json.number("timestampNs", 1000000000));
    const auto developSettings = rawrcam::jni::readRendererDevelopSettings(json);
    o.develop = developSettings;
    rawrcam::develop::applyDevelopSettings(c, developSettings);
    c.denoiseStrength = developSettings.denoiseStrength;
    c.denoiseDetail = developSettings.denoiseDetail;
    c.denoiseForceY = developSettings.denoiseForceY;
    c.denoiseMaxScale = developSettings.denoiseMaxScale;
    c.galoshYuvMode = developSettings.galoshYuvMode;
    c.galoshYuvStrengthY = developSettings.galoshYuvStrengthY;
    c.galoshYuvStrengthC = developSettings.galoshYuvStrengthC;
#ifndef NDEBUG
    // The headless DNG replay uses the same render path and requests the
    // existing production stage dumps only in debug builds.
    c.diagnosticsEnabled = json.flag("replayDiagnostics", false);
#endif
    auto& t = c.tonemapParams;
    t.exposureEV = tone.number("renderExposure", 0);
    t.blackPointEV = tone.number("blacks", 0);
    t.shadowLiftEV = tone.number("shadows", 0);
    t.midtoneLiftEV = tone.number("midtones", 0);
    t.contrast = tone.number("contrast", 0);
    t.whitePointEV = tone.number("whites", 0);
    t.highlightBiasEV = tone.number("highlights", 0);
    t.saturation = tone.number("saturation", 0);
    t.vibrance = tone.number("vibrance", 0);
    t.aePostGain = json.number("aePostGain", 1);
    Json adaptive(env, json.child("adaptiveTone"));
    t.adaptiveTone = {float(adaptive.number("liftEv", 0)), float(adaptive.number("keyEv", -4)),
                      float(adaptive.number("highlightEv", 0))};
    if (!tonemap::validAdaptiveTone(t.adaptiveTone)) throw std::runtime_error("Invalid adaptive tone recipe");
    // Captures carry their local tone mapping; absent = off. Version 1 was
    // the earlier guided-filter lift (strength/detail): replayed as exposure
    // fusion at the defaults when it was on.
    Json localTone(env, json.child("localTone"));
    if (localTone.number("version", 1) >= 2) {
        t.localTone = {float(localTone.number("shadowsEv", 0)),
                       float(localTone.number("highlightsEv", rawr::local_tone::kDefaultHighlightsEv))};
    } else {
        t.localTone = localTone.number("strength", 0) > 0
                          ? tonemap::LocalToneParams{rawr::local_tone::kDefaultShadowsEv,
                                                     rawr::local_tone::kDefaultHighlightsEv}
                          : tonemap::LocalToneParams{};
    }
    // Editor override (Tone mapping / Local shadows); 0 = adaptive curve.
    if (const double shadows = json.number("localToneShadowsEv", -1); shadows >= 0)
        t.localTone = shadows > 0 ? tonemap::LocalToneParams{float(shadows), rawr::local_tone::kDefaultHighlightsEv}
                                  : tonemap::LocalToneParams{};
    if (rawr::local_tone::fusionActive(t.localTone.shadowsEv) &&
        !rawr::local_tone::validParams(rawr::local_tone::fusionParams(t.localTone.shadowsEv, t.localTone.highlightsEv)))
        throw std::runtime_error("Invalid local tone recipe");
    o.adaptiveToneMode = json.text("adaptiveToneMode");
    if (o.adaptiveToneMode == "null") o.adaptiveToneMode.clear();
    // Captures since the mode was recorded say whether the camera metered;
    // older ones metered exactly when they stored a lift.
    const std::string captured = json.text("capturedAutoKeyExposure");
    o.adaptiveToneMeasured = !captured.empty() && captured != "null" ? captured != "Off" : t.adaptiveTone.liftEv > 0.0f;
    if (rawrcam::tonemap_integration::isAgxProfile(developSettings.colorRenderProfile)) {
        // Preset names match tonemap::AgxPreset; recipe keys (kAgxControlKeys)
        // override its control row, so older recipes fill in from the preset.
        Json agx(env, json.child("agx"));
        static constexpr const char* kPresets[tonemap::kAgxPresetCount] = {"Default", "BlenderBase", "BlenderPunchy",
                                                                           "Sigmoid", "Smooth"};
        const std::string name = agx.text("preset");
        uint32_t preset = 0;
        for (uint32_t i = 0; i < tonemap::kAgxPresetCount; ++i)
            if (name == kPresets[i]) preset = i;
        float controls[tonemap::kAgxControlCount];
        for (uint32_t i = 0; i < tonemap::kAgxControlCount; ++i)
            controls[i] = float(agx.number(tonemap::kAgxControlKeys[i], tonemap::kAgxPresetControls[preset][i]));
        if (!rawrcam::tonemap_integration::agxFromControls(controls, &t.agx))
            throw std::runtime_error("Invalid AgX recipe");
    }
#define RAWR_FILM_FIELD(type, name) c.filmLook.name = film.scalar<type>(#name, c.filmLook.name);
#include "tonemap/FilmLookFields.inc"
#undef RAWR_FILM_FIELD
    o.demosaic = json.text("demosaicAlgorithm");
    if (o.demosaic.empty()) o.demosaic = "Rcd";
    if (o.demosaic != "Amaze" && o.demosaic != "Rcd" && o.demosaic != "Vng4" && o.demosaic != "DualRcdVng4" &&
        o.demosaic != "DualAmazeVng4")
        throw std::runtime_error("Unsupported demosaic recipe");
    o.dualAutoContrast = json.flag("dualAutoContrast", true);
    o.dualContrastPercent = std::clamp(float(json.number("dualContrastPercent", 20)), 0.f, 100.f);
    o.waveform = json.flag("rendererWaveform", false);
    // Retired renderer control: ignore flags in older drafts/capture recipes.
    o.quadfix = false;
    o.quadfixFastMedian = false;
    o.temperature = float(tone.number("wbTemperature", 0));
    o.tint = float(tone.number("wbTint", 0));
    c.lutStrength = float(tone.number("colorRenderingStrength", 1));
    c.filmTiled = uint64_t(outputW) * outputH >= 12000000u && (!c.filmLook.grainEnabled || c.filmLook.grainModel != 2);
    // UltraHDR (JPEG_R): follow the recipe flag (capture default). Mux
    // metadata mirrors the still-capture defaults via toGainmapParams() so
    // render and mux can never drift; hdrExposure matches the SDR base
    // domain exactly like SingleFrame/MFSR (tonemap: aePostGain * 2^EV,
    // film: folded film EV). The CST is filled in RendererEngine::render()
    // from the final DngSource matrix. Previews are gated off at the call
    // site (fd<0) and overview is gated in the engine.
    c.ultraHdrEnabled = json.flag("ultraHdrEnabled", false);
    if (c.ultraHdrEnabled) {
        rawrcam::color::UltraHdrParams uhDefaults;
        c.gainmapParams = uhDefaults.toGainmapParams();
        const float aeGain = std::max(float(t.aePostGain), 1.0e-6f);
        if (c.filmEnabled) {
            c.gainmapParams.hdrExposure = std::exp2(rawrcam::color::filmExposureEv(c.filmLook, aeGain));
        } else {
            c.gainmapParams.hdrExposure = aeGain * std::exp2(float(t.exposureEV));
        }
    }
    return o;
}
}  // namespace
extern "C" JNIEXPORT jlong JNICALL Java_com_rawr_camera_renderer_RendererNative_open(JNIEnv* e, jobject, jstring path,
                                                                                     jstring files, jstring lib,
                                                                                     jobject assets) {
    try {
        auto s = std::make_shared<Session>(e, str(e, path), str(e, files), str(e, lib), assets);
        jlong id = next++;
        std::lock_guard<std::mutex> lock(handlesMutex);
        handles[id] = s;
        return id;
    } catch (const std::exception& x) {
        fail(e, x);
        return 0;
    }
}
extern "C" JNIEXPORT jintArray JNICALL Java_com_rawr_camera_renderer_RendererNative_waveform(JNIEnv* e, jobject, jlong id) {
    try {
        auto session = get(id);
        if (!session->engine || session->engine->waveform().empty()) return nullptr;
        const auto& pixels = session->engine->waveform();
        auto result = e->NewIntArray(jsize(pixels.size()));
        if (result) e->SetIntArrayRegion(result, 0, jsize(pixels.size()), reinterpret_cast<const jint*>(pixels.data()));
        return result;
    } catch (const std::exception& error) { fail(e, error); return nullptr; }
}

extern "C" JNIEXPORT jstring JNICALL Java_com_rawr_camera_renderer_RendererNative_metadata(JNIEnv* e, jobject,
                                                                                           jlong id) {
    try {
        auto session = get(id);
        const auto& source = session->source;
        std::ostringstream text;
        text << "{\"iso\":" << source.sensitivity().value_or(0)
             << ",\"exposureNs\":" << source.exposureTimeNs().value_or(0)
             << ",\"aperture\":" << source.aperture().value_or(0)
             << ",\"whiteLevel\":" << source.whiteLevel
             << ",\"cfa\":" << source.cfa
             << ",\"blackLevels\":[" << source.black[0] << ',' << source.black[1] << ','
             << source.black[2] << ',' << source.black[3] << "]}";
        return e->NewStringUTF(text.str().c_str());
    } catch (const std::exception& error) {
        fail(e, error);
        return nullptr;
    }
}

extern "C" JNIEXPORT jstring JNICALL Java_com_rawr_camera_renderer_RendererNative_inspect(JNIEnv* e, jobject,
                                                                                          jlong id) {
    try {
        auto s = get(id);
        auto& d = s->source;
        std::ostringstream text;
        text << d.crop[2] << '\n'
             << d.crop[3] << '\n'
             << d.orientation << '\n'
             << (d.rawr ? 1 : 0) << '\n'
             << d.provenance;
        return e->NewStringUTF(text.str().c_str());
    } catch (const std::exception& x) {
        fail(e, x);
        return nullptr;
    }
}
extern "C" JNIEXPORT jintArray JNICALL
Java_com_rawr_camera_renderer_RendererNative_render(JNIEnv* e, jobject, jlong id, jstring recipe, jint w, jint h,
                                                    jint fd, jintArray crop, jint fullWidth, jint fullHeight) {
    try {
        auto s = get(id);
        if (!s->engine)
            s->engine = std::make_unique<rawrcam::renderer::RendererEngine>(s->files, s->lib,
                                                                            AAssetManager_fromJava(e, s->assets));
        s->activeEngine.store(s->engine.get());
        auto& engine = *s->engine;
        engine.cancelled = false;
        engine.progress = 0;
        engine.stage = 0;
        if (w < 1 || h < 1 || fullWidth < 1 || fullHeight < 1 || uint64_t(fullWidth) * fullHeight > 250000000)
            throw std::runtime_error("Invalid render dimensions");
        auto o = options(e, recipe, w, h, w, h);
        o.overview = fd < 0 && !crop;
        const auto croppedPixels = uint64_t(s->source.crop[2]) * s->source.crop[3];
        const bool highResolutionSource = croppedPixels >= 180000000u && croppedPixels <= 220000000u;
        if (highResolutionSource && ((s->source.crop[0] | s->source.crop[1] | s->source.crop[2] | s->source.crop[3]) & 1u))
            throw std::runtime_error("200MP binning requires an even Bayer crop");
        o.bayerBin2x = croppedPixels >= 180000000u && croppedPixels <= 220000000u &&
                       !(s->source.crop[0] & 1u) && !(s->source.crop[1] & 1u) && !(s->source.crop[2] & 1u) &&
                       !(s->source.crop[3] & 1u);
        if (o.bayerBin2x && (o.width > s->source.crop[2] / 2u || o.height > s->source.crop[3] / 2u))
            throw std::runtime_error("200MP sources must render at 50MP or below");
        auto path = s->cache + (o.shading ? "-lsc" : "-plain");
#ifndef NDEBUG
        Json replayJson(e, recipe);
        const auto replayCfa = replayJson.text("replayCfaOverride");
        if (!replayCfa.empty()) {
            if (replayCfa.size() != 20 || replayCfa.substr(16) != ".f32" ||
                !std::all_of(replayCfa.begin(), replayCfa.begin() + 16,
                             [](char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); }))
                throw std::runtime_error("Invalid replay CFA name");
            s->source.setReplayCfa(s->files + "/dng_replay/" + replayCfa);
            // Opposed reconstruction has already changed the CFA. This debug
            // replay exercises the normal remaining demosaic/JPEG_R path.
            o.context.highlightReconstructionEnabled = false;
            path += "-replay-" + replayCfa.substr(0, 16);
        }
#endif
        Json recipeJson(e, recipe);
        Json frame(e, recipeJson.child("resolvedFrame"));
        auto array = [&](const char* key, float* target, int count) {
            jstring k = e->NewStringUTF(key);
            auto a = e->CallObjectMethod(
                frame.value, e->GetMethodID(frame.cls, "optJSONArray", "(Ljava/lang/String;)Lorg/json/JSONArray;"), k);
            e->DeleteLocalRef(k);
            if (!a) return;
            auto cls = e->FindClass("org/json/JSONArray");
            if (e->CallIntMethod(a, e->GetMethodID(cls, "length", "()I")) == count)
                for (int i = 0; i < count; ++i)
                    target[i] =
                        float(e->CallDoubleMethod(a, e->GetMethodID(cls, "optDouble", "(ID)D"), i, double(target[i])));
            e->DeleteLocalRef(a);
            e->DeleteLocalRef(cls);
        };
        float wb[4]{s->source.wb[0], s->source.wb[1], s->source.wb[1], s->source.wb[2]};
        array("whiteBalanceRggb", wb, 4);
        s->source.wb = {wb[0], (wb[1] + wb[2]) * .5f, wb[3]};
        array("cameraToLinearSrgbRowMajor", s->source.cameraToSrgb.data(), 9);
        resolveAdaptiveTone(*s, o);
        const auto proxyPath = path + ".proxy";
        if (o.bayerBin2x) path += "-natural2x-f32-v1";
        // Calibration participates in both binning and demosaic cache identity.
        for (float gain : s->source.wb) path += "-" + std::to_string(gain);
        const auto binnedPath = path + ".cfa";
        path += "." + o.demosaic + (o.dualAutoContrast ? "-auto" : "-manual") + std::to_string(o.dualContrastPercent);
        path += "-balanced-v2";
        if (o.quadfix) path += o.quadfixFastMedian ? "-qf-pad-v2-fast" : "-qf-pad-v2";
        const uint64_t expected = uint64_t(s->source.info().width / (o.bayerBin2x ? 2 : 1)) *
            (s->source.info().height / (o.bayerBin2x ? 2 : 1)) * 8;
        const bool prepared = std::filesystem::exists(path) && std::filesystem::file_size(path) == expected;
        if (o.overview && !prepared) {
            o.quickProxy = true;
            o.bayerBin2x = false;
            path = proxyPath;
            engine.stage = 1;
            engine.prepareProxy(s->source, path, o.shading);
        } else if (!prepared) {
            const auto prepareStarted = std::chrono::steady_clock::now();
            if (o.bayerBin2x) {
                engine.stage = 2;
                s->source.prepareBinned(binnedPath, o.shading, [&] {
                    if (engine.cancelled) throw std::runtime_error("Cancelled");
                });
                __android_log_print(ANDROID_LOG_INFO, "RawrRenderer", "Bayer bin/cache ready in %lldms",
                    static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-prepareStarted).count()));
            }
            engine.stage = 3;
            engine.prepare(s->source, path, o);
            __android_log_print(ANDROID_LOG_INFO, "RawrRenderer", "Working RAW ready in %lldms",
                static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-prepareStarted).count()));
        }
        jint region[4]{0, 0, jint(s->source.crop[2]), jint(s->source.crop[3])};
        if (crop) {
            if (e->GetArrayLength(crop) != 4) throw std::runtime_error("Invalid preview region");
            e->GetIntArrayRegion(crop, 0, 4, region);
            if (region[0] < 0 || region[1] < 0 || region[2] < 1 || region[3] < 1 ||
                uint64_t(region[0]) + region[2] > s->source.crop[2] ||
                uint64_t(region[1]) + region[3] > s->source.crop[3])
                throw std::runtime_error("Invalid preview region");
        }
        if (crop) {
            // Spatial stages see neighbors beyond the visible viewport. Keep
            // this bounded in preview pixels, independent of source size.
            constexpr int margin = 128;
            const int px = int(std::ceil(double(region[2]) * margin / w));
            const int py = int(std::ceil(double(region[3]) * margin / h));
            const int left = std::max(0, region[0] - px) & ~1;
            const int top = std::max(0, region[1] - py) & ~1;
            const int right = std::min(int(s->source.crop[2]), (region[0] + region[2] + px + 1) & ~1);
            const int bottom = std::min(int(s->source.crop[3]), (region[1] + region[3] + py + 1) & ~1);
            o.region = {uint32_t(left), uint32_t(top), uint32_t(right-left), uint32_t(bottom-top)};
            o.width = std::max(2u, uint32_t(std::lround(double(w) * o.region[2] / region[2])));
            o.height = std::max(2u, uint32_t(std::lround(double(h) * o.region[3] / region[3])));
        }
        std::vector<uint8_t> pixels;
        const std::string key =
            "renderer-v2-lab-rcd\n" + std::to_string(o.width) + "x" + std::to_string(o.height) + "\n" + std::to_string(region[0]) + "," +
            std::to_string(region[1]) + "," + std::to_string(region[2]) + "," + std::to_string(region[3]) + "\n" +
            std::to_string(o.region[0]) + "," + std::to_string(o.region[1]) + "," + std::to_string(o.region[2]) + "," + std::to_string(o.region[3]) + "\n" + str(e, recipe);
        const std::string renderedPath = path + ".rendered", keyPath = renderedPath + ".key";
        bool cached = false;
        // Exports bypass the rendered-pixel cache: a hit would skip
        // engine.render() and leave no timings to report. The demosaic and
        // reduced-image caches underneath still apply.
        const bool isExport = fd >= 0;
        // UltraHDR gain map is export-only: previews (overview and detail)
        // never allocate map resources, matching the still-capture contract.
        if (!isExport) o.context.ultraHdrEnabled = false;
        if (!o.overview && !isExport && !o.waveform && std::filesystem::exists(renderedPath) &&
            std::filesystem::file_size(renderedPath) == uint64_t(o.width) * o.height * 4) {
            std::ifstream k(keyPath);
            std::string previous((std::istreambuf_iterator<char>(k)), {});
            if (previous == key) {
                pixels.resize(size_t(o.width) * o.height * 4);
                std::ifstream in(renderedPath, std::ios::binary);
                in.read(reinterpret_cast<char*>(pixels.data()), std::streamsize(pixels.size()));
                cached = bool(in);
            }
        }
        if (!cached) {
            pixels = engine.render(s->source, path, o);
            if (!o.overview && !isExport) {
                std::ofstream out(renderedPath + ".tmp", std::ios::binary);
                out.write(reinterpret_cast<const char*>(pixels.data()), std::streamsize(pixels.size()));
                out.close();
                if (!out) throw std::runtime_error("Cannot cache rendered image");
                std::filesystem::rename(renderedPath + ".tmp", renderedPath);
                std::ofstream k(keyPath + ".tmp");
                k << key;
                k.close();
                if (!k) throw std::runtime_error("Cannot save render cache key");
                std::filesystem::rename(keyPath + ".tmp", keyPath);
            }
        }
        if (fd >= 0) {
            Json json(e, recipe), tone(e, json.child("tone")), exportJson(e, json.child("export"));
            rawrcam::encoding::jpeg::JpegCaptureContext c;
            c.outputFd = rawrcam::encoding::OutputFd(dup(fd));
            c.quality = int(tone.number("jpegQuality", 98));
            auto subsampling = tone.text("jpegChromaSubsamplingId");
            c.subsampling = subsampling == "jpeg.444"   ? rawrcam::encoding::jpeg::ChromaSubsampling::Yuv444
                            : subsampling == "jpeg.422" ? rawrcam::encoding::jpeg::ChromaSubsampling::Yuv422
                                                        : rawrcam::encoding::jpeg::ChromaSubsampling::Yuv420;
            c.exifOrientation = uint16_t(s->source.orientation);
            c.deviceRotationDegrees = s->source.orientation == 6   ? 90
                                      : s->source.orientation == 8 ? 270
                                      : s->source.orientation == 3 ? 180
                                                                   : 0;
            // Export identity (Kotlin-built, still-capture format). Absent on
            // recipes predating the export block: fall back to the legacy values.
            c.deviceMake = exportJson.text("deviceMake");
            if (c.deviceMake.empty()) c.deviceMake = "RAWR";
            c.deviceModel = exportJson.text("deviceModel");
            if (c.deviceModel == "null") c.deviceModel.clear();
            c.wallClockUnixMillis = std::int64_t(exportJson.number("wallClockMillis", 0));
            c.utcOffsetMinutes = std::int16_t(exportJson.number("utcOffsetMinutes", 0));
            c.displayName = exportJson.text("displayName");
            if (c.displayName.empty()) c.displayName = "Renderer.jpg";
            // Capture EXIF carried by the source DNG, when present. Tags are
            // omitted when absent, matching the still writer's behavior.
            if (const auto exposure = s->source.exposureTimeNs()) c.exposureTimeNs = *exposure;
            if (const auto sensitivity = s->source.sensitivity()) c.sensitivity = *sensitivity;
            if (const auto aperture = s->source.aperture()) c.aperture = *aperture;
            // Stage timings: same attribution as still captures. Demosaic is
            // the renderer pre-work wall time (prepare + resample/warp/upload).
            const auto& completion = engine.lastCompletion();
            c.demosaicMs = engine.lastDemosaicMs();
            c.colorProcessingMs = completion.colorProcessingMs;
            c.highlightReconstructionMs = completion.highlightReconstructionMs;
            c.refinementMs = completion.refinementMs;
            c.tonemapMs = completion.tonemapMs;
            c.denoiseMs = completion.denoiseMs;
            c.galoshYuvMs = completion.galoshYuvMs;
            c.gainmapMs = completion.gainmapMs;
            // Stage labels and the defringe/highlight-tone tail, as on still captures.
            c.postTailMs = completion.postTailMs;
            c.highlightStage = completion.highlightStage;
            c.defringeEnabled = completion.defringeEnabled;
            c.filmRendered = completion.filmRendered;
            c.filmFallbackMemory = completion.filmFallbackMemory;
            c.renderSetupMs = completion.renderSetupMs;
            c.renderEngineBuildMs = completion.engineBuildMs;
            c.queueGapsMs = completion.queueGapsMs;
            c.readbackMs = completion.readbackMs;
            c.renderTotalMs = engine.lastDemosaicMs() + completion.totalMs;
            // UltraHDR intent: mux metadata mirrors the render gainmap params
            // so the file and the GPU encode can never drift field-by-field
            // (same toGainmapParams() defaults as still captures).
            if (o.context.ultraHdrEnabled) {
                rawrcam::color::UltraHdrParams uh;
                uh.enabled = true;
                uh.gainMapMinLog2 = o.context.gainmapParams.minLog2;
                uh.gainMapMaxLog2 = o.context.gainmapParams.maxLog2;
                uh.gamma = o.context.gainmapParams.gamma;
                uh.offsetSdr = o.context.gainmapParams.offsetSdr;
                uh.offsetHdr = o.context.gainmapParams.offsetHdr;
                uh.hdrCapacityMinLog2 = o.context.gainmapParams.hdrCapacityMin;
                uh.hdrCapacityMaxLog2 = o.context.gainmapParams.hdrCapacityMax;
                c.ultraHdr = uh;
            }
            c.appliedFccSteps = std::clamp(o.context.fccSteps, 1u, 8u);
            c.appliedLensShadingCorrection = o.shading;
            // Human-readable header identical to still captures: renderer
            // profile + tonemap values, or the film block when the pixels
            // actually rendered through film.
            std::string filmDescription = exportJson.text("filmDescription");
            std::string rendererDisplayName = exportJson.text("rendererDisplayName");
            if (rendererDisplayName.empty() || rendererDisplayName == "null")
                rendererDisplayName = o.context.filmEnabled ? "Film" : "RAWR";
            c.filmDescription = filmDescription;
            c.rendererDisplayName = rendererDisplayName;
            c.imageDescription = rawrcam::encoding::jpeg::buildPublicDescription(
                o.develop, o.context.tonemapParams, rendererDisplayName, c.quality, c.subsampling, c.filmRendered,
                filmDescription, o.context.denoiseStrength, o.context.denoiseNoiseA, o.context.denoiseNoiseB,
                /*rendererExport=*/true);
            // Full parity with still captures: dual-encode + mux when the
            // GPU gain map ran, legacy SDR otherwise (same fallback contract
            // as SingleFrame/MFSR: missing map degrades, never fails).
            const bool dimsMatch =
                w == int(o.width) && h == int(o.height) && pixels.size() == size_t(o.width) * o.height * 4;
            const bool ultraHdr = c.ultraHdr.enabled && dimsMatch && !engine.lastGainmap().empty() &&
                                  engine.lastGainmapWidth() > 0 && engine.lastGainmapHeight() > 0;
            rawrcam::encoding::jpeg::JpegCaptureWriter writer;
            bool writerAccepted = false;
            if (ultraHdr) {
                writerAccepted = writer.startUltraHdr(
                    1, pixels.data(), pixels.size(), uint32_t(w), uint32_t(h), engine.lastGainmap().data(),
                    engine.lastGainmap().size(), engine.lastGainmapWidth(), engine.lastGainmapHeight(), std::move(c));
                __android_log_print(ANDROID_LOG_INFO, "RawrCamNative",
                                    "RENDERER_JPEG_WRITE_STARTED ultrahdr=1 gainmap=%ux%u", engine.lastGainmapWidth(),
                                    engine.lastGainmapHeight());
            } else {
                if (c.ultraHdr.enabled)
                    __android_log_print(ANDROID_LOG_WARN, "RawrCamNative",
                                        "RENDERER_ULTRAHDR_MAP_MISSING fallback=legacy_jpeg dimsMatch=%d mapBytes=%zu",
                                        dimsMatch ? 1 : 0, engine.lastGainmap().size());
                writerAccepted = writer.start(1, pixels.data(), pixels.size(), w, h, std::move(c));
            }
            if (!writerAccepted) throw std::runtime_error("JPEG encoder refused output");
            std::optional<rawrcam::encoding::jpeg::JpegWriteCompletion> done;
            while (!(done = writer.pollCompletion())) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (!done->success) throw std::runtime_error(done->error);
            engine.progress = 100;
            return nullptr;
        }
        std::vector<jint> argb(size_t(w) * h);
        if (!crop) {
            for (size_t i = 0; i < argb.size(); ++i)
                argb[i] = jint(0xff000000u | (uint32_t(pixels[i * 4]) << 16) |
                    (uint32_t(pixels[i * 4 + 1]) << 8) | pixels[i * 4 + 2]);
        } else
        for (int yy = 0; yy < h; ++yy)
            for (int xx = 0; xx < w; ++xx) {
                double px = crop ? (region[0] + (xx + .5) * region[2] / w - o.region[0]) * o.width / o.region[2] - .5 : xx;
                double py = crop ? (region[1] + (yy + .5) * region[3] / h - o.region[1]) * o.height / o.region[3] - .5 : yy;
                int x0 = std::clamp(int(std::floor(px)), 0, int(o.width) - 1),
                    y0 = std::clamp(int(std::floor(py)), 0, int(o.height) - 1);
                int x1 = std::min(x0 + 1, int(o.width) - 1), y1 = std::min(y0 + 1, int(o.height) - 1);
                double fx = px - std::floor(px), fy = py - std::floor(py);
                uint32_t color = 0xff000000u;
                for (int c = 0; c < 3; ++c) {
                    auto sample = [&](int x, int y) { return pixels[(size_t(y) * o.width + x) * 4 + c]; };
                    auto v = uint32_t(std::lround((1 - fy) * ((1 - fx) * sample(x0, y0) + fx * sample(x1, y0)) +
                                                  fy * ((1 - fx) * sample(x0, y1) + fx * sample(x1, y1))));
                    color |= v << (16 - c * 8);
                }
                argb[size_t(yy) * w + xx] = jint(color);
            }
        // Display order is applied here (see renderer/ExifTranspose.h, locked
        // by tests/native/exif_transpose_test.cpp) so Kotlin receives a
        // ready buffer with no second-Bitmap rotation allocation per preview.
        const auto oriented = rawrcam::renderer::transposeExif(argb, uint32_t(w), uint32_t(h), s->source.orientation);
        auto result = e->NewIntArray(jsize(oriented.size()));
        e->SetIntArrayRegion(result, 0, jsize(oriented.size()), oriented.data());
        return result;
    } catch (const std::exception& x) {
        fail(e, x);
        return nullptr;
    }
}
// These UI-facing operations borrow the engine only while holding the registry
// lock. They must not become the last shared_ptr owner and destroy a GPU session
// on the caller's thread if close() runs concurrently.
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_renderer_RendererNative_resetProgress(JNIEnv*, jobject, jlong id) {
    std::lock_guard<std::mutex> lock(handlesMutex);
    auto it = handles.find(id);
    if (it != handles.end()) if (auto* engine = it->second->activeEngine.load()) engine->progress = 0;
}
extern "C" JNIEXPORT jint JNICALL Java_com_rawr_camera_renderer_RendererNative_stage(JNIEnv*, jobject, jlong id) {
    std::lock_guard<std::mutex> lock(handlesMutex);
    auto it = handles.find(id);
    if (it != handles.end()) if (auto* engine = it->second->activeEngine.load()) return engine->stage.load();
    return 0;
}
extern "C" JNIEXPORT jint JNICALL Java_com_rawr_camera_renderer_RendererNative_progress(JNIEnv*, jobject, jlong id) {
    std::lock_guard<std::mutex> lock(handlesMutex);
    auto it = handles.find(id);
    if (it != handles.end()) if (auto* engine = it->second->activeEngine.load()) return engine->progress.load();
    return 0;
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_renderer_RendererNative_cancel(JNIEnv*, jobject, jlong id) {
    std::lock_guard<std::mutex> lock(handlesMutex);
    auto it = handles.find(id);
    if (it != handles.end()) if (auto* engine = it->second->activeEngine.load()) engine->cancelled = true;
}
extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_renderer_RendererNative_close(JNIEnv* e, jobject, jlong id) {
    std::shared_ptr<Session> retired;
    {
        std::lock_guard<std::mutex> lock(handlesMutex);
        auto it = handles.find(id);
        if (it != handles.end()) {
            retired = std::move(it->second);
            handles.erase(it);
        }
    }
    // Session destruction can wait for GPU work. Never hold the handle registry
    // lock here: cancellation/progress are also queried from the UI thread.
    if (retired) e->DeleteGlobalRef(retired->assets);
}

extern "C" JNIEXPORT void JNICALL Java_com_rawr_camera_renderer_RendererNative_materialize(JNIEnv* e, jobject,
                                                                                           jstring path, jint fd,
                                                                                           jstring files, jstring lib,
                                                                                           jobject assets) {
    try {
        using namespace rawrcam;
        auto job = capture::persistence::load(str(e, path), [](auto&, size_t, const auto&) {});
        if (!job.multiframe) {
            job = capture::persistence::load(str(e, path));
            job.dng.outputFd = encoding::OutputFd(dup(fd));
            job.frame.requestId = 1;
            encoding::dng::DngCaptureWriter writer;
            if (!writer.start(std::make_shared<imaging::RawSnapshot>(std::move(job.frame)), std::move(job.dng)))
                throw std::runtime_error("Cannot prepare queued RAW");
            std::optional<encoding::dng::DngWriteCompletion> done;
            while (!(done = writer.pollCompletion())) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (!done->success) throw std::runtime_error(done->error);
        } else {
            if (!vkCreateInstance) vulkan::dispatch::configure(str(e, lib), "");
            vulkan::VulkanContext vk;
            vk.createInstance();
            vk.createDeviceForSurface(VK_NULL_HANDLE);
            std::mutex& mutex = vk.primaryQueue().mutex();
            auto work = capture::persistence::loadBurst(str(e, path), vk, mutex);
            work->requestId = 1;
            work->jpegRequested = false;
            work->baseDng.outputFd.reset();
            work->mergedDng.outputFd = encoding::OutputFd(dup(fd));
            work->mergedJpeg.output.outputFd.reset();
            rawr::raw_gpu_pipeline::AndroidBurstCoordinator coordinator;
            develop::rendered::StillImageRenderer processor(str(e, files));
            capture::multiframe::StringMailbox dngBox, jpegBox;
            capture::multiframe::MfsrCaptureJob::Context context{
                vk,
                mutex,
                str(e, files),
                work->capture->frames.front().raw.ref.width,
                work->capture->frames.front().raw.ref.height,
                work->capture->referenceMetadata.cameraContext->rawPreviewCfa,
                coordinator,
                processor,
                false,
                dngBox,
                jpegBox,
                AAssetManager_fromJava(e, assets)};
            capture::multiframe::MfsrCaptureJob(context, std::move(work)).run();
            auto done = dngBox.poll();
            if (done.find("\t1\t") == std::string::npos) throw std::runtime_error("Queued merge failed: " + done);
        }
    } catch (const std::exception& x) {
        fail(e, x);
    }
}
