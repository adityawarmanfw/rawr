// GPU scene exposure histogram vs the CPU reference on an rgba16f image.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "image_scopes/image_scopes.h"
#include "image_scopes_cpu.h"
#include "vulkan_test_context.hpp"
using namespace image_scopes;
namespace t = image_scopes::tests;
static void require(bool v, const char* m) {
    if (!v) throw std::runtime_error(m);
}
static std::uint16_t toHalf(float f) {
    std::uint32_t x;
    std::memcpy(&x, &f, 4);
    const std::uint32_t sign = (x >> 16) & 0x8000u;
    const int exp = int((x >> 23) & 0xffu) - 127 + 15;
    std::uint32_t mant = x & 0x7fffffu;
    if (exp >= 31) return std::uint16_t(sign | 0x7c00u);
    if (exp <= 0) {
        if (exp < -10) return std::uint16_t(sign);
        mant |= 0x800000u;
        const int shift = 14 - exp;
        std::uint32_t h = mant >> shift;
        if ((mant >> (shift - 1)) & 1u) ++h;
        return std::uint16_t(sign | h);
    }
    std::uint32_t h = sign | (std::uint32_t(exp) << 10) | (mant >> 13);
    if (mant & 0x1000u) ++h;
    return std::uint16_t(h);
}
static float fromHalf(std::uint16_t h) {
    const std::uint32_t sign = std::uint32_t(h & 0x8000u) << 16;
    const std::uint32_t exp = (h >> 10) & 0x1fu, mant = h & 0x3ffu;
    float v;
    if (exp == 0)
        v = std::ldexp(float(mant), -24);
    else if (exp == 31)
        v = mant ? NAN : INFINITY;
    else
        v = std::ldexp(float(mant | 0x400u), int(exp) - 25);
    std::uint32_t bits;
    std::memcpy(&bits, &v, 4);
    bits |= sign;
    std::memcpy(&v, &bits, 4);
    return v;
}

int main() {
    std::cout.setf(std::ios::unitbuf);
    try {
        // CPU binning contract.
        require(cpu::sceneExposureBin(0.0f) == 0u, "zero luma must use bin 0");
        require(cpu::sceneExposureBin(1.0f) == 213u, "luma 1.0 must land at (0+20)*256/24");
        require(cpu::sceneExposureBin(1.0e9f) == kSceneExposureBins - 2u, "bright luma must stop below clip bin");
        require(cpu::sceneExposureBin(1.0e-12f) == 0u, "tiny luma must clamp to bin 0");

        t::VulkanTestContext vk;
        std::cout << "Using GPU: " << vk.deviceName() << "\n";
        constexpr std::uint32_t W = 203, H = 151;
        std::vector<std::uint16_t> half(std::size_t(W) * H * 4);
        std::vector<float> ref(half.size());
        for (std::uint32_t y = 0; y < H; ++y)
            for (std::uint32_t x = 0; x < W; ++x) {
                // Log ramp over about -18..+2 EV with channel tint, a dark
                // corner of zeros and a clipped sky band at the top.
                const float ev = -18.0f + 20.0f * float(x) / float(W - 1) + 0.5f * std::sin(float(y) * 0.37f);
                const float base = std::exp2(ev);
                float rgb[3] = {base * 1.2f, base, base * 0.7f};
                if (x < 12 && y > H - 12) rgb[0] = rgb[1] = rgb[2] = 0.0f;
                if (y < H / 5) rgb[0] = rgb[1] = rgb[2] = 6.0f;
                // A saturated red light: luma under the clip, red over its ceiling.
                if (y >= H / 2 && y < H / 2 + 20 && x >= W / 2 && x < W / 2 + 40) {
                    rgb[0] = 3.0f;
                    rgb[1] = rgb[2] = 0.2f;
                }
                for (int c = 0; c < 3; ++c) {
                    const auto i = (std::size_t(y) * W + x) * 4 + c;
                    half[i] = toHalf(rgb[c]);
                    ref[i] = fromHalf(half[i]);
                }
                const auto a = (std::size_t(y) * W + x) * 4 + 3;
                half[a] = toHalf(1.0f);
                ref[a] = 1.0f;
            }
        auto im = vk.createRgbaImage(W, H, VK_FORMAT_R16G16B16A16_SFLOAT);
        vk.uploadRgba(im, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(half.data()),
                                                        half.size() * sizeof(std::uint16_t)));
        ImageScopes scopes({{vk.physicalDevice(), vk.device(), nullptr}, 2});
        auto readback = vk.createBuffer(sizeof(SceneExposureStatsData), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        const float luma[3] = {0.3f, 0.6f, 0.1f};
        const float clipLuma = 4.0f;
        const float ceilings[3] = {2.0f, 1.0f, 1.5f};
        const float focus[4] = {0.55f, 0.3f, 0.8f, 0.7f};
        const auto measure = [&](std::uint32_t block, bool spot) {
            vk.beginCommands();
            SceneExposureStatsRecordInfo info{};
            info.commandBuffer = vk.commandBuffer();
            info.input = {im.view, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_LAYOUT_GENERAL, W, H};
            info.frameSlot = 1;
            info.sampleBlockSize = block;
            std::memcpy(info.lumaWeights, luma, sizeof(luma));
            info.clipLuma = clipLuma;
            std::memcpy(info.channelCeilings, ceilings, sizeof(ceilings));
            info.focusX0 = focus[0];
            info.focusY0 = focus[1];
            info.focusX1 = focus[2];
            info.focusY1 = focus[3];
            if (spot) {
                info.spotX0 = info.spotY0 = 0.4f;
                info.spotX1 = info.spotY1 = 0.6f;
            }
            scopes.recordSceneExposureStats(info);
            scopes.recordCopySceneExposureStats(vk.commandBuffer(), 1, readback.buffer, 0);
            vk.submitAndWait();
            scopes.retireFrameSlot(1);
            SceneExposureStatsData gpu{};
            vk.invalidate(readback);
            std::memcpy(&gpu, vk.map(readback), sizeof(gpu));
            vk.unmap(readback);
            return gpu;
        };
        for (std::uint32_t block : {4u, 2u}) {
            const auto gpu = measure(block, false);
            const auto cpu =
                cpu::measureSceneExposure({ref.data(), W, H, W * 4u}, luma, clipLuma, block, ceilings, focus);
            require(gpu.focusSamples == cpu.focusSamples && gpu.focusSamples > 0u &&
                        gpu.focusSamples < gpu.sampleCount / 4u,
                    "focus samples mismatch");
            std::uint64_t focusDiff = 0, focusSum = 0;
            for (std::uint32_t b = 0; b < kSceneExposureBins; ++b) {
                focusDiff += std::uint64_t(std::llabs(std::int64_t(gpu.focusBins[b]) - cpu.focusBins[b]));
                focusSum += gpu.focusBins[b];
            }
            require(focusSum == gpu.focusSamples, "focus bins do not sum to focus samples");
            std::uint32_t zoneSamples = 0;
            for (std::uint32_t z = 0; z < kSceneZoneCount; ++z) {
                require(gpu.zoneSamples[z] == cpu.zoneSamples[z] && gpu.zoneClipped[z] == cpu.zoneClipped[z],
                        "zone counts mismatch");
                require(std::abs(double(gpu.zoneLogSum[z]) - double(cpu.zoneLogSum[z])) <=
                            double(gpu.zoneSamples[z]) * 0.01 + 1.0,
                        "zone log sum differs from CPU reference");
                zoneSamples += gpu.zoneSamples[z];
            }
            require(zoneSamples == gpu.frameSamples, "zones must cover the frame");
            // The sky band (top fifth) clips the top-left zone; the dark
            // bottom-left zone (left end of the ramp) does not clip.
            const std::uint32_t bottomLeft = kSceneZoneCount - kSceneZoneCols;
            require(gpu.zoneClipped[0] == gpu.zoneSamples[0] && gpu.zoneClipped[bottomLeft] == 0,
                    "zone clipping misplaced");
            require(*sceneZoneLogMeanEv(gpu, 0) > *sceneZoneLogMeanEv(gpu, bottomLeft) + 8.0f,
                    "zone means misplaced");
            require(focusDiff * 200u <= focusSum + 200u, "focus histogram differs from CPU reference");
            std::uint64_t diff = 0, sum = 0;
            for (std::uint32_t b = 0; b < kSceneExposureBins; ++b) {
                diff += std::uint64_t(std::llabs(std::int64_t(gpu.channelBins[b]) - cpu.channelBins[b]));
                sum += gpu.channelBins[b];
            }
            require(sum == gpu.totalWeight, "channel bins do not sum to total weight");
            require(diff * 200u <= sum, "channel histogram differs from CPU reference by more than 0.5%");
            require(gpu.channelBins[kSceneExposureBins - 1] > gpu.weightedBins[kSceneExposureBins - 1],
                    "a saturated colour must clip the channel histogram but not luma");
            require(gpu.frameSamples == cpu.frameSamples && gpu.frameSamples == gpu.sampleCount,
                    "frame samples mismatch");
            // log2 precision may round a sample differently; the mean must match.
            require(std::abs(double(gpu.frameLogSum) - double(cpu.frameLogSum)) <= double(gpu.frameSamples) * 0.01,
                    "frame log sum differs from CPU reference");
            const auto spot = measure(block, true);
            require(spot.sampleCount < gpu.sampleCount / 10u && spot.sampleCount > 0, "spot must restrict the bins");
            require(spot.frameSamples == gpu.frameSamples && spot.frameLogSum == gpu.frameLogSum,
                    "the frame log sum must cover the whole frame under a spot");
            require(spot.focusSamples == gpu.focusSamples, "the focus bins must not depend on a spot");
            std::cout << "SCENE_CHANNEL_PARITY block=" << block << " binDiff=" << diff << " frameLogMean="
                      << *sceneFrameLogMeanEv(gpu) << "\n";
        }
        for (std::uint32_t block : {4u, 2u}) {
            vk.beginCommands();
            SceneExposureStatsRecordInfo info{};
            info.commandBuffer = vk.commandBuffer();
            info.input = {im.view, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_LAYOUT_GENERAL, W, H};
            info.frameSlot = 1;
            info.sampleBlockSize = block;
            std::memcpy(info.lumaWeights, luma, sizeof(luma));
            info.clipLuma = clipLuma;
            scopes.recordSceneExposureStats(info);
            scopes.recordCopySceneExposureStats(vk.commandBuffer(), 1, readback.buffer, 0);
            vk.submitAndWait();
            scopes.retireFrameSlot(1);
            SceneExposureStatsData gpu{};
            vk.invalidate(readback);
            std::memcpy(&gpu, vk.map(readback), sizeof(gpu));
            vk.unmap(readback);
            const auto cpu = cpu::measureSceneExposure({ref.data(), W, H, W * 4u}, luma, clipLuma, block);
            require(gpu.sampleCount == cpu.sampleCount, "scene sample count mismatch");
            require(gpu.totalWeight == cpu.totalWeight, "scene total weight mismatch");
            require(gpu.clippedWeight == cpu.clippedWeight, "scene clipped weight mismatch");
            require(gpu.clippedWeight > 0 && gpu.weightedBins[kSceneExposureBins - 1] == gpu.clippedWeight,
                    "clipped samples must fill the top bin");
            require(gpu.totalWeight > gpu.sampleCount, "centre weighting missing");
            std::uint64_t diff = 0, sum = 0;
            for (std::uint32_t b = 0; b < kSceneExposureBins; ++b) {
                diff += std::uint64_t(std::llabs(std::int64_t(gpu.weightedBins[b]) - cpu.weightedBins[b]));
                sum += gpu.weightedBins[b];
            }
            require(sum == gpu.totalWeight, "scene bins do not sum to total weight");
            // log2 precision may move a sample across a bin edge; nothing else may differ.
            require(diff * 200u <= sum, "scene histogram differs from CPU reference by more than 0.5%");
            std::cout << "SCENE_EXPOSURE_PARITY block=" << block << " samples=" << gpu.sampleCount
                      << " weight=" << gpu.totalWeight << " binDiff=" << diff << "\n";
        }
        {
            bool rejected = false;
            vk.beginCommands();
            SceneExposureStatsRecordInfo bad{};
            bad.commandBuffer = vk.commandBuffer();
            bad.input = {im.view, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_GENERAL, W, H};
            try {
                scopes.recordSceneExposureStats(bad);
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            vk.submitAndWait();
            scopes.retireFrameSlot(0);
            require(rejected, "scene stats accepted a non-float input");
        }
        vk.destroyBuffer(readback);
        vk.destroyRgbaImage(im);
        std::cout << "SCENE_EXPOSURE_VALIDATION_PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
