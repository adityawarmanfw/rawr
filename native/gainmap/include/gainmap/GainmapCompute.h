#pragma once
#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>

#include "gainmap/Types.h"

namespace gainmap {

// UltraHDR gain map encode parameters (spec hdrgm semantics).
// minLog2/maxLog2 are log2(min/max_content_boost). gamma/offsetSdr/offsetHdr
// match hdrgm:Gamma / OffsetSDR / OffsetHDR. Capacity fields are mux-only
// metadata (needed for the file, not for the per-pixel math) but carried
// here so record() and the JPEG muxer share one validated struct.
struct GainmapParams {
    // Identity floor: minLog2 = 0 (1x). A gain map must never darken the
    // base rendition - sub-identity quotients in shadows encode the SDR
    // tone-curve toe, not real headroom (the shadow fade already pulls them
    // to identity; the floor guarantees it everywhere).
    float minLog2 = 0.0f;
    float maxLog2 = 4.7090998f;    // log2(~26x)
    float gamma = 1.0f;
    float offsetSdr = 0.015625f;  // 1/64
    float offsetHdr = 0.015625f;  // 1/64
    // Linear scene-exposure gain applied to the HDR tap before the CST so it
    // shares the SDR base's exposure domain (tonemap: aePostGain * 2^EV;
    // film: 2^folded film EV). The tap is pre-exposure sensor linear; without
    // this, regions clipped at 1.0 in both domains ratio to ~1x and HDR
    // highlights render flat. Mux metadata does not carry this (curve only).
    float hdrExposure = 1.0f;
    // Linear gain assigned to sensor-clipped texels (clip mask nonzero). The
    // HDR tap pins clipped blocks at sensor white, so the measured ratio can
    // only ever reach hdrExposure there. A positive boost renders them
    // brighter than the measured headroom (specular dazzle), but any uniform
    // target flattens large clipped washes into a hard-edged disc: the rim
    // with the ratio surroundings can never be feathered away for a big
    // core. Default 0 (disabled: pure measured ratio everywhere) gives the
    // smooth natural rolloff - the ratio already carries genuine recovered
    // headroom above hdrExposure where detail exists. Set positive only for
    // the dazzle effect on detail-less cores.
    float clipBoost = 0.0f;
    // Chroma protection: a multiplicative single-channel gain pushes
    // saturated colors into panel clip (warm lamp renders white). Attenuates
    // logGain where the SDR base is bright AND saturated so colored lights
    // stay colored; neutral highlights keep full pop. 0 = off, 1 = saturated
    // brights pinned near SDR brightness. Default 0.6. Encode-only (like
    // hdrExposure); the file mux never sees it.
    float satProtect = 0.6f;
    // Multi-channel (RGB) map: per-channel ratios instead of the luminance
    // ratio, so hue survives a white SDR base (warm lamp stays yellow).
    // Metadata uses one shared parameter set for all JPEG components.
    // Default on (Photos verdict).
    bool multiChannelMap = true;
    // Film glow factor (see GlowImageView): dimensionless post/pre scatter
    // quotient, ~1.0 where the film look adds no glow. Multiplies the HDR
    // tap before the CST so halation/camera-diffusion glow carries HDR
    // headroom with hue instead of suppressing the gain. G=1 never dims the
    // core; clamping at glowMax keeps dark-neighbor bleed (halation onto
    // dark rocks) bounded; strength scales the effect (0 = pure scene tap).
    // Encode-only like hdrExposure/satProtect; the file mux never sees it.
    float glowStrength = 0.5f;
    // Upper clamp on the per-channel glow factor before strength applies.
    // Must be >= 1. Default 2.0 (glow can add at most ~1 stop pre-strength).
    float glowMax = 2.0f;
    // Post-encode map blur (separable Gaussian, map texels): melts
    // quantization contouring and residual kinks the map grid preserves
    // (recovery/tap C1 discontinuities transmit straight through the ratio).
    // 0 = off (blur passes skipped). Default 0: full-res A/B shows unblurred
    // maps are sharper with no halo lift at dark edges; raise only for
    // banding-prone smooth gradients. Encode-only; the mux never sees it.
    float mapBlurSigma = 0.0f;
    float hdrCapacityMin = 0.0f;
    float hdrCapacityMax = 4.7090998f;
    // Full-resolution map grid (1 map texel per base pixel) instead of the
    // legacy half-res grid. The shader's 2x2 flatness probe and the half-res
    // clip mask are resolution-aware (probe keeps a 2x2 footprint, clip is
    // sampled at gid/2), so output is identical apart from grid density.
    // Default true: full-res A/B shows sharper maps with no extra halo.
    bool fullResMap = true;
    // Optional capture-color sidecar. The caller allocates the RGBA8 map
    // image with twice the logical map height. The lower plane stores
    // R/(R+G+B), G/(R+G+B) as two UNORM16 values, little-endian byte pairs.
    // 0xffffffff means no usable HDR color. The upper plane remains the
    // ordinary wide-range map; blur only touches that plane. Not JPEG data.
    bool writeHdrChromaticity = false;
    // The SDR render's adaptive tone curve (tonemap/AdaptiveTone.h: lift,
    // key, highlight EV), applied to the HDR tap as the same luminance gain
    // so the map encodes headroom only, never an inverse of the shadow lift.
    // adaptiveToneLumaScale maps the exposed HDR tap's AP1 luminance into the
    // curve's coordinates (aePostGain / hdrExposure). liftEv 0 = off.
    float adaptiveTone[3] = {0.0f, -4.0f, 0.0f};
    float adaptiveToneLumaScale = 1.0f;
    // Row-major 3x3 HDR-input -> linear-sRGB. The HDR input is white-balanced
    // camera RGB, so this must be the calibrated camera->linear-sRGB matrix,
    // supplied per capture (see RenderedStillContext::gainmapCstRowMajor).
    // Default is AP1->sRGB (fits only an AP1 working space); tests may
    // override with identity.
    float hdrToLinearSrgbRowMajor[9] = {1.70505154f, -0.62179068f, -0.08325840f, -0.13025714f, 1.14080269f,
                                        -0.01054853f, -0.02400328f, -0.12896877f, 1.15297184f};
};

struct GainmapCreateInfo {
    VulkanContext context{};
    const uint32_t* shaderSpirv = nullptr;
    size_t shaderSpirvBytes = 0;
    // Blur stage (separable Gaussian over the encoded map). Null/empty
    // disables the blur pass (mapBlurSigma is then ignored).
    const uint32_t* blurShaderSpirv = nullptr;
    size_t blurShaderSpirvBytes = 0;
    uint32_t maxFramesInFlight = 1;
};

struct GainmapRecordInfo {
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    HdrImageView hdr{};
    SdrImageView sdr{};
    MapImageView map{};
    // Optional sensor clip mask (half-res R16UI, same grid as map). When null
    // the engine binds an internal all-clear texel and the boost is off.
    ClipImageView clip{};
    // Optional film glow factor (full-res RGBA16F, same grid as hdr). When
    // null the engine binds an internal 1.0 texel and the tap stays pure
    // scene (dim match requires hdr dims; record() rejects mismatches).
    GlowImageView glow{};
    const float* hdrToLinearSrgbRowMajor3x3 = nullptr;  // optional override
    GainmapParams params{};
    uint32_t frameSlot = 0;
};

class GainmapCompute {
   public:
    static bool validateParams(const GainmapParams& params, const char** reason = nullptr) noexcept;

    explicit GainmapCompute(const GainmapCreateInfo& createInfo);
    ~GainmapCompute();
    GainmapCompute(const GainmapCompute&) = delete;
    GainmapCompute& operator=(const GainmapCompute&) = delete;

    // Caller owns all image layout transitions to GENERAL. Map dimensions
    // must equal floor(base/scale): full base size when
    // GainmapParams::fullResMap is set, half size legacy. The shader
    // bilinearly samples both inputs at any density; the clip mask stays
    // half-res and is sampled at gid/2.
    void record(const GainmapRecordInfo& recordInfo);

   private:
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace gainmap
