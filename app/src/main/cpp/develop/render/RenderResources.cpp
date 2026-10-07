#include "develop/render/RenderResources.h"

#include <android/asset_manager.h>
#include <android/log.h>
#include <film_boost_milestone.h>
#include <film_develop.h>
#include <film_diffusion.h>
#include <film_dir.h>
#include <film_exposure.h>
#include <film_glow_ratio.h>
#include <film_grain.h>
#include <film_halation.h>
#include <film_input.h>
#include <film_output.h>
#include <film_output_half.h>
#include <film_printscan.h>
#include <film_scanner.h>
#include <gainmap.h>
#include <gainmap_blur.h>
#include <tonemap.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <tuple>
#include <utility>

#include "color/FilmExposure.h"
#include "develop/galosh/GaloshShaderTable.h"
#include "develop/render/FilmScratchEstimate.h"
#include "tonemap/ImportedLutProfileStore.h"
#include "vulkan/HostMemory.h"

namespace rawrcam::develop::rendered {
namespace {
void vkCheck(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string(what) + " VkResult=" + std::to_string(r));
}
}  // namespace

namespace {
uint32_t stillFilmArenaDim(uint32_t fullDim) noexcept { return ((std::max(1u, fullDim) + 15u) / 16u) * 16u; }

// Headroom above the film estimate: preview engine, ZSL ring, merge
// remnants and OS needs at build time.
constexpr std::uint64_t kFilmBuildHeadroomBytes = 768ull * 1024u * 1024u;

// /proc MemAvailable (reclaimable-aware, unlike sysinfo freeram).
// Returns 0 when unreadable; callers must fail open on 0.
std::atomic<std::uint64_t> gLastGateRequiredMb{0};
std::atomic<std::uint64_t> gLastGateAvailMb{0};

std::uint64_t readMemAvailableBytes() noexcept {
    try {
        std::ifstream in("/proc/meminfo");
        std::string key, unit;
        std::uint64_t kb = 0;
        while (in >> key >> kb >> unit) {
            if (key == "MemAvailable:") return kb * 1024u;
        }
    } catch (...) {
    }
    return 0;
}
}  // namespace
void RenderResources::createPixels(const RenderedStillContext& rendered) {
    output_ = rawrcam::vulkan::createOwnedImage(physicalDevice_, device_, rendered.width, rendered.height,
                                                VK_FORMAT_R8G8B8A8_UNORM,
                                                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    const uint64_t bytes64 = static_cast<uint64_t>(rendered.width) * rendered.height * 4u;
    if (!rendered.surfacePreview) {
        readbackBytes_ = static_cast<VkDeviceSize>(bytes64);
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = readbackBytes_;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCheck(vkCreateBuffer(device_, &bi, nullptr, &readback_), "rendered create readback buffer");
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device_, readback_, &req);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = vulkan::findCoherentHostMemoryType(physicalDevice_, req.memoryTypeBits);
        vkCheck(vkAllocateMemory(device_, &ai, nullptr, &readbackMemory_), "rendered allocate readback memory");
        vkCheck(vkBindBufferMemory(device_, readback_, readbackMemory_, 0), "rendered bind readback memory");
        vkCheck(vkMapMemory(device_, readbackMemory_, 0, readbackBytes_, 0, &mapped_), "rendered map readback memory");
    }
    if (rendered.ultraHdrEnabled) {
        const char* paramReason = nullptr;
        if (!gainmap::GainmapCompute::validateParams(rendered.gainmapParams, &paramReason))
            throw std::runtime_error(std::string("gainmap params: ") + (paramReason ? paramReason : "invalid"));
        mapWidth_ = std::max(1u, rendered.width / 2u);
        mapHeight_ = std::max(1u, rendered.height / 2u);
        map_ = rawrcam::vulkan::createOwnedImage(
            physicalDevice_, device_, mapWidth_, mapHeight_, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        const uint64_t mapBytes64 = static_cast<uint64_t>(mapWidth_) * mapHeight_ * 4u;
        mapReadbackBytes_ = static_cast<VkDeviceSize>(mapBytes64);
        VkBufferCreateInfo mbi{};
        mbi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        mbi.size = mapReadbackBytes_;
        mbi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        mbi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCheck(vkCreateBuffer(device_, &mbi, nullptr, &mapReadback_), "gainmap create readback buffer");
        VkMemoryRequirements mreq{};
        vkGetBufferMemoryRequirements(device_, mapReadback_, &mreq);
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = mreq.size;
        mai.memoryTypeIndex = vulkan::findCoherentHostMemoryType(physicalDevice_, mreq.memoryTypeBits);
        vkCheck(vkAllocateMemory(device_, &mai, nullptr, &mapReadbackMemory_), "gainmap allocate memory");
        vkCheck(vkBindBufferMemory(device_, mapReadback_, mapReadbackMemory_, 0), "gainmap bind memory");
        vkCheck(vkMapMemory(device_, mapReadbackMemory_, 0, mapReadbackBytes_, 0, &mapMapped_), "gainmap map memory");
        if (!gainmap_ || gainmapDevice_ != device_) {
            gainmap_.reset();
            gainmap::GainmapCreateInfo gci{};
            gci.context.physicalDevice = physicalDevice_;
            gci.context.device = device_;
            gci.shaderSpirv = reinterpret_cast<const uint32_t*>(gainmap_spv);
            gci.shaderSpirvBytes = gainmap_spv_size;
            gci.blurShaderSpirv = reinterpret_cast<const uint32_t*>(gainmap_blur_spv);
            gci.blurShaderSpirvBytes = gainmap_blur_spv_size;
            gci.maxFramesInFlight = 1;
            gainmap_ = std::make_unique<gainmap::GainmapCompute>(gci);
            gainmapDevice_ = device_;
        }
    }
}
bool RenderResources::ToneKey::operator==(const ToneKey& other) const noexcept {
    return std::tie(profile, lutId, strength, device) ==
           std::tie(other.profile, other.lutId, other.strength, other.device);
}
bool RenderResources::PostDemosaicKey::operator==(const PostDemosaicKey& other) const noexcept {
    return std::tie(width, height, fccSteps, normalized, edgeSigma, chromaBound, defringe, defringeEdge, defringeLuma,
                    device) == std::tie(other.width, other.height, other.fccSteps, other.normalized, other.edgeSigma,
                                        other.chromaBound, other.defringe, other.defringeEdge, other.defringeLuma,
                                        other.device);
}
double RenderResources::prepareEngines(const RenderedStillContext& rendered) {
    const auto engineBuildBegin = std::chrono::steady_clock::now();
    // Persistent engines (Settings > Experimental > Persistent Engine):
    // rebuild only on key mismatch, build-then-swap so a failed build
    // keeps the previous engine. Without persistence both are null
    // here and always rebuild — identical to the old behavior.
    const ToneKey toneKey{rendered.colorRenderProfile, rendered.importedLutProfileId, rendered.lutStrength, device_};
    const bool tonemapMatch = tonemap_ && toneKey_ == toneKey;
    if (!tonemapMatch) {
        // The render LUT is only consumed when the engine is built
        // (baked into it), so a reused engine skips the file load
        // (~350 ms per shot on device).
        auto renderLut = rawrcam::tonemap_integration::loadRenderTransformLut(rendered.colorRenderProfile, filesDir_,
                                                                              rendered.importedLutProfileId);
        emit("COLOR_RENDER_PROFILE requestId=" + std::to_string(rendered.requestId) +
             " profile=" + rawrcam::tonemap_integration::colorRenderProfileName(rendered.colorRenderProfile) +
             " lutStages=" + std::to_string(renderLut ? renderLut->stages.size() : 0));
        tonemap::TonemapCreateInfo tci{};
        tci.context.physicalDevice = physicalDevice_;
        tci.context.device = device_;
        tci.shaderSpirv = reinterpret_cast<const uint32_t*>(tonemap_spv);
        tci.shaderSpirvBytes = tonemap_spv_size;
        tci.maxFramesInFlight = 1;
        if (renderLut) renderLut->intensity = rendered.lutStrength;
        tci.lutChain = renderLut ? &*renderLut : nullptr;
        auto fresh = std::make_unique<tonemap::TonemapEngine>(tci);
        tonemap_ = std::move(fresh);
        toneKey_ = toneKey;
    }
    const std::uint32_t fccSteps = std::clamp(rendered.fccSteps, 1u, 8u);
    const float fccEdgeSigma =
        (std::isfinite(rendered.fccEdgeSigma) && rendered.fccEdgeSigma > 0.0f) ? rendered.fccEdgeSigma : 0.0f;
    const float fccChromaBound =
        (std::isfinite(rendered.fccChromaBound) && rendered.fccChromaBound > 0.0f) ? rendered.fccChromaBound : 0.0f;
    const float defringeStrength = (std::isfinite(rendered.defringeStrength) && rendered.defringeStrength > 0.0f)
                                       ? std::min(rendered.defringeStrength, 1.0f)
                                       : 0.0f;
    const float defringeEdge = (std::isfinite(rendered.defringeEdgeThreshold) && rendered.defringeEdgeThreshold > 0.0f)
                                   ? rendered.defringeEdgeThreshold
                                   : 0.02f;
    const float defringeLuma = (std::isfinite(rendered.defringeLumaFloor) && rendered.defringeLumaFloor >= 0.0f)
                                   ? rendered.defringeLumaFloor
                                   : 0.08f;
    const PostDemosaicKey postKey{rendered.width, rendered.height,  fccSteps,     rendered.normalizedFcc, fccEdgeSigma,
                                  fccChromaBound, defringeStrength, defringeEdge, defringeLuma,           device_};
    const bool postDemosaicMatch = postDemosaic_ && postKey_ == postKey;
    if (!postDemosaicMatch) {
        auto fresh = std::make_unique<rawr::post::PostDemosaicProcessor>(
            physicalDevice_, device_, queueFamily_, rendered.width, rendered.height, fccSteps, rendered.normalizedFcc,
            fccEdgeSigma, fccChromaBound, defringeStrength, defringeEdge, defringeLuma);
        postDemosaic_ = std::move(fresh);
        postKey_ = postKey;
    }
    const double engineBuildMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - engineBuildBegin).count();
    emit(std::string("STILL_RENDER_ENGINE tonemap=") + (tonemapMatch ? "reused" : "rebuilt") +
         " postDemosaic=" + (postDemosaicMatch ? "reused" : "rebuilt"));

    return engineBuildMs;
}
void RenderResources::ensureGaloshYuv() {
    if (galoshYuv_) {
        if (galoshYuvDevice_ == device_) return;
        // Device changed (teardown/recreation): destroy on the old device
        // before rebuilding. Caller guarantees no galosh work in flight
        // (worker thread, queue idle via fence).
        galoshYuv_.reset();
        galoshYuvDevice_ = VK_NULL_HANDLE;
    }
    if (!queueSubmitMutex_) throw std::logic_error("rendered galosh without queue mutex");
    galoshYuv_ =
        std::make_unique<::galosh::GaloshYuvPipeline>(physicalDevice_, device_, queueFamily_, galosh::yuvShaderMap());
    galoshYuvDevice_ = device_;
}
void RenderResources::closeFilmAssets() noexcept {
    if (filmAssets_.hanatos) {
        AAsset_close(filmAssets_.hanatos);
        filmAssets_.hanatos = nullptr;
    }
    if (filmAssets_.gamut) {
        AAsset_close(filmAssets_.gamut);
        filmAssets_.gamut = nullptr;
    }
}
void RenderResources::destroyFilm() noexcept {
    film_.reset();
    closeFilmAssets();
    filmArenaW_ = 0;
    filmArenaH_ = 0;
    filmDevice_ = VK_NULL_HANDLE;
}
bool RenderResources::ensureFilm(const RenderedStillContext& rendered) {
    auto* assetManager = assetManager_.load();
    const int idleEvictSeconds = filmIdleEvictSeconds_.load();
    if (!assetManager) {
        throw std::runtime_error("FilmSim: asset manager not set for stills");
    }
    const uint32_t fullW = stillFilmArenaDim(rendered.width);
    const uint32_t fullH = stillFilmArenaDim(rendered.height);
    const auto& look = rendered.filmLook;
    const auto now = std::chrono::steady_clock::now();
    // Idle eviction: drop the cached engine (GBs of full-res scratch) when
    // no still has used it for a while. Rebuild cost lands on the next
    // shutter press, inside an already multi-second capture — never on
    // preview. Caller guarantees no film work in flight (worker joined).
    if (film_ && idleEvictSeconds > 0 && now - lastFilmUse_ > std::chrono::seconds(idleEvictSeconds)) {
        emit("STILL_FILM_EVICTED idle_s=" +
             std::to_string(std::chrono::duration_cast<std::chrono::seconds>(now - lastFilmUse_).count()));
        __android_log_print(ANDROID_LOG_INFO, "RawrCamNative", "STILL_FILM_EVICTED");
        destroyFilm();
    }
    const bool tripleMatch = film_ && look.film == filmBaked_.film && look.paper == filmBaked_.paper &&
                             look.rgbToRawMethod == filmBaked_.rgbToRawMethod &&
                             look.inputColorSpace == filmBaked_.inputColorSpace &&
                             look.outputColorSpace == filmBaked_.outputColorSpace;
    // Conditional scratch gates allocation on these flags: a flip must
    // rebuild the engine, or record() would run an effect whose buffers
    // were never reserved (full-res scratch is gigabytes; never over-hold).
    // DIR is gated on amount crossing 0 (no enable flag; record() checks
    // dirAllocated). Shared S0/S1/S2 arena covers halation/DIR/scanner.
    const bool dirOn = look.dirCouplersAmount > 0.0f;
    const bool bakedDirOn = filmBaked_.dirCouplersAmount > 0.0f;
    const bool scratchMatch =
        !film_ || (look.halationEnabled == filmBaked_.halationEnabled &&
                   look.cameraDiffusionEnabled == filmBaked_.cameraDiffusionEnabled &&
                   look.printDiffusionEnabled == filmBaked_.printDiffusionEnabled &&
                   look.scannerEnabled == filmBaked_.scannerEnabled && look.grainEnabled == filmBaked_.grainEnabled &&
                   look.grainModel == filmBaked_.grainModel && dirOn == bakedDirOn);
    if (film_ && filmBakedTiled_ == rendered.filmTiled && filmDevice_ == device_ && tripleMatch && scratchMatch &&
        filmBakedDiagnostics_ == (rendered.filmDiagnosticTap != nullptr) && fullW <= filmArenaW_ &&
        fullH <= filmArenaH_) {
        lastFilmUse_ = now;
        // Camera-plane filtration bakes into the spectral tables but has no
        // scratch gate: refresh the tables in place instead of rebuilding.
        // Without this, a UV/IR toggle between captures would hit record()'s
        // camera-filters guard and silently fall back to tonemap.
        const spektrafilm_native::CameraFilters baked = film_->bakedCameraFilters();
        if (look.cameraUvFilterEnabled != baked.uvEnabled || look.cameraUvCutNm != baked.uvCutNm ||
            look.cameraIrFilterEnabled != baked.irEnabled || look.cameraIrCutNm != baked.irCutNm) {
            if (!std::isfinite(look.cameraUvCutNm) || !std::isfinite(look.cameraIrCutNm)) {
                throw std::runtime_error("FilmSim: non-finite camera filter cut");
            }
            spektrafilm_native::CameraFilters filters{};
            filters.uvEnabled = look.cameraUvFilterEnabled;
            filters.uvCutNm = look.cameraUvCutNm;
            filters.irEnabled = look.cameraIrFilterEnabled;
            filters.irCutNm = look.cameraIrCutNm;
            film_->updateCameraFilters(filters);
            emit("STILL_FILM_CAMERA_FILTERS updated");
        }
        return true;
    }
    // A mismatched cached engine cannot be reused. Release its GPU arena
    // before measuring available memory for the replacement.
    destroyFilm();
    // Memory gate: a cold film build reserves gigabytes (see
    // estimateFilmScratchBytes); under pressure that kills the app via LMK
    // and can freeze the device. Fail soft to tonemap instead. The reuse
    // path above allocates nothing and skips the gate.
    {
        FilmScratchGates gates{};
        gates.grainPreview = look.grainEnabled;
        gates.grainProduction = look.grainEnabled && look.grainModel != 0;
        gates.halation = look.halationEnabled;
        gates.cameraDiffusion = look.cameraDiffusionEnabled;
        gates.printDiffusion = look.printDiffusionEnabled;
        gates.scanner = look.scannerEnabled;
        gates.dir = look.dirCouplersAmount > 0.0f;
        // The tile arena reserves overlap for baked effect enables, even when
        // an individual strength is zero on this record.
        const uint32_t overlap =
            256u * (uint32_t(gates.halation) + uint32_t(gates.cameraDiffusion) + uint32_t(gates.dir) +
                    uint32_t(gates.printDiffusion) + uint32_t(gates.scanner)) +
            64u * uint32_t(gates.grainProduction);
        const uint32_t workingW = std::min(fullW, 512u + 2u * overlap);
        const uint32_t workingH = std::min(fullH, 256u + 2u * overlap);
        std::uint64_t required =
            (rendered.filmTiled ? estimateFilmTileScratchBytes(fullW, fullH, workingW, workingH, gates)
                                : estimateFilmScratchBytes(fullW, fullH, gates)) +
            kFilmBuildHeadroomBytes;
        // Film glow-factor transient (full-res RGBA16F quotient) for
        // film+UltraHDR: account it here so large stills with
        // halation/diffusion can't slip past the gate and LMK-trip mid-capture.
        // Skipped when glow is dialed off (strength 0 = pure scene tap, no
        // transient allocated below).
        if (rendered.ultraHdrEnabled && !rendered.replayBypassFcc && rendered.gainmapParams.glowStrength > 0.0f &&
            spektrafilm_native::SpektraFilm::willWriteGlowGain(
                look, rendered.width, rendered.height,
                rendered.filmTiled ? spektrafilm_native::GpuRenderTilingMode::Tiled
                                   : spektrafilm_native::GpuRenderTilingMode::LegacyFullFrame,
                rendered.filmTiled)) {
            required += uint64_t(rendered.width) * rendered.height * 8u;
        }
        const std::uint64_t avail = readMemAvailableBytes();
        gLastGateRequiredMb.store(required >> 20, std::memory_order_relaxed);
        gLastGateAvailMb.store(avail >> 20, std::memory_order_relaxed);
        emit("STILL_FILM_RAM_GATE requiredMB=" + std::to_string(required >> 20) +
             " availMB=" + std::to_string(avail >> 20));
        __android_log_print(ANDROID_LOG_INFO, "RawrCamNative", "STILL_FILM_RAM_GATE requiredMB=%llu availMB=%llu",
                            static_cast<unsigned long long>(required >> 20),
                            static_cast<unsigned long long>(avail >> 20));
        if (avail > 0 && required > avail) {
            emit("STILL_FILM_RAM_GATE_REJECT falling back to tonemap");
            __android_log_print(ANDROID_LOG_ERROR, "RawrCamNative", "STILL_FILM_RAM_GATE_REJECT");
            return false;
        }
    }
    AAsset* hanatos = AAssetManager_open(assetManager, "spektrafilm/SpektraHanatos2025Spectra.f32", AASSET_MODE_BUFFER);
    AAsset* gamut =
        AAssetManager_open(assetManager, "spektrafilm/SpektraOutputGamutCompression.f32", AASSET_MODE_BUFFER);
    if (!hanatos || !gamut) {
        if (hanatos) AAsset_close(hanatos);
        if (gamut) AAsset_close(gamut);
        throw std::runtime_error("FilmSim: missing APK assets (spektra/*.f32, check noCompress)");
    }
    filmAssets_.hanatos = hanatos;
    filmAssets_.gamut = gamut;
    spektrafilm_native::SpektraFilmCreateInfo filmCreate{};
    filmCreate.context.physicalDevice = physicalDevice_;
    filmCreate.context.device = device_;
    filmCreate.queue = queue_;
    filmCreate.queueFamilyIndex = queueFamily_;
    filmCreate.maxFramesInFlight = 1;
    filmCreate.maxWidth = fullW;
    filmCreate.maxHeight = fullH;
    filmCreate.look = look;
    // Full-res scratch is gigabytes per effect group: reserve only what
    // this look enables (scratchMatch above rebuilds on flag flips).
    filmCreate.conditionalEffectScratch = true;
    filmCreate.tiledMemorySaving = rendered.filmTiled;
    filmBakedTiled_ = rendered.filmTiled;
    filmCreate.diagnosticTransfers = rendered.filmDiagnosticTap != nullptr;
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
    filmCreate.boostMilestoneSpirv = reinterpret_cast<const uint32_t*>(film_boost_milestone_spv);
    filmCreate.boostMilestoneSpirvBytes = film_boost_milestone_spv_size;
    filmCreate.glowRatioSpirv = reinterpret_cast<const uint32_t*>(film_glow_ratio_spv);
    filmCreate.glowRatioSpirvBytes = film_glow_ratio_spv_size;
    filmCreate.hanatosSpectra = reinterpret_cast<const float*>(AAsset_getBuffer(hanatos));
    filmCreate.hanatosSpectraFloats = (size_t)AAsset_getLength64(hanatos) / sizeof(float);
    filmCreate.gamutCompression = reinterpret_cast<const float*>(AAsset_getBuffer(gamut));
    filmCreate.gamutCompressionFloats = (size_t)AAsset_getLength64(gamut) / sizeof(float);
    try {
        film_ = std::make_unique<spektrafilm_native::SpektraFilm>(filmCreate);
    } catch (...) {
        closeFilmAssets();
        throw;
    }
    filmBaked_ = look;
    filmBakedDiagnostics_ = rendered.filmDiagnosticTap != nullptr;
    filmArenaW_ = fullW;
    filmArenaH_ = fullH;
    filmDevice_ = device_;
    lastFilmUse_ = now;
    const std::string scratch = std::string("hal=") + (look.halationEnabled ? "1" : "0") +
                                " camdif=" + (look.cameraDiffusionEnabled ? "1" : "0") +
                                " prtdif=" + (look.printDiffusionEnabled ? "1" : "0") +
                                " scn=" + (look.scannerEnabled ? "1" : "0") +
                                " grn=" + (look.grainEnabled ? std::to_string(look.grainModel) : "off");
    emit("STILL_FILM_CREATED film=" + std::to_string(look.film) + " paper=" + std::to_string(look.paper) +
         " arena=" + std::to_string(fullW) + "x" + std::to_string(fullH) + " scratch=" + scratch);
    __android_log_print(ANDROID_LOG_INFO, "RawrCamNative", "STILL_FILM_CREATED film=%d paper=%d method=%d arena=%ux%u",
                        look.film, look.paper, look.rgbToRawMethod, fullW, fullH);
    return true;
}
void RenderResources::releasePixels() noexcept {
    // Engine persistence toggle: with it off (default) engines are rebuilt
    // every capture, exactly as before. With it on, TonemapEngine,
    // PostDemosaicProcessor and film survive across captures subject to the
    // match keys in start()/ensureFilm; only pixel-holding resources below
    // are always freed.
    if (!enginePersistenceEnabled_.load(std::memory_order_relaxed)) {
        tonemap_.reset();
        postDemosaic_.reset();
        // GALOSH-YUV owns hundreds of MB of transient graph memory: free
        // with the engines when persistence is off. With persistence on the
        // epoch reallocates per geometry inside process().
        galoshYuv_.reset();
        galoshYuvDevice_ = VK_NULL_HANDLE;
    } else if (galoshYuv_ && galoshYuvDevice_ != device_) {
        // Device changed under persistence (surface recreation): the pipeline
        // holds command pool/fence/descriptors on the old VkDevice.
        galoshYuv_.reset();
        galoshYuvDevice_ = VK_NULL_HANDLE;
    }
    if (device_) {
        if (mapped_ && readbackMemory_) vkUnmapMemory(device_, readbackMemory_);
        mapped_ = nullptr;
        if (mapMapped_ && mapReadbackMemory_) vkUnmapMemory(device_, mapReadbackMemory_);
        mapMapped_ = nullptr;
        if (readback_) vkDestroyBuffer(device_, readback_, nullptr);
        if (readbackMemory_) vkFreeMemory(device_, readbackMemory_, nullptr);
        if (mapReadback_) vkDestroyBuffer(device_, mapReadback_, nullptr);
        if (mapReadbackMemory_) vkFreeMemory(device_, mapReadbackMemory_, nullptr);
        rawrcam::vulkan::destroyOwnedImage(device_, output_);
        rawrcam::vulkan::destroyOwnedImage(device_, map_);
        rawrcam::vulkan::destroyOwnedImage(device_, glowGain_);
    }
    // GainmapCompute holds no per-capture GPU memory (descriptor pool only);
    // keep it across captures. It is destroyed with the processor.

    readback_ = VK_NULL_HANDLE;
    readbackMemory_ = VK_NULL_HANDLE;
    readbackBytes_ = 0;
    mapReadback_ = VK_NULL_HANDLE;
    mapReadbackMemory_ = VK_NULL_HANDLE;
    mapReadbackBytes_ = 0;
    mapWidth_ = 0;
    mapHeight_ = 0;
}
void RenderResources::bind(const RenderDeviceContext& context) noexcept {
    physicalDevice_ = context.physicalDevice;
    device_ = context.device;
    queue_ = context.queue;
    queueFamily_ = context.queueFamily;
    queueSubmitMutex_ = context.queueSubmitMutex;
    float16Compute_ = context.float16Compute;
}
RenderOutputView RenderResources::outputView() const noexcept {
    return {mapped_,    static_cast<size_t>(readbackBytes_),    output_.view,
            mapMapped_, static_cast<size_t>(mapReadbackBytes_), mapWidth_,
            mapHeight_};
}
VkImageView RenderResources::prepareGlow(uint32_t width, uint32_t height) {
    glowGain_ = vulkan::createOwnedImage(physicalDevice_, device_, width, height, VK_FORMAT_R16G16B16A16_SFLOAT,
                                         VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    return glowGain_.view;
}
void RenderResources::reset() noexcept {
    // Destroy GALOSH while device_ is still valid: the pipeline destructor
    // issues vkDestroy* on its stored VkDevice. Must precede the nulling
    // below (destroyResources keeps it alive under persistence).
    galoshYuv_.reset();
    galoshYuvDevice_ = VK_NULL_HANDLE;
    releasePixels();
    tonemap_.reset();
    postDemosaic_.reset();
    destroyFilm();
    gainmap_.reset();
    gainmapDevice_ = VK_NULL_HANDLE;
    // Assets are consumed at ensureFilm (create/rebuild) only; the cached
    // film_ never re-reads them. Close here so per-shot instances (multiframe
    // local, replay local) don't leak 2 fds per film still via the dtor.
    closeFilmAssets();
    physicalDevice_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
    queueFamily_ = 0;
    queueSubmitMutex_ = nullptr;
}
std::string lastFilmGateSummary() {
    return "need=" + std::to_string(gLastGateRequiredMb.load(std::memory_order_relaxed)) +
           "MB free=" + std::to_string(gLastGateAvailMb.load(std::memory_order_relaxed)) + "MB";
}

}  // namespace rawrcam::develop::rendered
