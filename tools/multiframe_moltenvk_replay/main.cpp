#include <rawr/raw_gpu_pipeline/AndroidBurstCoordinator.h>
#include <rawr/raw_gpu_pipeline/ReplayMetadata.h>
#include <rawr/zsl_codec/ZslDecoder.h>
#include <rawr/zsl_container/ZslContainer.h>
#include <raw_sharpness/raw_sharpness_types.hpp>
#include <tinydng.h>

#include "color/ColorCalibration.h"
#include "color/ColorMath.h"
#include "renderer/DngSource.h"
#include "vk_test_common.hpp"
#include "SyntheticBurst.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

struct Commands {
    VkDevice device{};
    VkCommandPool pool{};
    VkCommandBuffer command{};
    VkFence fence{};
    explicit Commands(const vktest::Ctx& c) : device(c.dev) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = c.qf;
        vktest::ck(vkCreateCommandPool(device, &pi, nullptr, &pool), "command pool");
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        vktest::ck(vkAllocateCommandBuffers(device, &ai, &command), "command buffer");
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vktest::ck(vkCreateFence(device, &fi, nullptr, &fence), "fence");
    }
    ~Commands() {
        reset();
    }
    void reset() {
        if (fence) vkDestroyFence(device, fence, nullptr);
        if (pool) vkDestroyCommandPool(device, pool, nullptr);
        fence = VK_NULL_HANDLE;
        command = VK_NULL_HANDLE;
        pool = VK_NULL_HANDLE;
        device = VK_NULL_HANDLE;
    }
    template <typename F> void submit(VkQueue queue, F&& record) {
        vktest::ck(vkResetCommandBuffer(command, 0), "reset command");
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vktest::ck(vkBeginCommandBuffer(command, &bi), "begin command");
        record(command);
        vktest::ck(vkEndCommandBuffer(command), "end command");
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &command;
        vktest::ck(vkResetFences(device, 1, &fence), "reset fence");
        vktest::ck(vkQueueSubmit(queue, 1, &si, fence), "queue submit");
        vktest::ck(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "fence wait");
    }
};

std::string tsv(const std::string& metadata, const std::string& key) {
    const std::string prefix = key + "\t";
    const auto at = metadata.find(prefix);
    if (at == std::string::npos || (at != 0 && metadata[at - 1] != '\n')) return {};
    const auto begin = at + prefix.size();
    const auto end = metadata.find('\n', begin);
    return metadata.substr(begin, end == std::string::npos ? end : end - begin);
}

std::array<float, 4> floats4(const std::string& text, std::array<float, 4> fallback) {
    std::istringstream in(text);
    char comma{};
    for (float& value : fallback) {
        if (!(in >> value)) return fallback;
        in >> comma;
    }
    return fallback;
}

rawrcam::metadata::Matrix3x3 matrix3(const std::string& text) {
    rawrcam::metadata::Matrix3x3 out{};
    std::istringstream in(text);
    int valid = 0;
    char comma{};
    if (!(in >> valid)) return out;
    in >> comma;
    for (float& value : out.rowMajor) {
        if (!(in >> value)) return {};
        in >> comma;
    }
    out.valid = valid != 0;
    return out;
}

rawrcam::metadata::FrameMetadataSnapshot frameMetadata(const std::string& metadata) {
    rawrcam::metadata::FrameMetadataSnapshot out{};
    out.blackLevelPhysicalRggb = floats4(tsv(metadata, "blackLevelPhysicalRggb"), {64, 64, 64, 64});
    out.colorCorrectionGainsRggb = floats4(tsv(metadata, "colorCorrectionGainsRggb"), {1, 1, 1, 1});
    const auto white = tsv(metadata, "effectiveWhiteLevel");
    out.effectiveWhiteLevel = white.empty() ? 1023.f : std::stof(white);
    const auto neutral = tsv(metadata, "neutralColorPoint");
    if (!neutral.empty()) {
        std::istringstream in(neutral);
        int valid = 0;
        char comma{};
        in >> valid >> comma >> out.neutralColorPoint[0] >> comma >> out.neutralColorPoint[1] >> comma >>
            out.neutralColorPoint[2];
        out.hasNeutralColorPoint = valid != 0;
    }
    out.colorCorrectionTransform = matrix3(tsv(metadata, "colorCorrectionTransform"));
    auto camera = std::make_shared<rawrcam::metadata::CameraContextMetadata>();
    const auto illuminant1 = tsv(metadata, "referenceIlluminant1");
    const auto illuminant2 = tsv(metadata, "referenceIlluminant2");
    camera->color.referenceIlluminant1 = illuminant1.empty() ? -1 : std::stoi(illuminant1);
    camera->color.referenceIlluminant2 = illuminant2.empty() ? -1 : std::stoi(illuminant2);
    camera->color.colorTransform1 = matrix3(tsv(metadata, "colorTransform1"));
    camera->color.colorTransform2 = matrix3(tsv(metadata, "colorTransform2"));
    camera->color.calibrationTransform1 = matrix3(tsv(metadata, "calibrationTransform1"));
    camera->color.calibrationTransform2 = matrix3(tsv(metadata, "calibrationTransform2"));
    camera->color.forwardMatrix1 = matrix3(tsv(metadata, "forwardMatrix1"));
    camera->color.forwardMatrix2 = matrix3(tsv(metadata, "forwardMatrix2"));
    out.cameraContext = std::move(camera);
    return out;
}

rawr::raw_gpu_pipeline::MultiframeFrameParameters parameters(
    const rawrcam::metadata::FrameMetadataSnapshot& metadata) {
    rawr::raw_gpu_pipeline::MultiframeFrameParameters out{};
    out.normalization.blackByPhase = metadata.blackLevelPhysicalRggb;
    out.normalization.whiteLevel = metadata.effectiveWhiteLevel;
    const auto& gains = metadata.colorCorrectionGainsRggb;
    out.whiteBalance = {gains[0], .5f * (gains[1] + gains[2]), gains[3]};
    return out;
}

std::uint16_t floatToHalf(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const std::uint32_t sign = (bits >> 16u) & 0x8000u;
    const int exponent = static_cast<int>((bits >> 23u) & 0xffu) - 112;
    const std::uint32_t mantissa = bits & 0x7fffffu;
    if (exponent <= 0) {
        if (exponent < -10) return static_cast<std::uint16_t>(sign);
        const std::uint32_t m = (mantissa | 0x800000u) >> static_cast<std::uint32_t>(1 - exponent);
        return static_cast<std::uint16_t>(sign | (m >> 13u));
    }
    if (exponent >= 31) return static_cast<std::uint16_t>(sign | 0x7bffu);
    return static_cast<std::uint16_t>(sign | (static_cast<std::uint32_t>(exponent) << 10u) | (mantissa >> 13u));
}

void writeOfflineInput(const std::filesystem::path& outputDir, const std::string& inputPath,
                       std::uint32_t cfa, std::uint32_t width, std::uint32_t height,
                       const rawrcam::metadata::FrameMetadataSnapshot& metadata, const void* rgba16f,
                       const std::string& suffix) {
    std::filesystem::create_directories(outputDir);
    std::string stem = std::filesystem::path(inputPath).stem().string();
    if (!suffix.empty()) stem += "." + suffix;
    const auto taggedPath = outputDir / (stem + ".tagged.rgba16f");
    const auto configPath = outputDir / (stem + ".txt");
    const auto* pixels = static_cast<const std::uint16_t*>(rgba16f);
    std::ofstream tagged(taggedPath, std::ios::binary | std::ios::trunc);
    if (!tagged) throw std::runtime_error("cannot write " + taggedPath.string());
    const auto sample = [&](std::uint32_t x, std::uint32_t y, std::uint32_t channel) {
        const std::uint16_t bits = pixels[(static_cast<std::size_t>(y) * width + x) * 4u + channel];
        const float value = vktest::halfToFloat(bits);
        std::uint16_t magnitude = bits & 0x7fffu;
        if (!std::isfinite(value) || value <= 0.f) magnitude = 0u;
        else if (value >= 1.f) magnitude = 0x3c00u;
        if (value >= .995f) magnitude |= 0x8000u;
        return magnitude;
    };
    for (std::uint32_t y = 0; y < height; y += 2u) {
        for (std::uint32_t x = 0; x < width; x += 2u) {
            std::array<std::array<std::uint32_t, 2>, 4> q{};
            if (cfa == 0u) q = {{{x, y}, {x + 1u, y}, {x, y + 1u}, {x + 1u, y + 1u}}};
            else if (cfa == 1u) q = {{{x + 1u, y}, {x, y}, {x + 1u, y + 1u}, {x, y + 1u}}};
            else if (cfa == 2u) q = {{{x, y + 1u}, {x + 1u, y + 1u}, {x, y}, {x + 1u, y}}};
            else q = {{{x + 1u, y + 1u}, {x, y + 1u}, {x + 1u, y}, {x, y}}};
            const std::array<std::uint16_t, 4> cell{{sample(q[0][0], q[0][1], 0u),
                                                      sample(q[1][0], q[1][1], 1u),
                                                      sample(q[2][0], q[2][1], 1u),
                                                      sample(q[3][0], q[3][1], 2u)}};
            tagged.write(reinterpret_cast<const char*>(cell.data()), sizeof(cell));
        }
    }
    tagged.close();
    const auto color = rawrcam::color::deriveFrameColorTransform(metadata, rawrcam::color::PreviewColorMode::Auto);
    const auto working = rawrcam::color::math::multiply(rawrcam::color::math::kLinearSrgbToAcesAp1,
                                                        color.cameraToLinearSrgbRowMajor);
    const auto columnMajor = rawrcam::color::math::toColumnMajor(working);
    const auto& gains = color.baselineWbRggb;
    std::ofstream config(configPath, std::ios::trunc);
    if (!config) throw std::runtime_error("cannot write " + configPath.string());
    config << "source=" << inputPath << '\n'
           << "width=" << width << "\nheight=" << height << "\ncfa=" << cfa << '\n'
           << "fccSteps=2\ndualAutoContrast=1\ndualContrastPercent=20\nexposureEV=0\n"
           << "wbRgb=" << gains[0] << ',' << .5f * (gains[1] + gains[2]) << ',' << gains[3] << '\n'
           << "cameraToWorkingColumnMajor=";
    for (std::size_t i = 0; i < columnMajor.size(); ++i) config << (i ? "," : "") << columnMajor[i];
    config << '\n';
    std::cout << "OFFLINE_INPUT tagged=" << taggedPath << " config=" << configPath
              << " color_source=" << color.source << '\n';
}

// Packs a decoded base RAW16 mosaic (full-res R16_UINT) into the same tagged
// half-res RGBA16F + txt layout as merged output, so the offline demosaic +
// tonemap renders base and merged through the identical path. Normalization
// mirrors raw_normalize.comp: (raw - blackByPhase) / max(1, white - black).
void writeBaseOfflineInput(const std::filesystem::path& outputDir, const std::string& inputPath,
                           std::uint32_t cfa, std::uint32_t width, std::uint32_t height,
                           const rawrcam::metadata::FrameMetadataSnapshot& metadata,
                           const std::uint16_t* raw16, const std::string& stemSuffix = "base",
                           const std::string& sourceTag = ":base") {
    std::filesystem::create_directories(outputDir);
    const std::string stem = std::filesystem::path(inputPath).stem().string() + "." + stemSuffix;
    const auto taggedPath = outputDir / (stem + ".tagged.rgba16f");
    const auto configPath = outputDir / (stem + ".txt");
    const float white = metadata.effectiveWhiteLevel;
    const auto& black = metadata.blackLevelPhysicalRggb;
    std::ofstream tagged(taggedPath, std::ios::binary | std::ios::trunc);
    if (!tagged) throw std::runtime_error("cannot write " + taggedPath.string());
    const auto norm = [&](std::uint32_t x, std::uint32_t y) {
        const std::uint32_t phase = (y & 1u) * 2u + (x & 1u);
        // Bundle CFA is RGGB-ordered phases; remap for non-RGGB like the merge sampler.
        std::uint32_t phaseForCfa = phase;
        if (cfa == 1u) phaseForCfa = (phase == 0u ? 1u : (phase == 1u ? 0u : (phase == 2u ? 3u : 2u)));
        else if (cfa == 2u) phaseForCfa = (phase == 0u ? 2u : (phase == 1u ? 3u : (phase == 2u ? 0u : 1u)));
        else if (cfa == 3u) phaseForCfa = 3u - phase;
        const float b = black[phaseForCfa];
        const float den = std::max(1.f, white - b);
        const float raw = static_cast<float>(raw16[static_cast<std::size_t>(y) * width + x]);
        float v = (raw - b) / den;
        if (!std::isfinite(v) || v <= 0.f) v = 0.f;
        if (v > 1.f) v = 1.f;
        std::uint16_t mag = floatToHalf(v);
        // Match merged-path tagging: magnitude cleared for <=0, sign bit = near-clip flag.
        if (v <= 0.f) mag = 0u;
        else mag &= 0x7fffu;
        if (v >= .995f) mag |= 0x8000u;
        return mag;
    };
    for (std::uint32_t y = 0; y < height; y += 2u) {
        for (std::uint32_t x = 0; x < width; x += 2u) {
            std::array<std::array<std::uint32_t, 2>, 4> q{};
            if (cfa == 0u) q = {{{x, y}, {x + 1u, y}, {x, y + 1u}, {x + 1u, y + 1u}}};
            else if (cfa == 1u) q = {{{x + 1u, y}, {x, y}, {x + 1u, y + 1u}, {x, y + 1u}}};
            else if (cfa == 2u) q = {{{x, y + 1u}, {x + 1u, y + 1u}, {x, y}, {x + 1u, y}}};
            else q = {{{x + 1u, y + 1u}, {x, y + 1u}, {x + 1u, y}, {x, y}}};
            const std::array<std::uint16_t, 4> cell{
                {norm(q[0][0], q[0][1]), norm(q[1][0], q[1][1]), norm(q[2][0], q[2][1]), norm(q[3][0], q[3][1])}};
            tagged.write(reinterpret_cast<const char*>(cell.data()), sizeof(cell));
        }
    }
    tagged.close();
    const auto color = rawrcam::color::deriveFrameColorTransform(metadata, rawrcam::color::PreviewColorMode::Auto);
    const auto working = rawrcam::color::math::multiply(rawrcam::color::math::kLinearSrgbToAcesAp1,
                                                        color.cameraToLinearSrgbRowMajor);
    const auto columnMajor = rawrcam::color::math::toColumnMajor(working);
    const auto& gains = color.baselineWbRggb;
    std::ofstream config(configPath, std::ios::trunc);
    if (!config) throw std::runtime_error("cannot write " + configPath.string());
    config << "source=" << inputPath << sourceTag << '\n'
           << "width=" << width << "\nheight=" << height << "\ncfa=" << cfa << '\n'
            << "fccSteps=2\ndualAutoContrast=1\ndualContrastPercent=20\nexposureEV=0\n"
           << "wbRgb=" << gains[0] << ',' << .5f * (gains[1] + gains[2]) << ',' << gains[3] << '\n'
           << "cameraToWorkingColumnMajor=";
    for (std::size_t i = 0; i < columnMajor.size(); ++i) config << (i ? "," : "") << columnMajor[i];
    config << '\n';
    std::cout << "OFFLINE_INPUT tagged=" << taggedPath << " config=" << configPath
              << " color_source=" << color.source << " kind=" << stemSuffix << '\n';
}

void imageBarrier(VkCommandBuffer command, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                  VkAccessFlags srcAccess, VkAccessFlags dstAccess, VkPipelineStageFlags srcStage,
                  VkPipelineStageFlags dstStage) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(command, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

// Write one decoded RAW16 frame as a minimal standalone DNG (no opcodes /
// lens shading) so external burst tools (e.g. hdr-plus-swift) can consume
// RZSL bursts. Tags come from the frame's own RZSL metadata.
void writeFrameDng(const std::filesystem::path& file, std::uint32_t cfa, std::uint32_t width, std::uint32_t height,
                   const std::string& rawMetadata, const rawrcam::metadata::FrameMetadataSnapshot& metadata,
                   const std::uint16_t* raw16) {
    static const std::array<std::array<std::uint8_t, 4>, 4> kCfaBytes{{
        {0, 1, 1, 2}, {1, 0, 2, 1}, {1, 2, 0, 1}, {2, 1, 1, 0}}};  // RGGB GRBG GBRG BGGR
    tinydng_cfa cfaInfo{};
    cfaInfo.present = 1;
    cfaInfo.pattern_dim[0] = cfaInfo.pattern_dim[1] = 2;
    for (int i = 0; i < 4; ++i) cfaInfo.pattern[i] = kCfaBytes[std::min(cfa, 3u)][std::size_t(i)];
    cfaInfo.pattern_size = 4;
    cfaInfo.plane_color[0] = 0;
    cfaInfo.plane_color[1] = 1;
    cfaInfo.plane_color[2] = 2;
    cfaInfo.plane_color_count = 3;
    cfaInfo.layout = 1;

    tinydng_raw_info raw{};
    raw.has_dng_version = 1;
    raw.dng_version[0] = 1;
    raw.dng_version[1] = 4;
    std::string model = "Rawr RZSL camera " + tsv(rawMetadata, "cameraId");
    raw.unique_camera_model = model.data();
    // BlackLevel is in CFA-pattern order; RZSL stores physical R,Gr,Gb,B.
    const auto& black = metadata.blackLevelPhysicalRggb;
    const auto& pattern = kCfaBytes[std::min(cfa, 3u)];
    int greenSeen = 0;
    for (int i = 0; i < 4; ++i) {
        const int colour = pattern[std::size_t(i)];
        const float value = colour == 0 ? black[0] : colour == 2 ? black[3] : black[std::size_t(1 + greenSeen++)];
        raw.black_level_exact[i] = value;
        raw.black_level[i] = static_cast<std::int32_t>(std::lround(value));
    }
    raw.has_black_level_exact = 1;
    raw.black_level_present = 1;
    raw.has_black_level_repeat = 1;
    raw.black_level_repeat[0] = raw.black_level_repeat[1] = 2;
    raw.white_level_present = 1;
    raw.white_level[0] = std::max(1, int(std::lround(metadata.effectiveWhiteLevel)));

    const auto& color = metadata.cameraContext->color;
    const auto copy9 = [](double* dst, const rawrcam::metadata::Matrix3x3& m) {
        for (int i = 0; i < 9; ++i) dst[i] = m.rowMajor[std::size_t(i)];
    };
    if (color.colorTransform1.valid) {
        raw.color_matrix_present = 1;
        copy9(raw.color_matrix1, color.colorTransform1);
        raw.calibration_illuminant1 = std::uint16_t(std::max(0, color.referenceIlluminant1));
        if (color.forwardMatrix1.valid) { copy9(raw.forward_matrix1, color.forwardMatrix1); raw.has_forward_matrix1 = 1; }
        if (color.calibrationTransform1.valid) {
            copy9(raw.camera_calibration1, color.calibrationTransform1);
            raw.camera_calibration_present = 1;
        }
        if (color.colorTransform2.valid && color.referenceIlluminant2 > 0) {
            raw.has_color_matrix2 = 1;
            copy9(raw.color_matrix2, color.colorTransform2);
            raw.calibration_illuminant2 = std::uint16_t(color.referenceIlluminant2);
            if (color.forwardMatrix2.valid) { copy9(raw.forward_matrix2, color.forwardMatrix2); raw.has_forward_matrix2 = 1; }
            if (color.calibrationTransform2.valid) {
                copy9(raw.camera_calibration2, color.calibrationTransform2);
                raw.has_camera_calibration2 = 1;
            }
        }
    }
    if (metadata.hasNeutralColorPoint) {
        for (int i = 0; i < 3; ++i) raw.as_shot_neutral[i] = metadata.neutralColorPoint[std::size_t(i)];
        raw.has_as_shot_neutral = 1;
    }

    tinydng_exif exif{};
    const auto exposureNs = tsv(rawMetadata, "exposureTimeNs");
    if (!exposureNs.empty()) {
        // 1/1e6 s units keep sub-ms ZSL exposures exact enough for EV ratios.
        exif.exposure_time[0] = static_cast<std::int32_t>(std::llround(std::stod(exposureNs) / 1000.0));
        exif.exposure_time[1] = 1000000;
        exif.has_exposure_time = 1;
    }
    const auto iso = tsv(rawMetadata, "sensitivity");
    if (!iso.empty()) {
        exif.iso = static_cast<std::uint32_t>(std::stoul(iso));
        exif.has_iso = 1;
    }
    std::string make = "Rawr";
    exif.make = make.data();
    exif.model = model.data();
    exif.orientation = 1;

    tinydng_write_image image{};
    image.width = width;
    image.height = height;
    image.samples_per_pixel = 1;
    image.bits_per_sample = 16;
    image.data = reinterpret_cast<const std::uint8_t*>(raw16);
    image.data_size = std::size_t(width) * height * 2u;
    image.cfa = &cfaInfo;
    image.raw = &raw;
    image.exif = &exif;
    tinydng_write_options options{};
    options.as_dng = 1;
    options.compression = 1;

    tinydng_error err{};
    tinydng_config config{};
    tinydng_context* ctx = tinydng_context_create(&config, &err);
    if (!ctx) throw std::runtime_error("tinydng context: " + std::string(err.message));
    const tinydng_status status = tinydng_write_file(ctx, file.string().c_str(), &image, &options, &err);
    tinydng_context_destroy(ctx);
    if (status != TINYDNG_OK)
        throw std::runtime_error("cannot write " + file.string() + ": " + std::string(err.message));
}

struct TuneOpts {
    std::string noiseSource = "burst";  // matches the app (always burst-fitted)
    float scale = 1.f;
    int lkIterations = 3;
    bool dumpLinear = false;
    float kDetail = -1.f;  // -1 = merge default
    float kDenoise = -1.f;
    float dThreshold = -1.f;
    float dTransition = -1.f;
    float kStretch = -1.f;
    float kShrink = -1.f;
    float flatSigma = -2.f;  // -2 = Config default (-1 = legacy)
    float detailFloorSigma = -1.f;  // -1 = Config default (0 = legacy)
    float scaleBandwidthGain = -1.f;
    float coverageNeffLo = -1.f;
    float coverageNeffHi = -1.f;
    float coverageMassLo = -1.f;
    float coverageMassHi = -1.f;
    float robustnessT = -1.f;
    float robustnessS1 = -1.f;
    float robustnessS2 = -1.f;
    int maxFrames = 0;  // 0 = all frames
    std::string calProfile;  // calibration.txt (R/G1/G2/B a/b lines) for noise-source=calibrated
    std::string suffix;
    bool dumpBase = false;
    bool dumpAllSingles = false;
    // Also write each decoded frame's unclipped RAW16 mosaic (dark-frame /
    // fixed-pattern analysis; the tagged dumps clip at black).
    bool dumpRaw16 = false;
    // Write every decoded frame as frame_NN.dng (+ ref.txt) into this dir.
    std::string exportDng;
    bool duplicateMiddle = false;  // diagnostic: every frame = the middle frame (perfect alignment)
    float affineDeadzone = -1.f;
    float affineSoftness = -1.f;
    bool affineIsotropic = false;
    float fallbackChroma = -1.f;
    float fallbackLuma = -1.f;
    float fallbackMaxSigma = -1.f;
    bool hotPixels = true;
    std::string hotPixelList;  // test/diagnostic override: "x y" per line
    std::string reference = "recorded";  // recorded|middle|sharpest
    std::string mergeAlgorithm = "wronski";  // wronski|hdrplus|hdrplus-freq
    rawr::raw_merge_hdrplus_gpu::Config hdrplus{};
};

std::uint64_t fnv1a(const void* data, std::size_t bytes) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::uint64_t hash = 1469598103934665603ull;
    for (std::size_t i = 0; i < static_cast<std::size_t>(bytes); ++i) hash = (hash ^ p[i]) * 1099511628211ull;
    return hash;
}

// Offline DNG-burst layout (.cache/dng_burst/rawburst convention, byte-exact
// with the checked-in Sea converter): GBRG source DNGs cropped to RGGB,
// frame_NN.raw16 (W*H uint16 mosaic) + frame_NN.meta (TSV metadata carrying
// the per-frame DNG Camera2 noise profile) + bundle.txt.
// Frames already hold raw pixels, so they decode via the memcpy path.
rawr::zsl_container::Bundle rawburstBundle(const std::string& path) {
    const std::string dir = path.substr(std::string("rawburst:").size());
    const auto kv = [&](const std::string& file) {
        std::map<std::string, std::string> out;
        std::ifstream in(std::filesystem::path(dir) / file);
        if (!in) throw std::runtime_error("cannot open " + dir + "/" + file);
        std::string line;
        while (std::getline(in, line)) {
            const auto eq = line.find('=');
            if (eq != std::string::npos) out[line.substr(0, eq)] = line.substr(eq + 1u);
        }
        return out;
    };
    const auto info = kv("bundle.txt");
    const std::uint32_t width = static_cast<std::uint32_t>(std::stoul(info.at("width")));
    const std::uint32_t height = static_cast<std::uint32_t>(std::stoul(info.at("height")));
    const std::uint32_t cfa = static_cast<std::uint32_t>(std::stoul(info.at("cfa")));
    const int frames = std::stoi(info.at("frames"));
    if (!(width & 1u) && !(height & 1u) && cfa <= 3u && frames >= 2) {}
    rawr::zsl_container::Bundle out{};
    out.cfa = cfa;
    char name[32];
    for (int i = 0; i < frames; ++i) {
        std::snprintf(name, sizeof(name), "frame_%02d", i);
        std::ifstream meta(std::filesystem::path(dir) / (std::string(name) + ".meta"), std::ios::binary);
        if (!meta) throw std::runtime_error(std::string("cannot open ") + name + ".meta");
        std::string metadata((std::istreambuf_iterator<char>(meta)), std::istreambuf_iterator<char>());
        std::ifstream raw(std::filesystem::path(dir) / (std::string(name) + ".raw16"),
                          std::ios::binary | std::ios::ate);
        if (!raw) throw std::runtime_error(std::string("cannot open ") + name + ".raw16");
        const std::streamoff bytes = raw.tellg();
        if (bytes != static_cast<std::streamoff>(std::size_t(width) * height * 2u))
            throw std::runtime_error(std::string(name) + ".raw16 size mismatch");
        raw.seekg(0);
        rawr::zsl_container::PackedFrame f{};
        f.width = width;
        f.height = height;
        f.gpuPacket.resize(static_cast<std::size_t>(bytes));
        if (!raw.read(reinterpret_cast<char*>(f.gpuPacket.data()), bytes))
            throw std::runtime_error(std::string("cannot read ") + name + ".raw16");
        const auto num = [&](const char* key, std::uint64_t fallback) {
            const std::string prefix = std::string(key) + "\t";
            const auto at = metadata.find(prefix);
            if (at == std::string::npos) return fallback;
            return static_cast<std::uint64_t>(std::stoull(metadata.substr(at + prefix.size())));
        };
        f.frameId = num("zslFrameId", static_cast<std::uint64_t>(i + 1));
        f.timestampNs = num("timestampNs", 1000000000ull + static_cast<std::uint64_t>(i) * 33333333ull);
        f.metadata = std::move(metadata);
        out.frames.push_back(std::move(f));
    }
    return out;
}

// XYZ white (Y=1) of a DNG CalibrationIlluminant (EXIF LightSource id).
// Named standards use their tabulated whites; other ids go through the
// nominal CCT on the Planckian (<4000 K) or daylight locus.
std::array<double, 3> illuminantWhiteXyz(int id) {
    switch (id) {
        case 17: return {1.09850, 1, 0.35585};  // Standard light A
        case 18: return {0.99072, 1, 0.85223};  // Standard light B
        case 19: return {0.98074, 1, 1.18232};  // Standard light C
        case 20: return {0.95682, 1, 0.92149};  // D55
        case 21: return {0.95047, 1, 1.08883};  // D65
        case 22: return {0.94972, 1, 1.22638};  // D75
        case 23: return {0.96422, 1, 0.82521};  // D50
        default: break;
    }
    const auto cct = rawrcam::color::math::referenceIlluminantCctKelvin(id);
    const double t = cct ? *cct : 5003.0;
    double x = 0, y = 0;
    if (t < 4000.0) {  // Kim et al. cubic Planckian locus approximation
        x = -0.2661239e9 / (t * t * t) - 0.2343589e6 / (t * t) + 0.8776956e3 / t + 0.179910;
        y = t < 2222.0 ? -1.1063814 * x * x * x - 1.34811020 * x * x + 2.18555832 * x - 0.20219683
                       : -0.9549476 * x * x * x - 1.37418593 * x * x + 2.09137015 * x - 0.16748867;
    } else {  // CIE daylight locus
        x = t <= 7000.0 ? -4.6070e9 / (t * t * t) + 2.9678e6 / (t * t) + 0.09911e3 / t + 0.244063
                        : -2.0064e9 / (t * t * t) + 1.9018e6 / (t * t) + 0.24748e3 / t + 0.237040;
        y = -3.0 * x * x + 2.87 * x - 0.275;
    }
    return {x / y, 1, (1 - x - y) / y};
}

using Mat3d = std::array<double, 9>;
Mat3d mul3(const Mat3d& a, const Mat3d& b) {
    Mat3d out{};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            for (int k = 0; k < 3; ++k) out[r * 3 + c] += a[r * 3 + k] * b[k * 3 + c];
    return out;
}
std::array<double, 3> mul3(const Mat3d& a, const std::array<double, 3>& v) {
    return {a[0] * v[0] + a[1] * v[1] + a[2] * v[2], a[3] * v[0] + a[4] * v[1] + a[5] * v[2],
            a[6] * v[0] + a[7] * v[1] + a[8] * v[2]};
}
Mat3d inv3(const Mat3d& m) {
    const double det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
                       m[2] * (m[3] * m[7] - m[4] * m[6]);
    if (!(std::abs(det) > 1e-12)) throw std::runtime_error("DNG: singular color matrix");
    return {(m[4] * m[8] - m[5] * m[7]) / det, (m[2] * m[7] - m[1] * m[8]) / det, (m[1] * m[5] - m[2] * m[4]) / det,
            (m[5] * m[6] - m[3] * m[8]) / det, (m[0] * m[8] - m[2] * m[6]) / det, (m[2] * m[3] - m[0] * m[5]) / det,
            (m[3] * m[7] - m[4] * m[6]) / det, (m[1] * m[6] - m[0] * m[7]) / det, (m[0] * m[4] - m[1] * m[3]) / det};
}

// DNG-spec ForwardMatrix equivalent of a ColorMatrix (XYZ -> reference camera)
// at its calibration illuminant: maps the WB-balanced reference camera RGB to
// D50 XYZ, with camera neutral (1,1,1) landing exactly on D50 via Bradford.
// Used when the DNG omits ForwardMatrixN, since the app colour solver
// (ColorCalibration.cpp) is forward-matrix based.
Mat3d forwardFromColorMatrix(const Mat3d& colorMatrix, int illuminant) {
    const Mat3d bradford{.8951, .2664, -.1614, -.7502, 1.7135, .0367, .0389, -.0685, 1.0296};
    const auto white = illuminantWhiteXyz(illuminant);
    const auto from = mul3(bradford, white);
    const auto to = mul3(bradford, std::array<double, 3>{0.96422, 1, 0.82521});
    Mat3d scale{};
    for (int i = 0; i < 3; ++i) scale[i * 4] = to[i] / from[i];
    const Mat3d adapt = mul3(inv3(bradford), mul3(scale, bradford));
    const auto neutral = mul3(colorMatrix, white);
    Mat3d diag{};
    for (int i = 0; i < 3; ++i) diag[i * 4] = neutral[i];
    return mul3(adapt, mul3(inv3(colorMatrix), diag));
}

// DNG burst: every *.dng in a directory (sorted by name) becomes one frame,
// decoded by the app's production DngSource. Pixels are the raw mosaic of the
// DNG crop (trimmed to even size); metadata is synthesized in the TSV layout
// frameMetadata() reads, so colour goes through the same app solver as RZSL.
// DNG opcodes (lens-shading gain maps) are not applied, as with RZSL input.
rawr::zsl_container::Bundle dngBundle(const std::string& path) {
    const std::string dir = path.substr(std::string("dng:").size());
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
        if (entry.is_regular_file() && ext == ".dng") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    if (files.size() < 2) throw std::runtime_error("DNG burst needs at least two .dng files in " + dir);
    rawr::zsl_container::Bundle out{};
    for (std::size_t i = 0; i < files.size(); ++i) {
        rawrcam::renderer::DngSource source(files[i].string());
        const auto& raw = source.info().raw;
        const std::uint32_t cx = source.crop[0], cy = source.crop[1];
        const std::uint32_t width = source.crop[2] & ~1u, height = source.crop[3] & ~1u;
        // DngSource CFA and black sites are raster-absolute; rebase to the crop.
        const std::uint32_t cfa = source.cfa ^ (cx & 1u) ^ ((cy & 1u) << 1u);
        if (i == 0) out.cfa = cfa;
        else if (cfa != out.cfa || width != out.frames.front().width || height != out.frames.front().height)
            throw std::runtime_error("DNG burst: " + files[i].filename().string() + " geometry/CFA differs");
        if (!(source.whiteLevel > 0)) throw std::runtime_error("DNG burst: missing WhiteLevel");
        // Bundle phases are RGGB-ordered: channel = cropPhase ^ cfa (inverse of
        // the merge sampler's remap).
        std::array<double, 4> blackRggb{};
        for (std::uint32_t phase = 0; phase < 4u; ++phase) {
            const std::uint32_t site = (((cy + (phase >> 1u)) & 1u) << 1u) | ((cx + (phase & 1u)) & 1u);
            blackRggb[phase ^ cfa] = source.black[site];
        }
        const auto matrix = [](const double* m) { return Mat3d{m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8]}; };
        const auto anyNonZero = [](const double* m) { return std::any_of(m, m + 9, [](double v) { return v != 0; }); };
        const Mat3d identity{1, 0, 0, 0, 1, 0, 0, 0, 1};
        const int illum1 = raw.calibration_illuminant1 ? raw.calibration_illuminant1 : 21;
        const bool dual = raw.has_color_matrix2 && anyNonZero(raw.color_matrix2) && raw.calibration_illuminant2;
        const int illum2 = dual ? raw.calibration_illuminant2 : illum1;
        const Mat3d cm1 = matrix(raw.color_matrix1);
        const Mat3d cm2 = dual ? matrix(raw.color_matrix2) : cm1;
        const Mat3d cc1 = raw.camera_calibration_present ? matrix(raw.camera_calibration1) : identity;
        const Mat3d cc2 = raw.camera_calibration_present && raw.has_camera_calibration2 &&
                                  anyNonZero(raw.camera_calibration2)
                              ? matrix(raw.camera_calibration2)
                              : cc1;
        const Mat3d fm1 = raw.has_forward_matrix1 && anyNonZero(raw.forward_matrix1) ? matrix(raw.forward_matrix1)
                                                                                      : forwardFromColorMatrix(cm1, illum1);
        const Mat3d fm2 = dual && raw.has_forward_matrix2 && anyNonZero(raw.forward_matrix2)
                              ? matrix(raw.forward_matrix2)
                              : (dual ? forwardFromColorMatrix(cm2, illum2) : fm1);
        std::ostringstream meta;
        meta << std::setprecision(9);
        const auto putMatrix = [&](const char* key, const Mat3d& m) {
            meta << key << "\t1";
            for (double v : m) meta << ',' << v;
            meta << '\n';
        };
        meta << "blackLevelPhysicalRggb\t" << blackRggb[0] << ',' << blackRggb[1] << ',' << blackRggb[2] << ','
             << blackRggb[3] << '\n'
             << "effectiveWhiteLevel\t" << source.whiteLevel << '\n'
             << "colorCorrectionGainsRggb\t" << source.wb[0] << ',' << source.wb[1] << ',' << source.wb[1] << ','
             << source.wb[2] << '\n'
             << "neutralColorPoint\t1," << raw.as_shot_neutral[0] << ',' << raw.as_shot_neutral[1] << ','
             << raw.as_shot_neutral[2] << '\n'
             << "referenceIlluminant1\t" << illum1 << '\n'
             << "referenceIlluminant2\t" << illum2 << '\n';
        putMatrix("colorTransform1", cm1);
        putMatrix("colorTransform2", cm2);
        putMatrix("calibrationTransform1", cc1);
        putMatrix("calibrationTransform2", cc2);
        putMatrix("forwardMatrix1", fm1);
        putMatrix("forwardMatrix2", fm2);
        meta << "zslFrameId\t" << (i + 1) << '\n'
             << "timestampNs\t" << (1000000000ull + i * 33333333ull) << '\n';
        const auto pixels = source.read(cx, cy, width, height);
        rawr::zsl_container::PackedFrame f{};
        f.width = width;
        f.height = height;
        f.gpuPacket.resize(pixels.size() * sizeof(std::uint16_t));
        std::memcpy(f.gpuPacket.data(), pixels.data(), f.gpuPacket.size());
        f.frameId = i + 1;
        f.timestampNs = 1000000000ull + i * 33333333ull;
        f.metadata = meta.str();
        std::cerr << "dng frame=" << i << ' ' << files[i].filename().string() << ' ' << width << 'x' << height
                  << " cfa=" << cfa << " white=" << source.whiteLevel << (raw.has_forward_matrix1 ? "" : " fm=synth")
                  << '\n';
        out.frames.push_back(std::move(f));
    }
    return out;
}

int replay(const std::string& path, const std::filesystem::path& outputDir, const TuneOpts& tune) {    const auto loadBegin = Clock::now();
    const bool isSynthetic = path.rfind("synthetic:", 0) == 0;
    const bool isRawburst = path.rfind("rawburst:", 0) == 0;
    const bool isDng = path.rfind("dng:", 0) == 0;
    const bool isRawPixels = isSynthetic || isRawburst || isDng;
    auto bundle = isSynthetic  ? synthetic::burst(path)
                  : isRawburst ? rawburstBundle(path)
                  : isDng      ? dngBundle(path)
                               : rawr::zsl_container::readBundle(path);
    const double loadMs = std::chrono::duration<double, std::milli>(Clock::now() - loadBegin).count();
    if (bundle.frames.size() < 2) throw std::runtime_error("bundle needs at least two frames");
    const auto width = bundle.frames.front().width;
    const auto height = bundle.frames.front().height;
    for (const auto& frame : bundle.frames)
        if (frame.width != width || frame.height != height) throw std::runtime_error("mixed frame geometry");

    auto context = vktest::ctx();
    std::cerr << "device=" << context.prop.deviceName
              << " shared_kb=" << (context.prop.limits.maxComputeSharedMemorySize / 1024u)
              << " max_invocations=" << context.prop.limits.maxComputeWorkGroupInvocations
              << " input=" << path << " frames=" << bundle.frames.size()
              << " geometry=" << width << 'x' << height << " cfa=" << bundle.cfa << '\n';
    Commands commands(context);
    rawr::zsl_codec::VulkanDecoder decoder;
    decoder.initialize(context.dev);

    std::size_t maxPacket = 0;
    for (const auto& frame : bundle.frames) maxPacket = std::max(maxPacket, frame.gpuPacket.size());
    auto packetBuffer = vktest::mkBuf(context.pd, context.dev, maxPacket, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    std::vector<vktest::Img> images;
    images.reserve(bundle.frames.size());
    std::vector<rawr::raw_gpu_pipeline::BurstFrame> burst;
    std::vector<rawrcam::metadata::FrameMetadataSnapshot> metadata;
    burst.reserve(bundle.frames.size());
    metadata.reserve(bundle.frames.size());

    const auto decodeBegin = Clock::now();
    for (auto& frame : bundle.frames) {
        images.push_back(vktest::mkImg(context.pd, context.dev, width, height, VK_FORMAT_R16_UINT,
                                      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT));
        void* mapped = nullptr;
        vktest::ck(vkMapMemory(context.dev, packetBuffer.m, 0, frame.gpuPacket.size(), 0, &mapped), "map packet");
        std::memcpy(mapped, frame.gpuPacket.data(), frame.gpuPacket.size());
        vkUnmapMemory(context.dev, packetBuffer.m);
        decoder.beginBatch();
        auto& image = images.back();
        commands.submit(context.q, [&](VkCommandBuffer command) {
            imageBarrier(command, image.i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
                         VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            rawr::zsl_codec::DecodePacket packet{packetBuffer.b,
                                                 frame.gpuPacket.size(),
                                                 frame.width,
                                                 frame.height,
                                                 frame.tilesX,
                                                 frame.tilesY,
                                                 frame.streams,
                                                 frame.tableBytes,
                                                 frame.payloadBytes};
            if (isRawPixels) {
                imageBarrier(command, image.i, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                VkBufferImageCopy copy{};
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {width, height, 1};
                vkCmdCopyBufferToImage(command, packetBuffer.b, image.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
                imageBarrier(command, image.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                             VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            } else decoder.record(command, packet, image.v);
        });
        rawr::zsl_ring::GpuRawImageView raw{{frame.frameId, frame.timestampNs, width, height},
                                            image.i,
                                            image.v,
                                            VK_FORMAT_R16_UINT,
                                            VkDeviceSize(width) * height * 2u};
        metadata.push_back(frameMetadata(frame.metadata));
        burst.push_back({raw, parameters(metadata.back())});
        std::vector<std::uint8_t>().swap(frame.gpuPacket);
    }
    const double decodeMs = std::chrono::duration<double, std::milli>(Clock::now() - decodeBegin).count();
    if (tune.duplicateMiddle) {
        const std::size_t mid = images.size() / 2;
        commands.submit(context.q, [&](VkCommandBuffer command) {
            for (std::size_t fi = 0; fi < images.size(); ++fi) {
                if (fi == mid) continue;
                VkImageCopy copy{};
                copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.extent = {width, height, 1};
                vkCmdCopyImage(command, images[mid].i, VK_IMAGE_LAYOUT_GENERAL, images[fi].i, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
            }
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT,
                                    VK_ACCESS_SHADER_READ_BIT};
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                                 &barrier, 0, nullptr, 0, nullptr);
        });
        std::cerr << "diagnostic=duplicate_middle frame=" << mid << '\n';
    }

    rawr::raw_alignment_gpu::Config alignment{};
    alignment.lkIterations = static_cast<std::uint32_t>(tune.lkIterations);
    rawr::raw_merge_wronski_gpu::Config merge{};
    merge.cfa = static_cast<rawr::raw_merge_wronski_gpu::CfaPattern>(bundle.cfa);
    merge.kDetail = .08f;
    merge.kDenoise = 5.f;
    merge.dThreshold = .25f;
    merge.dTransition = .3f;
    merge.kStretch = 1.f;
    merge.kShrink = 8.f;
    merge.flatSigma = .5f;
    merge.detailFloorSigma = 0.f;
    merge.scaleBandwidthGain = 1.f;
    const auto referenceText = tsv(bundle.frames.front().metadata, "rawrReferenceIndex");
    std::size_t originalRef = referenceText.empty() ? burst.size() / 2 : std::stoul(referenceText);
    // Device-computed per-frame scores, when the bundle carries them (new
    // Sharpest captures). Empty for old bundles and Middle-mode captures.
    const auto storedScores = [&]() {
        std::vector<float> out;
        for (auto& frame : bundle.frames) {
            const auto text = tsv(frame.metadata, "sharpnessScore");
            if (text.empty()) return std::vector<float>{};
            try {
                out.push_back(std::stof(text));
            } catch (...) {
                return std::vector<float>{};
            }
        }
        if (out.size() != burst.size()) return std::vector<float>{};
        return out;
    }();
    const auto middleIndex = static_cast<std::uint32_t>(burst.size() / 2);
    if (tune.reference == "middle") {
        originalRef = burst.size() / 2;
        std::cerr << "reference_source=middle_override ref=" << originalRef << '\n';
    } else if (tune.reference == "sharpest") {
        // Offline parity via the native reference scorer (same formula as
        // the on-device GPU path). Read back each decoded RAW16 once; fine
        // for an offline tool.
        const VkDeviceSize rawBytes = VkDeviceSize(width) * height * 2u;
        auto scoreReadback =
            vktest::mkBuf(context.pd, context.dev, rawBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        const auto pattern = bundle.cfa <= 3u ? static_cast<raw_sharpness::BayerPattern>(bundle.cfa)
                                              : raw_sharpness::BayerPattern::RGGB;
        std::vector<float> sharpScores;
        sharpScores.reserve(images.size());
        for (std::size_t fi = 0; fi < images.size(); ++fi) {
            commands.submit(context.q, [&](VkCommandBuffer command) {
                imageBarrier(command, images[fi].i, VK_IMAGE_LAYOUT_GENERAL,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
                             VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT);
                VkBufferImageCopy copy{};
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.imageExtent = {width, height, 1};
                vkCmdCopyImageToBuffer(command, images[fi].i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       scoreReadback.b, 1, &copy);
            });
            void* mapped = nullptr;
            vktest::ck(vkMapMemory(context.dev, scoreReadback.m, 0, rawBytes, 0, &mapped), "map score raw");
            const float score = raw_sharpness::reference::scoreCpu(static_cast<const std::uint8_t*>(mapped),
                                                                  width, height, pattern, 65535.f);
            vkUnmapMemory(context.dev, scoreReadback.m);
            std::cerr << "sharpness frame=" << fi << " score=" << score << '\n';
            sharpScores.push_back(score);
        }
        vktest::delBuf(context.dev, scoreReadback);
        const std::size_t best =
            raw_sharpness::reference::selectSharpest(sharpScores, middleIndex);
        if (sharpScores[best] > 0.f) originalRef = best;
        std::cerr << "reference_source=sharpest recomputed ref=" << originalRef << " score=" << sharpScores[best]
                  << '\n';
        if (!storedScores.empty()) {
            const std::size_t deviceBest =
                raw_sharpness::reference::selectSharpest(storedScores, middleIndex);
            std::cerr << "reference_device ref=" << deviceBest << " score=" << storedScores[deviceBest]
                      << " match=" << (deviceBest == originalRef ? 1 : 0) << '\n';
        }
    } else if (!storedScores.empty()) {
        // Exact on-device decision, ties included: the bundle carries the
        // device-computed per-frame scores.
        const std::size_t best = raw_sharpness::reference::selectSharpest(storedScores, middleIndex);
        if (best < burst.size()) {
            if (best != originalRef) {
                std::cerr << "reference_note=stored-scores override recorded-index " << originalRef << "->" << best
                          << '\n';
            }
            originalRef = best;
        }
        std::cerr << "reference_source=recorded_scores ref=" << originalRef << '\n';
    } else {
        std::cerr << "reference_source=recorded_index ref=" << originalRef << '\n';
    }
    if (originalRef >= burst.size()) throw std::invalid_argument("RZSL: reference index out of range");
    if (tune.noiseSource == "recorded" || tune.noiseSource == "burst") {
        const bool recorded = rawr::raw_gpu_pipeline::applyReplayNoise(bundle.frames[originalRef].metadata, merge);
        std::cerr << "noise_source=" << (recorded ? "recorded_resolved" : "legacy_missing_recorded_model") << '\n';
        if (merge.sensorNoiseProfile) {
            const auto& n = *merge.sensorNoiseProfile;
            std::cerr << "noise_profile slope=" << n.slopeBySite[0] << ',' << n.slopeBySite[1] << ',' << n.slopeBySite[2]
                      << ',' << n.slopeBySite[3] << " offset=" << n.offsetBySite[0] << ',' << n.offsetBySite[1] << ','
                      << n.offsetBySite[2] << ',' << n.offsetBySite[3] << '\n';
        }
        if (isSynthetic && !recorded) {
            std::cerr << "noise_warn=synthetic_has_no_recorded_model_using_legacy_explicitly\n";
        }
    } else if (tune.noiseSource == "camera2") {
        // Parity with the app fix: Camera2 S,O are sensor-DN domain, the merge
        // consumes normalized variance. a=S/range, b=(S*black+O)/range^2.
        std::istringstream in(tsv(bundle.frames[originalRef].metadata, "sensorNoiseProfile"));
        double sRaw[4], oRaw[4];
        for (int i = 0; i < 4; ++i)
            if (!(in >> sRaw[i] >> oRaw[i]))
                throw std::invalid_argument("RZSL: missing Camera2 noise profile");
        const auto& refMeta = metadata[originalRef];
        const float white = refMeta.effectiveWhiteLevel;
        rawr::raw_merge_wronski_gpu::CfaNoiseProfile profile{};
        for (int i = 0; i < 4; ++i) {
            const float black = refMeta.blackLevelPhysicalRggb[i];
            const float range = white - black;
            if (!(std::isfinite(sRaw[i]) && std::isfinite(oRaw[i]) && std::isfinite(range) && range > 0.f &&
                  sRaw[i] >= 0.0 && oRaw[i] >= 0.0))
                throw std::invalid_argument("RZSL: invalid Camera2 noise levels");
            profile.slopeBySite[i] = static_cast<float>(sRaw[i] / range);
            profile.offsetBySite[i] =
                static_cast<float>((sRaw[i] * black + oRaw[i]) / (double(range) * range));
        }
        if (!rawr::raw_merge_wronski_gpu::valid(profile)) throw std::invalid_argument("RZSL: invalid Camera2 noise profile");
        merge.sensorNoiseProfile = profile;
        std::cerr << "noise_source=camera2_override white=" << white
                  << " slope=" << profile.slopeBySite[0] << " offset=" << profile.offsetBySite[0] << '\n';
    } else if (tune.noiseSource == "calibrated") {
        // Investigation-only: inject a fitted calibration.txt profile
        // (lines "R a=.. b=.." for R/G1/G2/B) to test an effective floor
        // without touching production code.
        if (tune.calProfile.empty()) throw std::invalid_argument("calibrated needs --cal-profile");
        std::ifstream cal(tune.calProfile);
        if (!cal) throw std::invalid_argument("cannot open --cal-profile");
        rawr::raw_merge_wronski_gpu::CfaNoiseProfile profile{};
        const char* want[4] = {"R", "G1", "G2", "B"};
        std::string line;
        int got = 0;
        while (std::getline(cal, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream ls(line);
            std::string ch, aeq, beq;
            if (!(ls >> ch >> aeq >> beq)) throw std::invalid_argument("bad --cal-profile line");
            if (got >= 4 || ch != want[got]) throw std::invalid_argument("bad --cal-profile channel order");
            profile.slopeBySite[got] = std::stof(aeq.substr(aeq.find('=') + 1));
            profile.offsetBySite[got] = std::stof(beq.substr(beq.find('=') + 1));
            ++got;
        }
        if (got != 4) throw std::invalid_argument("incomplete --cal-profile");
        if (!rawr::raw_merge_wronski_gpu::valid(profile)) throw std::invalid_argument("invalid --cal-profile");
        merge.sensorNoiseProfile = profile;
        std::cerr << "noise_source=calibrated_override slope=" << profile.slopeBySite[0]
                  << " offset=" << profile.offsetBySite[0] << '\n';
    } else std::cerr << "noise_source=legacy_override\n";
    if (tune.kDetail >= 0.f) merge.kDetail = tune.kDetail;
    if (tune.kDenoise >= 0.f) merge.kDenoise = tune.kDenoise;
    if (tune.dThreshold >= 0.f) merge.dThreshold = tune.dThreshold;
    if (tune.dTransition >= 0.f) merge.dTransition = tune.dTransition;
    if (tune.kStretch >= 0.f) merge.kStretch = tune.kStretch;
    if (tune.kShrink >= 0.f) merge.kShrink = tune.kShrink;
    if (tune.flatSigma > -1.5f) merge.flatSigma = tune.flatSigma;
    if (tune.detailFloorSigma >= 0.f) merge.detailFloorSigma = tune.detailFloorSigma;
    if (tune.scaleBandwidthGain >= 0.f) merge.scaleBandwidthGain = tune.scaleBandwidthGain;
    if (tune.coverageNeffLo >= 0.f) merge.coverageNeffLo = tune.coverageNeffLo;
    if (tune.coverageNeffHi >= 0.f) merge.coverageNeffHi = tune.coverageNeffHi;
    if (tune.coverageMassLo >= 0.f) merge.coverageMassLo = tune.coverageMassLo;
    if (tune.coverageMassHi >= 0.f) merge.coverageMassHi = tune.coverageMassHi;
    if (tune.robustnessT >= 0.f) merge.robustnessT = tune.robustnessT;
    if (tune.robustnessS1 >= 0.f) merge.robustnessS1 = tune.robustnessS1;
    if (tune.robustnessS2 >= 0.f) merge.robustnessS2 = tune.robustnessS2;
    if (tune.affineDeadzone >= 0.f) merge.affineDeadzoneSigma = tune.affineDeadzone;
    if (tune.affineSoftness >= 0.f) merge.affineSoftnessSigma = tune.affineSoftness;
    if (tune.affineIsotropic) merge.affineApertureAware = false;
    if (tune.fallbackChroma >= 0.f) merge.fallbackChromaGain = tune.fallbackChroma;
    if (tune.fallbackLuma >= 0.f) merge.fallbackLumaGain = tune.fallbackLuma;
    if (tune.fallbackMaxSigma >= 0.f) merge.fallbackChromaMaxSigma = tune.fallbackMaxSigma;
    if (tune.hotPixels) {
        // Sensor hot-pixel map of the reference frame (recorded by newer dumps).
        std::istringstream hot(tsv(bundle.frames[originalRef].metadata, "hotPixelMap"));
        for (std::int32_t v; hot >> v;) merge.hotPixels.push_back(v);
        if (!tune.hotPixelList.empty()) {
            std::ifstream list(tune.hotPixelList);
            if (!list) throw std::invalid_argument("cannot open --hot-pixel-list");
            merge.hotPixels.clear();
            for (std::int32_t v; list >> v;) merge.hotPixels.push_back(v);
        }
        std::cerr << "hot_pixels=" << merge.hotPixels.size() / 2 << '\n';
    }
    // burst: fit the profile from the frames themselves (recorded stays as fallback).
    if (tune.noiseSource == "burst") merge.estimateNoiseFromBurst = true;
    // Frame slicing centered on reference (app uses middle or sharpest).
    // Keeps reference identical, only changes how many companions merge.
    std::vector<rawr::raw_gpu_pipeline::BurstFrame> runFrames = burst;
    std::uint32_t runRef = static_cast<std::uint32_t>(originalRef);
    std::uint32_t metaRef = runRef;
    int useFrames = static_cast<int>(burst.size());
    if (tune.maxFrames >= 4 && tune.maxFrames < static_cast<int>(burst.size())) {
        useFrames = tune.maxFrames;
        const int total = static_cast<int>(burst.size());
        const int refOrig = static_cast<int>(originalRef);
        int start = refOrig - useFrames / 2;
        if (start < 0) start = 0;
        if (start + useFrames > total) start = total - useFrames;
        runFrames.assign(burst.begin() + start, burst.begin() + start + useFrames);
        runRef = static_cast<std::uint32_t>(refOrig - start);
        metaRef = static_cast<std::uint32_t>(refOrig);
    }
    rawr::raw_gpu_pipeline::AndroidBurstCoordinator coordinator;
    if (std::getenv("RAWR_WARMUP") != nullptr) {
        // Exercise the app's pre-warm path: pipelines now, arena in initialize().
        const auto warmBegin = Clock::now();
        coordinator.warmPipelines(context.pd, context.dev, context.qf);
        const double warmMs = std::chrono::duration<double, std::milli>(Clock::now() - warmBegin).count();
        std::cerr << "warmup_ms=" << warmMs << '\n';
    }
    coordinator.initialize(context.pd, context.dev, context.qf,
                           [&](const VkSubmitInfo& info, VkFence fence) {
                               vktest::ck(vkQueueSubmit(context.q, 1, &info, fence), "burst queue submit");
                           },
                           width, height, tune.scale, {}, alignment, merge,
                           tune.mergeAlgorithm == "hdrplus"        ? rawr::raw_gpu_pipeline::MergeAlgorithm::HdrPlusSpatial
                           : tune.mergeAlgorithm == "hdrplus-freq" ? rawr::raw_gpu_pipeline::MergeAlgorithm::HdrPlusFrequency
                                                                   : rawr::raw_gpu_pipeline::MergeAlgorithm::Wronski,
                           tune.hdrplus);
    const auto result = coordinator.run(runFrames, runRef);
    if (merge.estimateNoiseFromBurst) {
        const auto& n = result.estimatedNoise;
        std::cerr << "noise_burst estimated=" << (result.noiseEstimated ? 1 : 0)
                  << " ms=" << result.timings.noiseEstimateMs << " slope=" << n.slopeBySite[0] << ','
                  << n.slopeBySite[1] << ',' << n.slopeBySite[2] << ',' << n.slopeBySite[3]
                  << " offset=" << n.offsetBySite[0] << ',' << n.offsetBySite[1] << ',' << n.offsetBySite[2]
                  << ',' << n.offsetBySite[3] << '\n';
    }

    const VkDeviceSize outputBytes = VkDeviceSize(result.outputExtent.width) * result.outputExtent.height * 8u;
    auto readback = vktest::mkBuf(context.pd, context.dev, outputBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    commands.submit(context.q, [&](VkCommandBuffer command) {
        imageBarrier(command, result.output, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {result.outputExtent.width, result.outputExtent.height, 1};
        vkCmdCopyImageToBuffer(command, result.output, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.b, 1, &copy);
    });
    void* output = nullptr;
    vktest::ck(vkMapMemory(context.dev, readback.m, 0, outputBytes, 0, &output), "map output");
    const auto hash = fnv1a(output, static_cast<std::size_t>(outputBytes));
    std::size_t nonzero = 0;
    for (std::size_t i = 0; i < static_cast<std::size_t>(outputBytes); ++i)
        nonzero += static_cast<const std::uint8_t*>(output)[i] != 0;
    writeOfflineInput(outputDir, path, bundle.cfa, result.outputExtent.width, result.outputExtent.height,
                      metadata[metaRef], output, tune.suffix);
    if (tune.dumpLinear) {
        const auto linearPath = outputDir / (std::filesystem::path(path).stem().string() +
            (tune.suffix.empty() ? "" : "." + tune.suffix) + ".linear.rgba16f");
        std::ofstream linear(linearPath, std::ios::binary | std::ios::trunc);
        linear.write(static_cast<const char*>(output), static_cast<std::streamsize>(outputBytes));
        if (!linear) throw std::runtime_error("cannot write " + linearPath.string());
    }
    if (isSynthetic) {
        const auto truthPath = outputDir / (std::filesystem::path(path).stem().string() +
            "." + tune.suffix + ".truth.rgb32f");
        std::ofstream truth(truthPath, std::ios::binary);
        for (unsigned y = 0; y < result.outputExtent.height; ++y) for (unsigned x = 0; x < result.outputExtent.width; ++x) {
            const auto rgb = synthetic::sample((float(x) + .5f) / tune.scale - .5f,
                (float(y) + .5f) / tune.scale - .5f, path == "synthetic:flat");
            truth.write(reinterpret_cast<const char*>(rgb.data()), sizeof(rgb));
        }
    }
    vkUnmapMemory(context.dev, readback.m);

    if (tune.dumpBase) {
        // Read back the decoded reference RAW16 for a fair base-vs-merged pair
        // through the identical offline demosaic/tonemap.
        const std::uint32_t baseIdx = metaRef;
        const VkDeviceSize rawBytes = VkDeviceSize(width) * height * 2u;
        auto rawReadback = vktest::mkBuf(context.pd, context.dev, rawBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        commands.submit(context.q, [&](VkCommandBuffer command) {
            imageBarrier(command, images[baseIdx].i, VK_IMAGE_LAYOUT_GENERAL,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
                         VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {width, height, 1};
            vkCmdCopyImageToBuffer(command, images[baseIdx].i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   rawReadback.b, 1, &copy);
        });
        void* rawMapped = nullptr;
        vktest::ck(vkMapMemory(context.dev, rawReadback.m, 0, rawBytes, 0, &rawMapped), "map base raw");
        writeBaseOfflineInput(outputDir, path, bundle.cfa, width, height, metadata[baseIdx],
                              static_cast<const std::uint16_t*>(rawMapped));
        vkUnmapMemory(context.dev, rawReadback.m);
        vktest::delBuf(context.dev, rawReadback);
    }

    if (tune.dumpAllSingles) {
        // Dump every decoded frame as a single-frame tagged input (same
        // layout as merged/base) so offline Python can rank per-frame
        // sharpness and the merge can be compared against the sharpest
        // single input. Uses each frame's own metadata for WB/color.
        const VkDeviceSize rawBytes = VkDeviceSize(width) * height * 2u;
        auto rawReadback = vktest::mkBuf(context.pd, context.dev, rawBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        for (std::size_t fi = 0; fi < images.size(); ++fi) {
            commands.submit(context.q, [&](VkCommandBuffer command) {
                imageBarrier(command, images[fi].i, VK_IMAGE_LAYOUT_GENERAL,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
                             VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT);
                VkBufferImageCopy copy{};
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.imageExtent = {width, height, 1};
                vkCmdCopyImageToBuffer(command, images[fi].i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       rawReadback.b, 1, &copy);
            });
            // After the first copy the image is in TRANSFER_SRC_OPTIMAL; move
            // it back to GENERAL so the next iteration's barrier (which
            // expects GENERAL) and the later merge see a defined layout.
            commands.submit(context.q, [&](VkCommandBuffer command) {
                imageBarrier(command, images[fi].i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT,
                             VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            });
            void* rawMapped = nullptr;
            vktest::ck(vkMapMemory(context.dev, rawReadback.m, 0, rawBytes, 0, &rawMapped), "map single raw");
            char stemBuf[32];
            std::snprintf(stemBuf, sizeof(stemBuf), "single%02zu", fi);
            std::string tag = ":single" + std::to_string(fi);
            writeBaseOfflineInput(outputDir, path, bundle.cfa, width, height, metadata[fi],
                                  static_cast<const std::uint16_t*>(rawMapped), stemBuf, tag);
            if (!tune.exportDng.empty()) {
                std::filesystem::path dir = tune.exportDng;
                std::filesystem::create_directories(dir);
                char name[32];
                std::snprintf(name, sizeof(name), "frame_%02zu.dng", fi);
                writeFrameDng(dir / name, bundle.cfa, width, height, bundle.frames[fi].metadata, metadata[fi],
                              static_cast<const std::uint16_t*>(rawMapped));
                if (fi == 0) std::ofstream(dir / "ref.txt") << metaRef << '\n';
            }
            if (tune.dumpRaw16) {
                const auto rawPath = outputDir / (std::filesystem::path(path).stem().string() + "." + stemBuf + ".raw16");
                std::ofstream rawOut(rawPath, std::ios::binary | std::ios::trunc);
                rawOut.write(static_cast<const char*>(rawMapped), static_cast<std::streamsize>(rawBytes));
                if (!rawOut) throw std::runtime_error("cannot write " + rawPath.string());
            }
            vkUnmapMemory(context.dev, rawReadback.m);
        }
        vktest::delBuf(context.dev, rawReadback);
    }

    const auto& t = result.timings;
    std::cout << std::fixed << std::setprecision(3)
              << "RESULT input=" << path << " suffix=" << tune.suffix << " merge=" << tune.mergeAlgorithm
              << " frames=" << result.frameCount << " requested=" << useFrames
              << " scale=" << tune.scale << " lkIterations=" << tune.lkIterations
              << " kDetail=" << merge.kDetail << " kDenoise=" << merge.kDenoise
              << " dThreshold=" << merge.dThreshold << " dTransition=" << merge.dTransition
              << " kStretch=" << merge.kStretch << " kShrink=" << merge.kShrink
              << " flatSigma=" << merge.flatSigma << " floor=" << merge.detailFloorSigma
              << " scaleGain=" << merge.scaleBandwidthGain
              << " neff=" << merge.coverageNeffLo << ".." << merge.coverageNeffHi
              << " mass=" << merge.coverageMassLo << ".." << merge.coverageMassHi
              << " robustnessT=" << merge.robustnessT
              << " load_ms=" << loadMs
              << " decode_ms=" << decodeMs << " init_ms=" << t.initializeMs << " ref_prepare_ms="
              << t.referencePrepareMs << " ref_stats_ms=" << t.referenceStatsMs << " companion_prepare_ms="
              << t.companionPrepareMs << " alignment_ms=" << t.alignmentMs << " kernel_stats_ms="
              << t.kernelStatsMs << " robustness_ms=" << t.robustnessMs << " accumulation_ms="
              << t.accumulationMs << " finalize_ms=" << t.finalizeMs << " merge_total_ms=" << t.mergeTotalMs()
              << " total_ms=" << t.totalMs << " submissions=" << result.submissionCount << " image_mib="
              << (double(result.physicalImageBytes) / 1048576.0) << " buffer_mib="
              << (double(result.physicalBufferBytes) / 1048576.0) << " nonzero_bytes=" << nonzero
              << " hash=0x" << std::hex << hash << std::dec << '\n';

    vktest::delBuf(context.dev, readback);
    coordinator.reset();
    decoder.reset();
    for (auto& image : images) vktest::delImg(context.dev, image);
    vktest::delBuf(context.dev, packetBuffer);
    commands.reset();
    vktest::delCtx(context);
    return nonzero == 0 ? 3 : 0;
}
}  // namespace

int main(int argc, char** argv) {
    const std::string usage =
        std::string("usage: ") + argv[0] +
        " --output-dir DIR [--k-detail F] [--k-denoise F]"
        " [--scale F] [--lk-iterations I] [--dump-linear] [--noise-source recorded|camera2|legacy|calibrated|burst]"
        " [--cal-profile FILE]"
        " [--d-threshold F] [--d-transition F] [--k-stretch F] [--k-shrink F]"
        " [--flat-sigma F] [--detail-floor F] [--scale-bandwidth-gain F]"
        " [--coverage-neff-lo F] [--coverage-neff-hi F] [--coverage-mass-lo F] [--coverage-mass-hi F]"
        " [--robustness-t F] [--robustness-s1 F] [--robustness-s2 F]"
         " [--max-frames I] [--suffix STR] [--dump-base] [--dump-all-singles] [--dump-raw16] [--export-dng DIR] [--merge wronski|hdrplus|hdrplus-freq] [--hdrplus-strength F] [--hdrplus-tile 16|32] [--hdrplus-search 32|64|128] [--hdrplus-faithful] [--affine-deadzone F] [--affine-softness F] [--affine-isotropic] [--fallback-chroma GAIN] [--fallback-luma GAIN] [--fallback-max-sigma PX] [--no-hot-pixels] [--hot-pixel-list FILE]"
         " [--reference recorded|middle|sharpest] INPUT [INPUT2 ...]\n"
         "  INPUT: FILE.rzsl | DNG_DIR or dng:DNG_DIR (every *.dng, name-sorted, is one burst)"
         " | rawburst:DIR | synthetic:...";
    try {
        std::filesystem::path outputDir;
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--output-dir" && i + 1 < argc) outputDir = argv[++i];
        }
        if (outputDir.empty()) {
            std::cerr << usage << '\n';
            return 2;
        }
        TuneOpts tune;
        std::vector<std::string> inputs;
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            auto need = [&](const char* what) {
                if (i + 1 >= argc) throw std::invalid_argument(std::string("missing value for ") + what);
                return std::string(argv[++i]);
            };
            if (a == "--output-dir") {
                (void)need(a.c_str());  // already consumed in the pre-scan
            } else if (a == "--noise-source") tune.noiseSource = need(a.c_str());
            else if (a == "--cal-profile") tune.calProfile = need(a.c_str());
            else if (a == "--scale") tune.scale = std::stof(need(a.c_str()));
            else if (a == "--lk-iterations") tune.lkIterations = std::stoi(need(a.c_str()));
            else if (a == "--dump-linear") tune.dumpLinear = true;
            else if (a == "--k-detail") tune.kDetail = std::stof(need(a.c_str()));
            else if (a == "--k-denoise") tune.kDenoise = std::stof(need(a.c_str()));
            else if (a == "--d-threshold") tune.dThreshold = std::stof(need(a.c_str()));
            else if (a == "--d-transition") tune.dTransition = std::stof(need(a.c_str()));
            else if (a == "--k-stretch") tune.kStretch = std::stof(need(a.c_str()));
            else if (a == "--k-shrink") tune.kShrink = std::stof(need(a.c_str()));
            else if (a == "--flat-sigma") tune.flatSigma = std::stof(need(a.c_str()));
            else if (a == "--detail-floor") tune.detailFloorSigma = std::stof(need(a.c_str()));
            else if (a == "--scale-bandwidth-gain") tune.scaleBandwidthGain = std::stof(need(a.c_str()));
            else if (a == "--coverage-neff-lo") tune.coverageNeffLo = std::stof(need(a.c_str()));
            else if (a == "--coverage-neff-hi") tune.coverageNeffHi = std::stof(need(a.c_str()));
            else if (a == "--coverage-mass-lo") tune.coverageMassLo = std::stof(need(a.c_str()));
            else if (a == "--coverage-mass-hi") tune.coverageMassHi = std::stof(need(a.c_str()));
            else if (a == "--robustness-t") tune.robustnessT = std::stof(need(a.c_str()));
            else if (a == "--robustness-s1") tune.robustnessS1 = std::stof(need(a.c_str()));
            else if (a == "--robustness-s2") tune.robustnessS2 = std::stof(need(a.c_str()));
            else if (a == "--max-frames") tune.maxFrames = std::stoi(need(a.c_str()));
            else if (a == "--suffix") tune.suffix = need(a.c_str());
            else if (a == "--dump-base") tune.dumpBase = true;
            else if (a == "--dump-all-singles") tune.dumpAllSingles = true;
            else if (a == "--affine-deadzone") tune.affineDeadzone = std::stof(need(a.c_str()));
            else if (a == "--affine-isotropic") tune.affineIsotropic = true;
            else if (a == "--no-hot-pixels") tune.hotPixels = false;
            else if (a == "--hot-pixel-list") tune.hotPixelList = need(a.c_str());
            else if (a == "--fallback-max-sigma") tune.fallbackMaxSigma = std::stof(need(a.c_str()));
            else if (a == "--fallback-luma") tune.fallbackLuma = std::stof(need(a.c_str()));
            else if (a == "--fallback-chroma") tune.fallbackChroma = std::stof(need(a.c_str()));
            else if (a == "--affine-softness") tune.affineSoftness = std::stof(need(a.c_str()));
            else if (a == "--duplicate-middle") tune.duplicateMiddle = true;
            else if (a == "--dump-raw16") tune.dumpAllSingles = tune.dumpRaw16 = true;
            else if (a == "--export-dng") { tune.exportDng = need(a.c_str()); tune.dumpAllSingles = true; }
            else if (a == "--reference") tune.reference = need(a.c_str());
            else if (a == "--merge") tune.mergeAlgorithm = need(a.c_str());
            else if (a == "--hdrplus-strength") tune.hdrplus.strength = std::stof(need(a.c_str()));
            else if (a == "--hdrplus-tile") tune.hdrplus.tileSize = std::uint32_t(std::stoul(need(a.c_str())));
            else if (a == "--hdrplus-search") tune.hdrplus.searchDistance = std::uint32_t(std::stoul(need(a.c_str())));
            else if (a == "--hdrplus-faithful") tune.hdrplus.frequencyAlignOnce = false;
            else if (a.rfind("--", 0) == 0) throw std::invalid_argument("unknown flag " + a);
            else if (a.rfind("dng:", 0) != 0 && a.find(':') == std::string::npos && std::filesystem::is_directory(a))
                inputs.push_back("dng:" + std::filesystem::path(a).lexically_normal().string());
            else inputs.push_back(a);
        }
        // Strip trailing separators so the output stem is the directory name.
        for (auto& in : inputs)
            while (in.rfind("dng:", 0) == 0 && in.size() > 5 && (in.back() == '/' || in.back() == '\\')) in.pop_back();
        if (tune.noiseSource != "recorded" && tune.noiseSource != "camera2" && tune.noiseSource != "legacy" &&
            tune.noiseSource != "calibrated" && tune.noiseSource != "burst")
            throw std::invalid_argument("noise-source must be recorded, camera2, legacy, calibrated, or burst");
        if (inputs.empty()) throw std::invalid_argument("no INPUT given");
        if (tune.reference != "recorded" && tune.reference != "middle" && tune.reference != "sharpest")
            throw std::invalid_argument("reference must be recorded, middle, or sharpest");
        if (!std::isfinite(tune.scale) || tune.scale < 1.f || tune.scale > 2.f)
            throw std::invalid_argument("scale must be finite and 1..2");
        if (tune.lkIterations < 1 || tune.lkIterations > 30)
            throw std::invalid_argument("lk-iterations must be 1..30");
        if (tune.maxFrames != 0 && (tune.maxFrames < 4 || tune.maxFrames > 30))
            throw std::invalid_argument("max-frames must be 4..30");
        if (tune.flatSigma != -2.f && !(std::isfinite(tune.flatSigma) && tune.flatSigma >= -1.f))
            throw std::invalid_argument("flat-sigma must be >= -1 (-1 = legacy kDetail*kDenoise)");
        if (tune.detailFloorSigma != -1.f && !(std::isfinite(tune.detailFloorSigma) && tune.detailFloorSigma >= 0.f))
            throw std::invalid_argument("detail-floor must be >= 0 (0 = legacy, try 0.08..0.15)");
        if (tune.scaleBandwidthGain != -1.f &&
            !(std::isfinite(tune.scaleBandwidthGain) && tune.scaleBandwidthGain >= 0.f))
            throw std::invalid_argument("scale-bandwidth-gain must be >= 0 (0 = scale-agnostic, try 0.5..1)");
        for (const float v : {tune.coverageNeffLo, tune.coverageNeffHi})
            if (v != -1.f && !(std::isfinite(v) && v >= 0.f)) throw std::invalid_argument("coverage-neff must be >= 0");
        for (const float v : {tune.coverageMassLo, tune.coverageMassHi})
            if (v != -1.f && !(std::isfinite(v) && v >= 0.f)) throw std::invalid_argument("coverage-mass must be >= 0");
        for (const auto& in : inputs) replay(in, outputDir, tune);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "ERROR " << e.what() << '\n';
        return 1;
    }
}
