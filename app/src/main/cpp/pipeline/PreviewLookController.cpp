#include "pipeline/PreviewLookController.h"

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <vulkan/vulkan_android.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "diagnostics/logging/FrameAuditWriter.h"
#include "diagnostics/logging/NativeLog.h"
#include "diagnostics/timing/GpuTimingTracker.h"
#include "film_develop.h"
#include "film_diffusion.h"
#include "film_dir.h"
#include "film_exposure.h"
#include "film_grain.h"
#include "film_halation.h"
#include "film_input.h"
#include "film_output.h"
#include "film_output_half.h"
#include "film_printscan.h"
#include "film_scanner.h"
#include "imaging/FrameLimits.h"
#include "metadata/MetadataDiagnostics.h"
#include "raw_preview/RawPreview.hpp"
#include "tonemap.h"
#include "tonemap/ColorRenderProfile.h"
#include "tonemap/ImportedLutProfileStore.h"
#include "tonemap/TonemapEngine.h"
#include "vulkan/RawAhbImporter.h"
#include "vulkan/VulkanContext.h"
#include "vulkan/VulkanDispatch.h"
namespace rawrcam::pipeline {
namespace {
#define SESSION_LOGI(...) LOGI(__VA_ARGS__)
#define SESSION_LOGE(...) LOGE(__VA_ARGS__)
void vkCheck(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string(what) + " VkResult=" + std::to_string(r));
}
}  // namespace
namespace {
// Quarter-res film arena, quantized up to a 16px grid: viewfinder geometry
// routinely flickers by a few px during startup/resume; without slack every
// flicker triggers a full ~800ms engine rebuild.
constexpr uint32_t kFilmArenaQuantum = 16u;
uint32_t filmArenaDim(uint32_t previewDim) noexcept {
    const uint32_t q = std::max(1u, previewDim / 2u);
    return ((q + kFilmArenaQuantum - 1u) / kFilmArenaQuantum) * kFilmArenaQuantum;
}
}  // namespace

PreviewLookController::PreviewLookController(vulkan::VulkanContext& context, std::mutex& transitionMutex,
                                             std::mutex& queueMutex, std::string filesDir, Diagnostic diagnostic,
                                             Publish publish,
                                             std::function<void(const tonemap::TonemapParams&)> publishTone,
                                             std::function<void(uint32_t)> publishDivisor)
    : vulkanContext_(context),
      transitionMutex_(transitionMutex),
      queueSubmitMutex_(queueMutex),
      filesDir_(std::move(filesDir)),
      diagnostic_(std::move(diagnostic)),
      publish_(std::move(publish)),
      publishTone_(std::move(publishTone)),
      publishDivisor_(std::move(publishDivisor)) {}
PreviewLookController::~PreviewLookController() { stop(); }
void PreviewLookController::start() {
    filmBuildThread_ = std::thread([this] { filmBuildLoop(); });
}
void PreviewLookController::stop() {
    filmBuildStop_.store(true);
    filmBuildCv_.notify_all();
    if (filmBuildThread_.joinable()) filmBuildThread_.join();
}
std::shared_ptr<spektrafilm_native::SpektraFilm> PreviewLookController::acquireFilm() const {
    std::lock_guard<std::mutex> lock(filmMutex_);
    return film_;
}
void PreviewLookController::configureGeometry(uint32_t width, uint32_t height) {
    ++filmBuildGeneration_;
    previewWidth_ = width;
    previewHeight_ = height;
    if (filmEnabled_ && filmHaveLook_) {
        const uint32_t quarterW = filmArenaDim(previewWidth_);
        const uint32_t quarterH = filmArenaDim(previewHeight_);
        if (!film_ || quarterW > filmArenaW_ || quarterH > filmArenaH_) {
            try {
                SESSION_LOGI("FILM_SIM_RECREATE reason=%s arena=%ux%u->%ux%u", !film_ ? "missing" : "arena-growth",
                             filmArenaW_, filmArenaH_, quarterW, quarterH);
                createFilm();
                filmArenaW_ = quarterW;
                filmArenaH_ = quarterH;
            } catch (const std::exception& e) {
                filmEnabled_ = false;
                {
                    std::lock_guard<std::mutex> filmLock(filmMutex_);
                    film_.reset();
                }
                emit(std::string("FILM_SIM_CONFIGURE_FAILURE error=") + e.what());
            }
        }
    }
}
void PreviewLookController::clearProcessing(bool teardownFilm) {
    ++filmBuildGeneration_;
    tonemap_.reset();
    if (teardownFilm) {
        std::lock_guard<std::mutex> lock(filmMutex_);
        film_.reset();
        filmArenaW_ = filmArenaH_ = 0;
    }
}
bool PreviewLookController::selectProfile(tonemap_integration::ColorRenderProfile profile,
                                          std::string importedProfileId) {
    if (profile == colorRenderProfile_ && importedProfileId == importedLutProfileId_ &&
        profile == tonemap_integration::ColorRenderProfile::RawrBase)
        return true;
    colorRenderProfile_ = profile;
    tonemap_integration::applyRenderProfile(profile, tonemapParams_);
    publishTone_(tonemapParams_);
    importedLutProfileId_ = std::move(importedProfileId);
    if (!tonemap_ || !vulkanContext_.device()) return true;
    try {
        std::lock_guard<std::mutex> queueLock(queueSubmitMutex_);
        vkCheck(vkQueueWaitIdle(vulkanContext_.queue()), "vkQueueWaitIdle color render change");
        tonemap_.reset();
        createTone();
        return true;
    } catch (const std::exception& e) {
        emit(std::string("COLOR_RENDER_PROFILE_CHANGE_FAIL error=") + e.what());
        return false;
    }
}
void PreviewLookController::suppressForVideo(bool suppressed) {
    if (videoSuppressed_ == suppressed) return;
    videoSuppressed_ = suppressed;
    ++filmBuildGeneration_;
    if (suppressed) {
        publish_(false, filmLook_);
        applyFilmSimState(false, filmLook_);
        return;
    }
    if (requestedFilmEnabled_ && canPrepareFilm() && needsFilmRebuild(filmLook_)) {
        scheduleFilmBuildLocked();
        return;
    }
    publish_(requestedFilmEnabled_, filmLook_);
    applyFilmSimState(requestedFilmEnabled_, filmLook_);
}
void PreviewLookController::createTone() {
    auto renderLut =
        rawrcam::tonemap_integration::loadRenderTransformLut(colorRenderProfile_, filesDir_, importedLutProfileId_);
    tonemap::TonemapCreateInfo toneCreate{};
    toneCreate.context.physicalDevice = vulkanContext_.physicalDevice();
    toneCreate.context.device = vulkanContext_.device();
    toneCreate.shaderSpirv = reinterpret_cast<const uint32_t*>(tonemap_spv);
    toneCreate.shaderSpirvBytes = tonemap_spv_size;
    toneCreate.maxFramesInFlight = rawrcam::imaging::kRealtimeFramesInFlight;
    toneCreate.lutChain = renderLut ? &*renderLut : nullptr;
    tonemap_ = std::make_unique<tonemap::TonemapEngine>(toneCreate);
    emit(std::string("PREVIEW_COLOR_RENDER_PROFILE profile=") +
         rawrcam::tonemap_integration::colorRenderProfileName(colorRenderProfile_) +
         " lutStages=" + std::to_string(renderLut ? renderLut->stages.size() : 0));
}

bool PreviewLookController::canPrepareFilm() const noexcept {
    return assetManager_ && previewWidth_ && previewHeight_ && vulkanContext_.device();
}

bool PreviewLookController::needsFilmRebuild(const spektrafilm_native::FilmLook& look) const noexcept {
    return !film_ || look.film != filmLookBaked_.film || look.paper != filmLookBaked_.paper ||
           look.rgbToRawMethod != filmLookBaked_.rgbToRawMethod ||
           look.inputColorSpace != filmLookBaked_.inputColorSpace ||
           look.outputColorSpace != filmLookBaked_.outputColorSpace || look.grainModel != filmLookBaked_.grainModel ||
           look.cameraUvFilterEnabled != filmLookBaked_.cameraUvFilterEnabled ||
           look.cameraUvCutNm != filmLookBaked_.cameraUvCutNm ||
           look.cameraIrFilterEnabled != filmLookBaked_.cameraIrFilterEnabled ||
           look.cameraIrCutNm != filmLookBaked_.cameraIrCutNm || filmArenaDim(previewWidth_) > filmArenaW_ ||
           filmArenaDim(previewHeight_) > filmArenaH_;
}

PreviewLookController::PreparedFilm PreviewLookController::prepareFilm(const spektrafilm_native::FilmLook& look,
                                                                       uint32_t width, uint32_t height) const {
    SESSION_LOGI("FILM_CREATE_BEGIN arena=%ux%u", filmArenaDim(width), filmArenaDim(height));
    if (!assetManager_) {
        throw std::runtime_error("FilmSim: asset manager not set (call setAssetManager first)");
    }
    if (width == 0 || height == 0) {
        throw std::runtime_error("FilmSim: no preview geometry (configure camera first)");
    }
    // RGBA16F blit (half-res linear -> quarter-res film input) must be supported.
    VkFormatProperties linearProps{};
    vkGetPhysicalDeviceFormatProperties(vulkanContext_.physicalDevice(), VK_FORMAT_R16G16B16A16_SFLOAT, &linearProps);
    constexpr auto kBlitBits = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    if ((linearProps.optimalTilingFeatures & kBlitBits) != kBlitBits) {
        throw std::runtime_error("FilmSim: RGBA16F blit unsupported on this GPU");
    }
    const auto createT0 = std::chrono::steady_clock::now();
    using AssetPtr = std::unique_ptr<AAsset, decltype(&AAsset_close)>;
    AssetPtr hanatos(AAssetManager_open(assetManager_, "spektrafilm/SpektraHanatos2025Spectra.f32", AASSET_MODE_BUFFER),
                     AAsset_close);
    AssetPtr gamut(
        AAssetManager_open(assetManager_, "spektrafilm/SpektraOutputGamutCompression.f32", AASSET_MODE_BUFFER),
        AAsset_close);
    if (!hanatos || !gamut) {
        throw std::runtime_error("FilmSim: missing APK assets (spektra/*.f32, check noCompress)");
    }
    const auto createT1 = std::chrono::steady_clock::now();
    spektrafilm_native::SpektraFilmCreateInfo filmCreate{};
    filmCreate.context.physicalDevice = vulkanContext_.physicalDevice();
    filmCreate.context.device = vulkanContext_.device();
    filmCreate.queue = vulkanContext_.queue();
    filmCreate.queueFamilyIndex = vulkanContext_.queueFamily();
    filmCreate.maxFramesInFlight = rawrcam::imaging::kRealtimeFramesInFlight;
    filmCreate.maxWidth = filmArenaDim(width);
    filmCreate.maxHeight = filmArenaDim(height);
    // Bake with every effect enable forced on (real grain model kept): the
    // preview pre-holds all scratch so on/off toggles and sliders stay
    // instant with no rebuild. Only a grain-model flip reallocates (see
    // bakedChanged). Per-frame record() still honors the live look, running
    // only effects whose buffers exist.
    {
        spektrafilm_native::FilmLook allocLook = look;
        allocLook.grainEnabled = true;
        allocLook.halationEnabled = true;
        allocLook.cameraDiffusionEnabled = true;
        allocLook.printDiffusionEnabled = true;
        allocLook.scannerEnabled = true;
        if (allocLook.dirCouplersAmount <= 0.0f) allocLook.dirCouplersAmount = 0.5f;
        filmCreate.look = allocLook;
    }
    filmCreate.inputSpirv = reinterpret_cast<const uint32_t*>(film_input_spv);
    filmCreate.inputSpirvBytes = film_input_spv_size;
    filmCreate.exposureSpirv = reinterpret_cast<const uint32_t*>(film_exposure_spv);
    filmCreate.exposureSpirvBytes = film_exposure_spv_size;
    filmCreate.developSpirv = reinterpret_cast<const uint32_t*>(film_develop_spv);
    filmCreate.developSpirvBytes = film_develop_spv_size;
    filmCreate.grainSpirv = reinterpret_cast<const uint32_t*>(film_grain_spv);
    filmCreate.grainSpirvBytes = film_grain_spv_size;
    filmCreate.dirSpirv = reinterpret_cast<const uint32_t*>(film_dir_spv);
    filmCreate.dirSpirvBytes = film_dir_spv_size;
    filmCreate.halationSpirv = reinterpret_cast<const uint32_t*>(film_halation_spv);
    filmCreate.halationSpirvBytes = film_halation_spv_size;
    filmCreate.diffusionSpirv = reinterpret_cast<const uint32_t*>(film_diffusion_spv);
    filmCreate.diffusionSpirvBytes = film_diffusion_spv_size;
    filmCreate.scannerSpirv = reinterpret_cast<const uint32_t*>(film_scanner_spv);
    filmCreate.scannerSpirvBytes = film_scanner_spv_size;
    filmCreate.printScanSpirv = reinterpret_cast<const uint32_t*>(film_printscan_spv);
    filmCreate.printScanSpirvBytes = film_printscan_spv_size;
    filmCreate.outputSpirv = reinterpret_cast<const uint32_t*>(film_output_spv);
    filmCreate.outputSpirvBytes = film_output_spv_size;
    filmCreate.hdrOutputSpirv = reinterpret_cast<const uint32_t*>(film_output_half_spv);
    filmCreate.hdrOutputSpirvBytes = film_output_half_spv_size;
    filmCreate.hanatosSpectra = reinterpret_cast<const float*>(AAsset_getBuffer(hanatos.get()));
    filmCreate.hanatosSpectraFloats = (size_t)AAsset_getLength64(hanatos.get()) / sizeof(float);
    filmCreate.gamutCompression = reinterpret_cast<const float*>(AAsset_getBuffer(gamut.get()));
    filmCreate.gamutCompressionFloats = (size_t)AAsset_getLength64(gamut.get()) / sizeof(float);
    PreparedFilm prepared{std::make_shared<spektrafilm_native::SpektraFilm>(filmCreate), look, filmCreate.maxWidth,
                          filmCreate.maxHeight};
    const auto createT2 = std::chrono::steady_clock::now();
    SESSION_LOGI("FILM_SIM_CREATE_MS assets=%lld ctor=%lld total=%lld film=%d paper=%d method=%d arena=%ux%u",
                 (long long)std::chrono::duration_cast<std::chrono::milliseconds>(createT1 - createT0).count(),
                 (long long)std::chrono::duration_cast<std::chrono::milliseconds>(createT2 - createT1).count(),
                 (long long)std::chrono::duration_cast<std::chrono::milliseconds>(createT2 - createT0).count(),
                 look.film, look.paper, look.rgbToRawMethod, prepared.arenaWidth, prepared.arenaHeight);
    return prepared;
}

bool PreviewLookController::preparedFilmFits(const PreparedFilm& prepared) const noexcept {
    return prepared.arenaWidth >= filmArenaDim(previewWidth_) && prepared.arenaHeight >= filmArenaDim(previewHeight_);
}

void PreviewLookController::installPreparedFilm(PreparedFilm prepared) {
    // The old engine's buffers may still be in flight after record() returns.
    // Constructing the replacement happened off the frame lock; wait only
    // for the final handoff, then publish the new shared engine.
    {
        std::lock_guard<std::mutex> queueLock(queueSubmitMutex_);
        vkCheck(vkQueueWaitIdle(vulkanContext_.queue()), "vkQueueWaitIdle film handoff");
    }
    {
        std::lock_guard<std::mutex> filmLock(filmMutex_);
        film_ = std::move(prepared.engine);
    }
    filmLookBaked_ = prepared.look;
    filmArenaW_ = prepared.arenaWidth;
    filmArenaH_ = prepared.arenaHeight;
    emit("FILM_SIM_CREATED film=" + std::to_string(filmLookBaked_.film) +
         " paper=" + std::to_string(filmLookBaked_.paper));
    SESSION_LOGI("FILM_SIM_CREATED film=%d paper=%d method=%d", filmLookBaked_.film, filmLookBaked_.paper,
                 filmLookBaked_.rgbToRawMethod);
}

void PreviewLookController::createFilm() {
    installPreparedFilm(prepareFilm(filmLookBaked_, previewWidth_, previewHeight_));
}

bool PreviewLookController::applyFilmSimState(bool enabled, const spektrafilm_native::FilmLook& bakedLook,
                                              bool isLookUpdate) {
    try {
        const bool bakedChanged = needsFilmRebuild(bakedLook);
        filmEnabled_ = enabled;
        if (isLookUpdate) filmHaveLook_ = true;
        // The enable call carries a complete look (engine-stored): latch it
        // so a pre-device enable still builds at session configure.
        if (enabled) filmHaveLook_ = true;
        // Pre-configure latch (the initial settings sync now lands here via
        // the start-gate, before any geometry exists): keep the latched
        // state and let configure create the engine. Must NOT clear
        // filmEnabled_ here — treating it as failure would leave film
        // silently off for the whole session.
        if (previewWidth_ == 0 || previewHeight_ == 0 || !vulkanContext_.device()) {
            if (!film_) filmLookBaked_ = bakedLook;
            return true;
        }
        if (!enabled) {
            // FrameSubmitCoordinator switches to tonemap on the next frame.
            // Keep the prepared engine and its arena for a later enable;
            // retiring them here stalls the queue and makes every toggle
            // rebuild all tables, pipelines and scratch images.
            emit("FILM_SIM_DISABLED");
            SESSION_LOGI("FILM_SIM_DISABLED");
            return true;
        }
        // An enable call carries the engine's current look (always complete),
        // so it must not depend on filmHaveLook_: a look that arrived before
        // the first enable was dropped by the engine gate, and requiring the
        // latch here would no-op the enable forever (startup race).
        if ((filmHaveLook_ || enabled) && bakedChanged && vulkanContext_.device()) {
            SESSION_LOGI("FILM_SIM_RECREATE reason=%s film=%d paper=%d", film_ ? "baked-change" : "missing",
                         bakedLook.film, bakedLook.paper);
            installPreparedFilm(prepareFilm(bakedLook, previewWidth_, previewHeight_));
        }
        return true;
    } catch (const std::exception& e) {
        filmEnabled_ = false;
        {
            std::lock_guard<std::mutex> filmLock(filmMutex_);
            film_.reset();
        }
        filmArenaW_ = 0;
        filmArenaH_ = 0;
        emit(std::string("FILM_SIM_FAILURE error=") + e.what());
        SESSION_LOGE("FILM_SIM_FAILURE error=%s", e.what());
        return false;
    }
}

std::optional<tonemap::lut::LutChain> PreviewLookController::videoRenderLut() const {
    return rawrcam::tonemap_integration::loadRenderTransformLut(colorRenderProfile_, filesDir_, importedLutProfileId_);
}

void PreviewLookController::setFilmSimEnabled(bool enabled) {
    __android_log_print(ANDROID_LOG_INFO, "RawrCamNative", "FILM_SIM_SET enabled=%d", (int)enabled);
    std::lock_guard<std::mutex> lock(transitionMutex_);
    requestedFilmEnabled_ = enabled;
    ++filmBuildGeneration_;
    if (videoSuppressed_) {
        emit("FILM_SIM_DEFERRED_DURING_VIDEO");
        return;
    }
    if (enabled && canPrepareFilm() && needsFilmRebuild(filmLook_)) {
        // Keep tonemap running until the first engine is prepared. A prepared
        // engine from a previous on/off cycle takes the immediate path below.
        publish_(false, filmLook_);
        applyFilmSimState(false, filmLook_);
        scheduleFilmBuildLocked();
        return;
    }
    publish_(enabled, filmLook_);
    if (!applyFilmSimState(enabled, filmLook_)) {
        emit("FILM_SIM enable rejected (see FILM_SIM_FAILURE)");
    }
}

void PreviewLookController::setViewfinderDivisor(uint32_t divisor) {
    __android_log_print(ANDROID_LOG_INFO, "RawrCamNative", "VIEWFINDER_DIVISOR divisor=%u", divisor);
    std::lock_guard<std::mutex> lock(transitionMutex_);
    publishDivisor_(divisor);
}

void PreviewLookController::setFilmSimLook(const spektrafilm_native::FilmLook& look) {
    std::lock_guard<std::mutex> lock(transitionMutex_);
    filmLook_ = look;
    ++filmBuildGeneration_;
    if (videoSuppressed_) return;
    if (requestedFilmEnabled_ && canPrepareFilm() && needsFilmRebuild(look)) {
        // The recorder retains the last compatible look and engine while a
        // new baked stock/paper/method is prepared on the film worker.
        scheduleFilmBuildLocked();
        return;
    }
    publish_(requestedFilmEnabled_, look);
    if (requestedFilmEnabled_ && !applyFilmSimState(true, look, /*isLookUpdate=*/true)) {
        emit("FILM_SIM look rejected (see FILM_SIM_FAILURE)");
    }
}

void PreviewLookController::scheduleFilmBuildLocked() {
    if (filmBuildStop_.load() || !canPrepareFilm()) return;
    {
        std::lock_guard<std::mutex> lock(filmBuildMutex_);
        pendingFilmBuild_ = FilmBuildRequest{filmLook_, previewWidth_, previewHeight_, filmBuildGeneration_};
    }
    filmBuildCv_.notify_one();
}

void PreviewLookController::filmBuildLoop() {
    for (;;) {
        FilmBuildRequest request{};
        {
            std::unique_lock<std::mutex> lock(filmBuildMutex_);
            filmBuildCv_.wait(lock, [this] { return filmBuildStop_.load() || pendingFilmBuild_.has_value(); });
            if (filmBuildStop_.load()) return;
            request = *pendingFilmBuild_;
            pendingFilmBuild_.reset();
        }
        try {
            auto prepared = prepareFilm(request.look, request.width, request.height);
            std::lock_guard<std::mutex> lock(transitionMutex_);
            if (filmBuildStop_.load() || request.generation != filmBuildGeneration_) continue;
            if (!preparedFilmFits(prepared)) {
                scheduleFilmBuildLocked();
                continue;
            }
            installPreparedFilm(std::move(prepared));
            if (!applyFilmSimState(requestedFilmEnabled_, filmLook_)) {
                publish_(false, filmLook_);
                emit("FILM_SIM prepared look rejected (see FILM_SIM_FAILURE)");
                continue;
            }
            publish_(requestedFilmEnabled_, filmLook_);
        } catch (const std::exception& e) {
            emit(std::string("FILM_SIM_PREPARE_FAILURE error=") + e.what());
        }
    }
}

void PreviewLookController::setQuickToneNativeValue(int target, float value) {
    if (!std::isfinite(value)) return;
    std::lock_guard<std::mutex> lock(transitionMutex_);
    if (target == 0 && value >= -100.0f && value <= 100.0f)
        tonemapParams_.shadowLiftEV = value;
    else if (target == 1 && value >= -100.0f && value <= 100.0f)
        tonemapParams_.highlightBiasEV = value;
    publishTone_(tonemapParams_);
}

void PreviewLookController::setTonemapParameters(float exposureEV, float blackPointEV, float shadowLiftEV,
                                                 float midtoneLiftEV, float contrast, float whitePointEV,
                                                 float highlightBiasEV, float saturation, float vibrance) {
    const bool finite = std::isfinite(exposureEV) && std::isfinite(blackPointEV) && std::isfinite(shadowLiftEV) &&
                        std::isfinite(midtoneLiftEV) && std::isfinite(contrast) && std::isfinite(whitePointEV) &&
                        std::isfinite(highlightBiasEV) && std::isfinite(saturation) && std::isfinite(vibrance);
    const bool inUiContract = exposureEV >= -5.0f && exposureEV <= 5.0f && blackPointEV >= -100.0f &&
                              blackPointEV <= 100.0f && shadowLiftEV >= -100.0f && shadowLiftEV <= 100.0f &&
                              midtoneLiftEV >= -100.0f && midtoneLiftEV <= 100.0f && contrast >= -100.0f &&
                              contrast <= 100.0f && whitePointEV >= -100.0f && whitePointEV <= 100.0f &&
                              highlightBiasEV >= -100.0f && highlightBiasEV <= 100.0f && saturation >= -100.0f &&
                              saturation <= 100.0f && vibrance >= -100.0f && vibrance <= 100.0f;
    if (!finite || !inUiContract) {
        LOGW("Rejected invalid tonemap control update");
        return;
    }
    std::lock_guard<std::mutex> lock(transitionMutex_);
    tonemapParams_.exposureEV = exposureEV;
    tonemapParams_.blackPointEV = blackPointEV;
    tonemapParams_.shadowLiftEV = shadowLiftEV;
    tonemapParams_.midtoneLiftEV = midtoneLiftEV;
    tonemapParams_.contrast = contrast;
    tonemapParams_.whitePointEV = whitePointEV;
    tonemapParams_.highlightBiasEV = highlightBiasEV;
    tonemapParams_.saturation = saturation;
    tonemapParams_.vibrance = vibrance;
    publishTone_(tonemapParams_);
}
}  // namespace rawrcam::pipeline
