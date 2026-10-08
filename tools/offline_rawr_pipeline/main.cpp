#include "vk_test_common.hpp"
#include "post_demosaic/PostDemosaicProcessor.h"

#include <dual/Dual.hpp>
#include <rcd/Rcd.hpp>
#include <quadfix/Pipeline.hpp>
#include <gainmap/GainmapCompute.h>
#include <spektrafilm/SpektraFilm.h>
#include <tonemap/TonemapEngine.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using vktest::Buf;
using vktest::Ctx;
using vktest::Img;

struct CaptureConfig {
    std::string source = "unknown";
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t cfa = 0;
    uint32_t fccSteps = 2;
    float fccEdgeSigma = 0.0f;
    float fccChromaBound = 0.0f;
    float defringeStrength = 0.0f;
    float defringeEdgeThreshold = 0.02f;
    float defringeLumaFloor = 0.08f;
    bool dualAutoContrast = true;
    float dualContrastPercent = 20.0f;
    float exposureEV = 0.0f;
    std::array<float, 3> wbRgb{1.0f, 1.0f, 1.0f};
    std::array<float, 9> cameraToWorking{
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 1.0f};
};

struct Arguments {
    std::filesystem::path input;
    bool linearRgb = false;
    bool demosaicedRgb = false;
    bool dumpDemosaic = false;
    bool rcdOnly = false;
    uint32_t benchmarkPost = 0;
    std::filesystem::path config;
    std::filesystem::path outputDir;
    // Technical DWG/Intermediate LUT, producing display-ready encoded sRGB.
    std::filesystem::path technicalLutDwg;
    bool hasExposureEV = false;
    bool singleVariant = false;
    bool singleRecovery = true;
    bool preserveReconstructedHighlights = false;
    bool quadfix = false;
    bool quadfixFastMedian = false;
    float exposureEV = 0.0f;
    float postGain = 1.0f;
    float highlightBias = 0.0f;
    float rtHighlightCompression = 0.0f;
    bool appColoropp = false;
    float appColoroppThreshold = 1.0f;
    float appColoroppCompression = 100.0f;
    // Film A/B replay: record the demosaiced frame through SpektraFilm
    // instead of tonemap. Cases are "filmEV:printEV" pairs; each renders
    // once with the given EVs on the shot look.
    bool film = false;
    std::filesystem::path spirvDir;
    std::filesystem::path hanatosPath;
    std::filesystem::path gamutPath;
    std::string filmCases = "0:0,1:0,1.47:0,2:0,0:-1,0:-1.47,0:-2";
    bool filmGrain = false;
    bool filmRecovery = true;
    float filmDirAmount = 0.8f;
    bool filmPrintDiffusion = true;
    // Linear-scatter effects for the film+UltraHDR replay (glow-quotient
    // tap calibration). Both ride the record look with stock defaults;
    // enabling either also exercises SpektraFilm::willWriteGlowGain.
    bool filmHalation = false;
    bool filmCameraDiffusion = false;
    // Row-major sensor->linear-sRGB for the film input (app post-WB
    // semantics). Default: vivo V2562 endpoint-2 (StdA) forward-matrix
    // composition replicated from ColorCalibration.cpp.
    std::array<float, 9> filmMatrix{1.3716f, -0.3383f, -0.0293f, -0.1511f, 1.1451f, 0.0164f, 0.1032f, -0.9586f,
                                    1.8471f};
    // UltraHDR replay: dump the pre-tonemap HDR linear input and run the
    // production gainmap.comp stage on (HDR, SDR) in the same submit.
    bool dumpHdr = false;
    bool gainmap = false;
    // Row-major HDR-cam-RGB -> linear-sRGB for the gainmap stage. Default is
    // the GainmapParams AP1->sRGB assumption; pass the calibrated
    // camera->linear-sRGB matrix (same one the tonemap/film path uses).
    std::array<float, 9> gainmapMatrix{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    bool hasGainmapMatrix = false;
    // Linear scene-exposure gain for the HDR tap (mirrors the device
    // aePostGain * 2^exposureEV / folded film EV). Default 1.0 = legacy.
    float gainmapExposure = 1.0f;
    gainmap::GainmapParams gainmapParams{};
};

void check(VkResult result, const char* what) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(what) + " VkResult=" + std::to_string(result));
    }
}

std::vector<uint8_t> readBytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("cannot open " + path.string());
    const std::streamoff size = file.tellg();
    if (size < 0) throw std::runtime_error("cannot size " + path.string());
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!file) throw std::runtime_error("cannot read " + path.string());
    return bytes;
}

std::vector<uint32_t> readWords(const std::filesystem::path& path) {
    const auto bytes = readBytes(path);
    if (bytes.empty() || (bytes.size() & 3u) != 0u) {
        throw std::runtime_error("invalid SPIR-V " + path.string());
    }
    std::vector<uint32_t> words(bytes.size() / sizeof(uint32_t));
    std::memcpy(words.data(), bytes.data(), bytes.size());
    return words;
}

template <size_t N>
std::array<float, N> parseFloatArray(const std::string& text, const char* key) {
    std::array<float, N> out{};
    std::istringstream stream(text);
    std::string token;
    for (size_t i = 0; i < N; ++i) {
        if (!std::getline(stream, token, ',')) {
            throw std::runtime_error(std::string("invalid ") + key);
        }
        out[i] = std::stof(token);
    }
    if (std::getline(stream, token, ',')) {
        throw std::runtime_error(std::string("invalid ") + key);
    }
    return out;
}

CaptureConfig loadConfig(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) throw std::runtime_error("cannot open config " + path.string());
    std::map<std::string, std::string> values;
    std::string line;
    while (std::getline(file, line)) {
        const auto separator = line.find('=');
        if (separator != std::string::npos) {
            values[line.substr(0, separator)] = line.substr(separator + 1u);
        }
    }
    CaptureConfig c{};
    if (values.count("source")) c.source = values.at("source");
    if (values.count("width")) c.width = static_cast<uint32_t>(std::stoul(values.at("width")));
    if (values.count("height")) c.height = static_cast<uint32_t>(std::stoul(values.at("height")));
    if (values.count("cfa")) c.cfa = static_cast<uint32_t>(std::stoul(values.at("cfa")));
    if (values.count("fccSteps")) c.fccSteps = static_cast<uint32_t>(std::stoul(values.at("fccSteps")));
    if (values.count("fccEdgeSigma")) c.fccEdgeSigma = std::stof(values.at("fccEdgeSigma"));
    if (values.count("fccChromaBound")) c.fccChromaBound = std::stof(values.at("fccChromaBound"));
    if (values.count("defringeStrength")) c.defringeStrength = std::stof(values.at("defringeStrength"));
    if (values.count("defringeEdgeThreshold")) c.defringeEdgeThreshold = std::stof(values.at("defringeEdgeThreshold"));
    if (values.count("defringeLumaFloor")) c.defringeLumaFloor = std::stof(values.at("defringeLumaFloor"));
    if (values.count("dualAutoContrast")) c.dualAutoContrast = std::stoul(values.at("dualAutoContrast")) != 0u;
    if (values.count("dualContrastPercent")) c.dualContrastPercent = std::stof(values.at("dualContrastPercent"));
    if (values.count("exposureEV")) c.exposureEV = std::stof(values.at("exposureEV"));
    if (values.count("wbRgb")) c.wbRgb = parseFloatArray<3>(values.at("wbRgb"), "wbRgb");
    if (values.count("cameraToWorkingColumnMajor")) {
        c.cameraToWorking = parseFloatArray<9>(values.at("cameraToWorkingColumnMajor"), "cameraToWorkingColumnMajor");
    }
    if (!c.width || !c.height || (c.width & 1u) || (c.height & 1u) || c.cfa > 3u) {
        throw std::runtime_error("invalid replay geometry/CFA in config");
    }
    if (c.fccSteps > 8u) throw std::runtime_error("fccSteps must be 0..8");
    return c;
}

Arguments parseArguments(int argc, char** argv) {
    Arguments args{};
    for (int i = 1; i < argc; ++i) {
        const std::string value = argv[i];
        if (value == "--input" && i + 1 < argc) args.input = argv[++i];
        else if (value == "--linear-rgb") args.linearRgb = true;
        else if (value == "--demosaiced-rgb") args.demosaicedRgb = true;
        else if (value == "--dump-demosaic") args.dumpDemosaic = true;
        else if (value == "--rcd") args.rcdOnly = true;
        else if (value == "--benchmark-post" && i + 1 < argc) args.benchmarkPost = std::stoul(argv[++i]);
        else if (value == "--config" && i + 1 < argc) args.config = argv[++i];
        else if (value == "--output-dir" && i + 1 < argc) args.outputDir = argv[++i];
        else if (value == "--technical-lut-dwg" && i + 1 < argc) args.technicalLutDwg = argv[++i];
        else if (value == "--quadfix") args.quadfix = true;
        else if (value == "--quadfix-fast-median") { args.quadfix = true; args.quadfixFastMedian = true; }
        else if (value == "--single-recovery" && i + 1 < argc) {
            args.singleVariant = true;
            args.singleRecovery = std::stoul(argv[++i]) != 0u;
        }
        else if (value == "--preserve-reconstructed") args.preserveReconstructedHighlights = true;
        else if (value == "--exposure-ev" && i + 1 < argc) {
            args.exposureEV = std::stof(argv[++i]);
            args.hasExposureEV = true;
        } else if (value == "--post-gain" && i + 1 < argc) {
            args.postGain = std::stof(argv[++i]);
        } else if (value == "--highlight-bias" && i + 1 < argc) {
            args.highlightBias = std::stof(argv[++i]);
        } else if (value == "--rt-highlight-compression" && i + 1 < argc) {
            args.rtHighlightCompression = std::stof(argv[++i]);
        } else if (value == "--app-coloropp") {
            args.appColoropp = true;
        } else if (value == "--app-hlth" && i + 1 < argc) {
            args.appColoroppThreshold = std::stof(argv[++i]);
        } else if (value == "--app-compression" && i + 1 < argc) {
            args.appColoroppCompression = std::stof(argv[++i]);
        } else if (value == "--film") {
            args.film = true;
        } else if (value == "--spirv-dir" && i + 1 < argc) {
            args.spirvDir = argv[++i];
        } else if (value == "--hanatos" && i + 1 < argc) {
            args.hanatosPath = argv[++i];
        } else if (value == "--gamut" && i + 1 < argc) {
            args.gamutPath = argv[++i];
        } else if (value == "--film-cases" && i + 1 < argc) {
            args.filmCases = argv[++i];
        } else if (value == "--film-grain" && i + 1 < argc) {
            args.filmGrain = std::stoul(argv[++i]) != 0u;
        } else if (value == "--film-recovery" && i + 1 < argc) {
            args.filmRecovery = std::stoul(argv[++i]) != 0u;
        } else if (value == "--film-dir-amount" && i + 1 < argc) {
            args.filmDirAmount = std::stof(argv[++i]);
        } else if (value == "--film-print-diffusion" && i + 1 < argc) {
            args.filmPrintDiffusion = std::stoul(argv[++i]) != 0u;
        } else if (value == "--film-halation" && i + 1 < argc) {
            args.filmHalation = std::stoul(argv[++i]) != 0u;
        } else if (value == "--film-camera-diffusion" && i + 1 < argc) {
            args.filmCameraDiffusion = std::stoul(argv[++i]) != 0u;
        } else if (value == "--film-matrix" && i + 1 < argc) {
            args.filmMatrix = parseFloatArray<9>(argv[++i], "film-matrix");
        } else if (value == "--dump-hdr") {
            args.dumpHdr = true;
        } else if (value == "--gainmap") {
            args.gainmap = true;
        } else if (value == "--gainmap-matrix" && i + 1 < argc) {
            args.gainmapMatrix = parseFloatArray<9>(argv[++i], "gainmap-matrix");
            args.hasGainmapMatrix = true;
        } else if (value == "--gainmap-max-log2" && i + 1 < argc) {
            args.gainmapParams.maxLog2 = std::stof(argv[++i]);
        } else if (value == "--gainmap-min-log2" && i + 1 < argc) {
            args.gainmapParams.minLog2 = std::stof(argv[++i]);
        } else if (value == "--gainmap-exposure" && i + 1 < argc) {
            args.gainmapExposure = std::stof(argv[++i]);
        } else if (value == "--gainmap-sat-protect" && i + 1 < argc) {
            args.gainmapParams.satProtect = std::stof(argv[++i]);
        } else if (value == "--gainmap-blur-sigma" && i + 1 < argc) {
            args.gainmapParams.mapBlurSigma = std::stof(argv[++i]);
        } else if (value == "--gainmap-rgb") {
            args.gainmapParams.multiChannelMap = true;
        } else if (value == "--gainmap-glow-strength" && i + 1 < argc) {
            args.gainmapParams.glowStrength = std::stof(argv[++i]);
        } else if (value == "--gainmap-glow-max" && i + 1 < argc) {
            args.gainmapParams.glowMax = std::stof(argv[++i]);
        } else throw std::runtime_error(
            "usage: rawr_offline_pipeline --input tagged.rgba16f --config replay.txt --output-dir DIR [--exposure-ev EV]\n"
            "       [--film --spirv-dir DIR --hanatos F.f32 --gamut G.f32 [--film-cases f:p,...]\n"
            "        [--film-grain 0|1] [--film-recovery 0|1] [--film-matrix m00,..,m22]]");
    }
    if (args.input.empty() || args.config.empty() || args.outputDir.empty()) {
        throw std::runtime_error("usage: rawr_offline_pipeline --input tagged.rgba16f --config replay.txt --output-dir DIR");
    }
    if (args.preserveReconstructedHighlights && (!args.singleVariant || args.singleRecovery))
        throw std::runtime_error("--preserve-reconstructed requires --single-recovery 0");
    if (args.rtHighlightCompression < 0 || args.rtHighlightCompression > 500)
        throw std::runtime_error("RT highlight compression must be 0..500");
    if (args.appColoropp && (args.preserveReconstructedHighlights ||
                             args.rtHighlightCompression != 0 || !args.singleVariant || !args.singleRecovery))
        throw std::runtime_error("--app-coloropp requires --single-recovery 1");
    if (args.demosaicedRgb && (args.linearRgb || args.dumpDemosaic || args.quadfix))
        throw std::runtime_error("--demosaiced-rgb cannot be combined with linear RGB, dump, or quadfix");
    if (args.film && (args.spirvDir.empty() || args.hanatosPath.empty() || args.gamutPath.empty())) {
        throw std::runtime_error("--film requires --spirv-dir, --hanatos and --gamut");
    }
    return args;
}

std::vector<uint32_t> loadSpvFile(const std::filesystem::path& dir, const char* name) {
    return readWords(dir / name);
}

std::vector<std::tuple<float, float, float>> parseFilmCases(const std::string& text) {
    // Entries are "filmEV:printEV" or "filmEV:printEV:pushStops".
    std::vector<std::tuple<float, float, float>> cases;
    std::istringstream stream(text);
    std::string token;
    while (std::getline(stream, token, ',')) {
        const auto first = token.find(':');
        if (first == std::string::npos) throw std::runtime_error("invalid --film-cases entry: " + token);
        const auto second = token.find(':', first + 1u);
        const float filmEv = std::stof(token.substr(0, first));
        const float printEv =
            std::stof(token.substr(first + 1u, second == std::string::npos ? std::string::npos : second - first - 1u));
        const float push = second == std::string::npos ? 0.0f : std::stof(token.substr(second + 1u));
        cases.emplace_back(filmEv, printEv, push);
    }
    if (cases.empty()) throw std::runtime_error("--film-cases is empty");
    return cases;
}

std::string filmStem(float filmEv, float printEv, float push) {
    auto fmt = [](float v) {
        std::ostringstream s;
        s << (v < 0.0f ? "m" : "p") << std::fixed << std::setprecision(2) << std::abs(v);
        return s.str();
    };
    std::string stem = "film_f" + fmt(filmEv) + "_p" + fmt(printEv);
    if (push != 0.0f) stem += "_push" + fmt(push);
    return stem;
}

VkImageMemoryBarrier imageBarrier(VkImage image, VkAccessFlags sourceAccess,
                                  VkAccessFlags destinationAccess, VkImageLayout oldLayout,
                                  VkImageLayout newLayout) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = sourceAccess;
    barrier.dstAccessMask = destinationAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return barrier;
}

VkCommandBuffer allocateCommand(VkDevice device, VkCommandPool pool) {
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    info.commandPool = pool;
    info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    info.commandBufferCount = 1;
    check(vkAllocateCommandBuffers(device, &info, &command), "allocate command buffer");
    return command;
}

void beginCommand(VkCommandBuffer command) {
    check(vkResetCommandBuffer(command, 0), "reset command buffer");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(command, &begin), "begin command buffer");
}

void submitAndWait(const Ctx& context, VkCommandBuffer command) {
    check(vkEndCommandBuffer(command), "end command buffer");
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    check(vkQueueSubmit(context.q, 1, &submit, VK_NULL_HANDLE), "submit command buffer");
    check(vkQueueWaitIdle(context.q), "wait queue");
}

Buf createHostBuffer(const Ctx& c, VkDeviceSize size, VkBufferUsageFlags usage) {
    return vktest::mkBuf(c.pd, c.dev, size, usage,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
}

void uploadImage(const Ctx& c, VkCommandBuffer command, const void* pixels,
                 size_t byteCount, Img& image, uint32_t width, uint32_t height) {
    Buf staging = createHostBuffer(c, byteCount, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    void* mapped = nullptr;
    check(vkMapMemory(c.dev, staging.m, 0, byteCount, 0, &mapped), "map upload");
    std::memcpy(mapped, pixels, byteCount);
    vkUnmapMemory(c.dev, staging.m);

    beginCommand(command);
    auto toTransfer = imageBarrier(image.i, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                                   VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toTransfer);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(command, staging.b, image.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    auto toCompute = imageBarrier(image.i, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toCompute);
    submitAndWait(c, command);
    vktest::delBuf(c.dev, staging);
}

dual::BayerPattern bayerPattern(uint32_t cfa) {
    return static_cast<dual::BayerPattern>(cfa);
}

void writePpm(const std::filesystem::path& path, const uint8_t* rgba,
              uint32_t width, uint32_t height) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) throw std::runtime_error("cannot write " + path.string());
    file << "P6\n" << width << ' ' << height << "\n255\n";
    for (size_t i = 0, count = static_cast<size_t>(width) * height; i < count; ++i) {
        file.write(reinterpret_cast<const char*>(rgba + i * 4u), 3);
    }
    if (!file) throw std::runtime_error("failed writing " + path.string());
}

void decodeTaggedPacked(const std::vector<uint8_t>& tagged, std::vector<uint8_t>& packed,
                        std::vector<uint16_t>& clipState) {
    if ((tagged.size() & 7u) != 0u) throw std::runtime_error("tagged RGBA16F byte count is invalid");
    packed.resize(tagged.size());
    clipState.resize(tagged.size() / 8u);
    for (size_t texel = 0; texel < clipState.size(); ++texel) {
        uint16_t state = 0;
        for (uint32_t channel = 0; channel < 4; ++channel) {
            const size_t offset = texel * 8u + channel * 2u;
            uint16_t bits = 0;
            std::memcpy(&bits, tagged.data() + offset, sizeof(bits));
            if ((bits & 0x8000u) != 0u) state |= static_cast<uint16_t>(1u << channel);
            bits &= 0x7fffu;
            std::memcpy(packed.data() + offset, &bits, sizeof(bits));
        }
        clipState[texel] = state;
    }
}

}  // namespace

int main(int argc, char** argv) {
    Ctx context{};
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    Img packedImage{}, clipImage{};
    Buf quadInput{}, quadFiltered{};
    try {
        const Arguments args = parseArguments(argc, argv);
        CaptureConfig config = loadConfig(args.config);
        if (args.hasExposureEV) config.exposureEV = args.exposureEV;
        std::filesystem::create_directories(args.outputDir);

        constexpr float ap1ToSrgb[9] = {1.70485868f, -.621716f, -.08314268f, -.13007682f, 1.14073577f,
                                        -.01065895f, -.02396407f, -.12897551f, 1.15293958f};
        std::array<float, 9> cameraToSrgb{};
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 3; ++x)
                for (int k = 0; k < 3; ++k)
                    cameraToSrgb[y * 3 + x] += ap1ToSrgb[y * 3 + k] * config.cameraToWorking[x * 3 + k];
        const auto tagged = readBytes(args.input);
        const size_t expected = static_cast<size_t>(config.width) * config.height * ((args.linearRgb || args.demosaicedRgb) ? 8u : 2u);
        if (tagged.size() != expected) {
            throw std::runtime_error("tagged input size mismatch: got " +
                                     std::to_string(tagged.size()) + " expected " +
                                     std::to_string(expected));
        }
        std::vector<uint8_t> packed;
        std::vector<uint16_t> clipState;
        if (!args.linearRgb && !args.demosaicedRgb) decodeTaggedPacked(tagged, packed, clipState);
        size_t clippedComponents = 0;
        for (const uint16_t state : clipState) {
            for (uint32_t bit = 0; bit < 4; ++bit) clippedComponents += (state >> bit) & 1u;
        }

        context = vktest::ctx();
        if (args.quadfix && config.cfa != 0u)
            throw std::runtime_error("Host quadfix packed-CFA replay currently requires RGGB");
        std::cout << "RAWR_OFFLINE_GPU name=\"" << context.prop.deviceName
                  << "\" source=" << config.source << " dimensions=" << config.width
                  << 'x' << config.height << " clipped_components=" << clippedComponents << '\n';

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = context.qf;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        check(vkCreateCommandPool(context.dev, &poolInfo, nullptr, &pool), "create command pool");
        command = allocateCommand(context.dev, pool);

        if (!args.linearRgb && !args.demosaicedRgb) {
        packedImage = vktest::mkImg(context.pd, context.dev, config.width / 2u, config.height / 2u,
                                    VK_FORMAT_R16G16B16A16_SFLOAT,
                                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        clipImage = vktest::mkImg(context.pd, context.dev, config.width / 2u, config.height / 2u,
                                  VK_FORMAT_R16_UINT,
                                  VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        uploadImage(context, command, packed.data(), packed.size(), packedImage,
                    config.width / 2u, config.height / 2u);
        uploadImage(context, command, clipState.data(), clipState.size() * sizeof(uint16_t), clipImage,
                    config.width / 2u, config.height / 2u);
        if (args.quadfix) {
            const VkDeviceSize rawBytes=VkDeviceSize(config.width)*config.height*sizeof(float);
            quadInput=createHostBuffer(context,rawBytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            quadFiltered=createHostBuffer(context,rawBytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            void* mapped=nullptr;
            check(vkMapMemory(context.dev,quadInput.m,0,rawBytes,0,&mapped),"map quadfix input");
            auto* values=static_cast<float*>(mapped);
            for (uint32_t y=0;y<config.height;++y)
                for (uint32_t x=0;x<config.width;++x) {
                    const size_t texel=(size_t(y/2)*(config.width/2)+x/2)*4u + (y%2)*2u+x%2;
                    uint16_t bits=0;
                    std::memcpy(&bits,packed.data()+texel*2u,sizeof(bits));
                    // Packed CFA stores the clip flag in the half sign bit.
                    // DngSource::normalized supplies quadfix in 0..255 units.
                    values[size_t(y)*config.width+x]=vktest::halfToFloat(bits & 0x7fffu)*255.0f;
                }
            vkUnmapMemory(context.dev,quadInput.m);
            const std::filesystem::path shaderDir=RAWR_OFFLINE_SHADER_DIR;
            quadfix::PipelineConfig cfg{};
            cfg.width=config.width; cfg.height=config.height;
            cfg.fastMedian=args.quadfixFastMedian;
            quadfix::QuadfixPipeline filter(
                quadfix::VulkanContext{context.pd,context.dev,context.qf,nullptr},
                [shaderDir](std::string_view name) { return readWords(shaderDir/(std::string(name)+".spv")); },cfg);
            beginCommand(command);
            filter.record(command,
                quadfix::BayerBufferView{{quadInput.b,0,rawBytes},config.width,config.height},
                quadfix::BayerBufferView{{quadFiltered.b,0,rawBytes},config.width,config.height});
            submitAndWait(context,command);
        }
        }

        {
        const std::filesystem::path shaderDir = RAWR_OFFLINE_SHADER_DIR;
        const auto shaderProvider = [shaderDir](std::string_view name) {
            return readWords(shaderDir / (std::string(name) + ".spv"));
        };
        dual::PipelineConfig dualConfig{};
        dualConfig.autoBalance = true;
        dualConfig.width = config.width;
        dualConfig.height = config.height;
        dualConfig.pattern = bayerPattern(config.cfa);
        dualConfig.inputMode = args.quadfix ? dual::InputMode::NormalizedFloatBuffer
                                             : dual::InputMode::PackedCfaRgba16fImage;
        dualConfig.outputScale = 1.0f / 255.0f;
        dualConfig.outputAlpha = 1.0f;
        dualConfig.contrastPercent = config.dualContrastPercent;
        dualConfig.autoContrast = config.dualAutoContrast;
        dualConfig.telemetry = true;
        dualConfig.optimizationMode = dual::OptimizationMode::VngExportBlend;
        std::unique_ptr<dual::DualDemosaicPipeline> demosaic;
        if (args.rcdOnly && (args.quadfix || args.linearRgb || args.demosaicedRgb))
            throw std::runtime_error("--rcd requires packed CFA input without quadfix");
        std::unique_ptr<rcd::RcdPipeline> rcdDemosaic;
        if (args.rcdOnly) {
            rcd::PipelineConfig cfg{};
            cfg.autoBalance = true;
            cfg.width = config.width;
            cfg.height = config.height;
            cfg.pattern = static_cast<rcd::BayerPattern>(config.cfa);
            cfg.inputMode = rcd::InputMode::PackedCfaRgba16fImage;
            rcdDemosaic = std::make_unique<rcd::RcdPipeline>(
                rcd::VulkanContext{context.pd, context.dev, context.qf, nullptr}, shaderProvider, cfg);
        }
        if (!args.rcdOnly && !args.linearRgb && !args.demosaicedRgb) demosaic = std::make_unique<dual::DualDemosaicPipeline>(
            dual::VulkanContext{context.pd, context.dev, context.qf, nullptr}, shaderProvider, dualConfig);

        if (!args.technicalLutDwg.empty() && args.film)
            throw std::runtime_error("--technical-lut-dwg cannot be combined with --film");
        tonemap::lut::LutChain technicalLut;
        if (!args.technicalLutDwg.empty()) {
            technicalLut.inputSpace = {tonemap::color::Gamut::DaVinciWideGamut,
                                      tonemap::color::TransferFunction::DaVinciIntermediate};
            technicalLut.placement = tonemap::lut::LutPlacement::RenderTransform;
            technicalLut.afterAction = tonemap::lut::LutAfterAction::UseDirectly;
            technicalLut.stages.push_back(tonemap::lut::parseCubeFile(args.technicalLutDwg.string()));
        }
        auto tonemapSpirv = readWords(shaderDir / "tonemap.comp.spv");
        tonemap::TonemapCreateInfo tonemapCreate{};
        tonemapCreate.context = {context.pd, context.dev, nullptr};
        tonemapCreate.shaderSpirv = tonemapSpirv.data();
        tonemapCreate.shaderSpirvBytes = tonemapSpirv.size() * sizeof(uint32_t);
        tonemapCreate.maxFramesInFlight = 1;
        if (!args.technicalLutDwg.empty()) tonemapCreate.lutChain = &technicalLut;
        tonemap::TonemapEngine tonemap(tonemapCreate);

        // Production UltraHDR gain map stage (optional).
        auto gainmapSpirv = readWords(shaderDir / "gainmap.comp.spv");
        auto gainmapBlurSpirv = readWords(shaderDir / "gainmap_blur.comp.spv");
        std::unique_ptr<gainmap::GainmapCompute> gainmapEngine;
        if (args.gainmap) {
            gainmap::GainmapCreateInfo gainmapCreate{};
            gainmapCreate.context = {context.pd, context.dev, nullptr};
            gainmapCreate.shaderSpirv = gainmapSpirv.data();
            gainmapCreate.shaderSpirvBytes = gainmapSpirv.size() * sizeof(uint32_t);
            gainmapCreate.blurShaderSpirv = gainmapBlurSpirv.data();
            gainmapCreate.blurShaderSpirvBytes = gainmapBlurSpirv.size() * sizeof(uint32_t);
            gainmapCreate.maxFramesInFlight = 1;
            gainmapEngine = std::make_unique<gainmap::GainmapCompute>(gainmapCreate);
        }

        // Film A/B engine: shot look baked once (Xtra 400 / Crystal Archive
        // II / Hanatos 2026 / print simulation, matching the device EXIF);
        // per-case EVs ride the record look. Mirrors the app still engine
        // (RenderedStillProcessor: full-res, 1 frame in flight, conditional
        // effect scratch).
        std::unique_ptr<spektrafilm_native::SpektraFilm> film;
        spektrafilm_native::FilmLook filmBaseLook{};
        std::vector<std::vector<uint32_t>> filmSpvStorage;
        std::vector<uint8_t> filmHanatosBytes;
        std::vector<uint8_t> filmGamutBytes;
        if (args.film) {
            filmBaseLook.film = 15;
            filmBaseLook.paper = 5;
            filmBaseLook.inputColorSpace = 15;
            filmBaseLook.outputColorSpace = 25;
            filmBaseLook.rgbToRawMethod = 2;
            filmBaseLook.process = 0;
            filmBaseLook.grainEnabled = args.filmGrain;
            filmBaseLook.grainModel = 0;
            filmBaseLook.filmFormat = 4;
            filmBaseLook.grainSeed = 1;
            filmBaseLook.grainAnimate = false;
            filmBaseLook.dirCouplersAmount = args.filmDirAmount;
            filmBaseLook.printDiffusionEnabled = args.filmPrintDiffusion;
            filmBaseLook.halationEnabled = args.filmHalation;
            filmBaseLook.cameraDiffusionEnabled = args.filmCameraDiffusion;
            filmBaseLook.printDiffusionFamily = 1;
            filmBaseLook.outputRole = 0;
            static const char* kFilmSpvNames[13] = {
                "spektra_input.comp.spv",       "SpektraFilmExposure.comp.spv", "SpektraCurveDevelop.comp.spv",
                "SpektraPrintScan.comp.spv",    "spektra_output.comp.spv",       "spektra_output_half.comp.spv",
                "spektra_boost_milestone.comp.spv", "SpektraGrain.comp.spv",     "SpektraDir.comp.spv",
                "SpektraHalation.comp.spv",     "SpektraDiffusion.comp.spv",     "SpektraScannerPost.comp.spv",
                "spektra_glow_ratio.comp.spv"};
            for (const char* name : kFilmSpvNames) filmSpvStorage.push_back(loadSpvFile(args.spirvDir, name));
            const auto words = [&](size_t i) {
                return reinterpret_cast<const uint32_t*>(filmSpvStorage[i].data());
            };
            const auto bytes = [&](size_t i) { return filmSpvStorage[i].size() * sizeof(uint32_t); };
            filmHanatosBytes = readBytes(args.hanatosPath);
            filmGamutBytes = readBytes(args.gamutPath);
            spektrafilm_native::SpektraFilmCreateInfo filmCreate{};
            filmCreate.context.physicalDevice = context.pd;
            filmCreate.context.device = context.dev;
            filmCreate.queue = context.q;
            filmCreate.queueFamilyIndex = context.qf;
            filmCreate.maxFramesInFlight = 1;
            filmCreate.maxWidth = config.width;
            filmCreate.maxHeight = config.height;
            filmCreate.look = filmBaseLook;
            filmCreate.conditionalEffectScratch = true;
            filmCreate.inputSpirv = words(0);
            filmCreate.inputSpirvBytes = bytes(0);
            filmCreate.exposureSpirv = words(1);
            filmCreate.exposureSpirvBytes = bytes(1);
            filmCreate.developSpirv = words(2);
            filmCreate.developSpirvBytes = bytes(2);
            filmCreate.printScanSpirv = words(3);
            filmCreate.printScanSpirvBytes = bytes(3);
            filmCreate.outputSpirv = words(4);
            filmCreate.outputSpirvBytes = bytes(4);
            filmCreate.hdrOutputSpirv = words(5);
            filmCreate.hdrOutputSpirvBytes = bytes(5);
            filmCreate.boostMilestoneSpirv = words(6);
            filmCreate.boostMilestoneSpirvBytes = bytes(6);
            filmCreate.grainSpirv = words(7);
            filmCreate.grainSpirvBytes = bytes(7);
            filmCreate.dirSpirv = words(8);
            filmCreate.dirSpirvBytes = bytes(8);
            filmCreate.halationSpirv = words(9);
            filmCreate.halationSpirvBytes = bytes(9);
            filmCreate.diffusionSpirv = words(10);
            filmCreate.diffusionSpirvBytes = bytes(10);
            filmCreate.scannerSpirv = words(11);
            filmCreate.scannerSpirvBytes = bytes(11);
            filmCreate.glowRatioSpirv = words(12);
            filmCreate.glowRatioSpirvBytes = bytes(12);
            filmCreate.hanatosSpectra = reinterpret_cast<const float*>(filmHanatosBytes.data());
            filmCreate.hanatosSpectraFloats = filmHanatosBytes.size() / sizeof(float);
            filmCreate.gamutCompression = reinterpret_cast<const float*>(filmGamutBytes.data());
            filmCreate.gamutCompressionFloats = filmGamutBytes.size() / sizeof(float);
            const char* filmReason = nullptr;
            if (!spektrafilm_native::SpektraFilm::validateCreateInfo(filmCreate, &filmReason)) {
                throw std::runtime_error(std::string("film create rejected: ") + (filmReason ? filmReason : "?"));
            }
            film = std::make_unique<spektrafilm_native::SpektraFilm>(filmCreate);
        }

        std::ofstream manifest(args.outputDir / "manifest.txt", std::ios::trunc);
        manifest << "source=" << config.source << "\n"
                 << "exposureEV=" << config.exposureEV << "\n"
                 << "gpu=" << context.prop.deviceName << "\n"
                 << "width=" << config.width << "\nheight=" << config.height << "\n"
                 << "cfa=" << config.cfa << "\n"
                 << "demosaic=" << (args.rcdOnly ? "production_rcd" : "production_dual_rcd_vng4") << "\n"
                 << "quadfix=" << (args.quadfix ? (args.quadfixFastMedian ? "fast" : "exact") : "off") << "\n"
                 << "post=production_wb_highlight_fcc\n"
                 << "tonemap=" << (args.technicalLutDwg.empty() ? "production_rawr_base" : "technical_lut_dwg_direct") << "\n"
                 << "technicalLutDwg=" << args.technicalLutDwg.string() << "\n"
                 << "clippedComponents=" << clippedComponents << "\n";
        // Direct-RGB checkpoints (--linear-rgb / --dump-linear) are pre-LSC
        // merge output. Device V2 JPEG applies LSC in prepare_rgb (~1.78x
        // corners on tuning captures), so offline linear renders are NOT
        // color-comparable at the corners until an LSC buffer input is added.
        if (args.linearRgb) {
            manifest << "lsc=missing_pre_lsc_checkpoint_not_device_comparable\n";
            std::cout << "RAWR_OFFLINE_WARN linear-rgb has no LSC; corners differ from device prepare_rgb\n";
        }

        struct ReplayVariant {
            std::string stem;
            bool recovery = false;
            float highlights = 0.0f;
            bool film = false;
            float filmEv = 0.0f;
            float printEv = 0.0f;
            float pushStops = 0.0f;
        };
        std::vector<ReplayVariant> variants;
        if (args.film) {
            for (const auto& filmCase : parseFilmCases(args.filmCases)) {
                variants.push_back({filmStem(std::get<0>(filmCase), std::get<1>(filmCase), std::get<2>(filmCase)),
                                    args.filmRecovery, 0.0f, true, std::get<0>(filmCase), std::get<1>(filmCase),
                                    std::get<2>(filmCase)});
            }
            manifest << "film=1 grain=" << (args.filmGrain ? 1 : 0) << " recovery=" << (args.filmRecovery ? 1 : 0)
                     << " halation=" << (args.filmHalation ? 1 : 0)
                     << " cameraDiffusion=" << (args.filmCameraDiffusion ? 1 : 0)
                     << " cases=" << args.filmCases << "\n"
                     << "filmMatrix=" << args.filmMatrix[0] << ',' << args.filmMatrix[1] << ',' << args.filmMatrix[2]
                     << ',' << args.filmMatrix[3] << ',' << args.filmMatrix[4] << ',' << args.filmMatrix[5] << ','
                     << args.filmMatrix[6] << ',' << args.filmMatrix[7] << ',' << args.filmMatrix[8] << "\n";
        } else {
            for (const bool recovery : {false, true}) {
                if (args.singleVariant && recovery != args.singleRecovery) continue;
                for (const float highlights : {-100.0f, 0.0f}) {
                    if (args.singleVariant && highlights != 0.0f) continue;
                    const float selectedHighlights = args.singleVariant ? args.highlightBias : highlights;
                    variants.push_back({std::string("recon_") + (recovery ? "on" : "off") +
                                            (highlights < -50.0f ? "_hm100" : "_h0"),
                                        recovery, selectedHighlights, false, 0.0f, 0.0f});
                }
            }
        }
        for (const auto& variant : variants) {
            {
                const bool recovery = variant.recovery;
                const float highlights = variant.highlights;
                const std::string stem = variant.stem;
                Img source = vktest::mkImg(context.pd, context.dev, config.width, config.height,
                                           VK_FORMAT_R16G16B16A16_SFLOAT,
                                           VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
                Img toneImage{};
                VkDescriptorSetLayout toneSetLayout = VK_NULL_HANDLE;
                VkDescriptorPool toneDescriptorPool = VK_NULL_HANDLE;
                VkPipelineLayout tonePipelineLayout = VK_NULL_HANDLE;
                VkShaderModule toneShader = VK_NULL_HANDLE;
                VkPipeline tonePipeline = VK_NULL_HANDLE;
                Img output = vktest::mkImg(context.pd, context.dev, config.width, config.height,
                                           VK_FORMAT_R8G8B8A8_UNORM,
                                           VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
                Buf readback = createHostBuffer(context,
                    static_cast<VkDeviceSize>(config.width) * config.height * 4u,
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT);
                // UltraHDR replay resources: half-res map image plus staging
                // for the HDR linear dump and the map. Film variants run the
                // gain map too (scene-linear tap with folded EV, plus the
                // glow quotient when the look has a linear scatter path).
                const uint32_t mapW = std::max(1u, config.width / 2u);
                const uint32_t mapH = std::max(1u, config.height / 2u);
                const bool wantHdrDump = args.dumpHdr;
                const bool wantGainmap = args.gainmap;
                Img mapImg{};
                bool mapImgOwned = false;
                Buf hdrReadback{};
                bool hdrReadbackOwned = false;
                Buf mapReadback{};
                bool mapReadbackOwned = false;
                // Film glow-quotient transient (film+gainmap only).
                Img glowImg{};
                bool glowImgOwned = false;
                Buf glowReadback{};
                bool glowReadbackOwned = false;
                bool useGlow = false;
                if (wantGainmap) {
                    mapImg = vktest::mkImg(context.pd, context.dev, mapW, mapH, VK_FORMAT_R8G8B8A8_UNORM,
                                           VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
                    mapImgOwned = true;
                    mapReadback = createHostBuffer(
                        context, static_cast<VkDeviceSize>(mapW) * mapH * 4u, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
                    mapReadbackOwned = true;
                }
                if (wantHdrDump) {
                    hdrReadback = createHostBuffer(
                        context, static_cast<VkDeviceSize>(config.width) * config.height * 8u,
                        VK_BUFFER_USAGE_TRANSFER_DST_BIT);
                    hdrReadbackOwned = true;
                }
                rawr::post::PostDemosaicProcessor post(
                    context.pd, context.dev, context.qf, config.width, config.height, config.fccSteps,
                    false,config.fccEdgeSigma,config.fccChromaBound,config.defringeStrength,
                    config.defringeEdgeThreshold,config.defringeLumaFloor);

                try {
                if (args.linearRgb || args.demosaicedRgb)
                    uploadImage(context, command, tagged.data(), tagged.size(), source, config.width, config.height);
                beginCommand(command);
                if (!args.linearRgb && !args.demosaicedRgb) {
                auto sourceInit = imageBarrier(source.i, 0, VK_ACCESS_SHADER_WRITE_BIT,
                                               VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
                vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                     0, nullptr, 0, nullptr, 1, &sourceInit);
                dual::PackedCfaImageView input{
                    packedImage.i, packedImage.v, VK_FORMAT_R16G16B16A16_SFLOAT,
                    VK_IMAGE_LAYOUT_GENERAL, config.width / 2u, config.height / 2u,
                    config.width, config.height, bayerPattern(config.cfa)};
                dual::LinearRgbImage demosaicOutput{
                    source.i, source.v, VK_FORMAT_R16G16B16A16_SFLOAT,
                    VK_IMAGE_LAYOUT_GENERAL, config.width, config.height};
                if (args.quadfix) {
                    const VkDeviceSize rawBytes=VkDeviceSize(config.width)*config.height*sizeof(float);
                    dual::NormalizedBayerBufferView normalized{
                        {quadFiltered.b,0,rawBytes},config.width,config.height,bayerPattern(config.cfa)};
                    demosaic->record(command,normalized,demosaicOutput);
                } else if (rcdDemosaic) {
                    rcd::PackedCfaImageView rcdInput{
                        packedImage.i, packedImage.v, VK_FORMAT_R16G16B16A16_SFLOAT,
                        VK_IMAGE_LAYOUT_GENERAL, config.width / 2u, config.height / 2u,
                        config.width, config.height, static_cast<rcd::BayerPattern>(config.cfa)};
                    rcd::LinearRgbImage rcdOutput{source.i, source.v, VK_FORMAT_R16G16B16A16_SFLOAT,
                                                VK_IMAGE_LAYOUT_GENERAL, config.width, config.height};
                    rcdDemosaic->record(command, rcdInput, rcdOutput);
                } else demosaic->record(command, input, demosaicOutput);
                }

                if (args.dumpDemosaic) {
                    const VkDeviceSize rgbBytes = VkDeviceSize(config.width) * config.height * 8u;
                    Buf rgbReadback = createHostBuffer(context, rgbBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
                    auto toTransfer = imageBarrier(source.i, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                                                   VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         0, 0, nullptr, 0, nullptr, 1, &toTransfer);
                    VkBufferImageCopy rgbCopy{};
                    rgbCopy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    rgbCopy.imageExtent = {config.width, config.height, 1};
                    vkCmdCopyImageToBuffer(command, source.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                           rgbReadback.b, 1, &rgbCopy);
                    submitAndWait(context, command);
                    void* rgbMapped = nullptr;
                    check(vkMapMemory(context.dev, rgbReadback.m, 0, rgbBytes, 0, &rgbMapped), "map demosaic checkpoint");
                    std::ofstream rgbOut(args.outputDir / "demosaiced_camera_rgb.rgba16f", std::ios::binary | std::ios::trunc);
                    rgbOut.write(static_cast<const char*>(rgbMapped), static_cast<std::streamsize>(rgbBytes));
                    rgbOut.close();
                    vkUnmapMemory(context.dev, rgbReadback.m);
                    vktest::delBuf(context.dev, rgbReadback);
                    beginCommand(command);
                    auto fromTransfer = imageBarrier(source.i, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                                                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
                    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                         0, 0, nullptr, 0, nullptr, 1, &fromTransfer);
                }
                if (!args.dumpDemosaic) {
                    auto demosaicReady = imageBarrier(source.i, (args.linearRgb || args.demosaicedRgb) ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_SHADER_WRITE_BIT,
                                                      VK_ACCESS_SHADER_READ_BIT,
                                                      VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
                    vkCmdPipelineBarrier(command, (args.linearRgb || args.demosaicedRgb) ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                         0, nullptr, 0, nullptr, 1, &demosaicReady);
                }
                post.record(command, source.i, source.v, clipImage.i, clipImage.v,
                            config.wbRgb, recovery, {}, VK_NULL_HANDLE, args.preserveReconstructedHighlights,
                            args.appColoropp ? 1u : 0u, args.appColoroppThreshold,
                            args.appColoroppCompression, args.postGain * std::exp2(config.exposureEV), {}, config.cfa, {}, {}, cameraToSrgb.data());

                auto postReady = imageBarrier(post.sdrOutputImage(), VK_ACCESS_SHADER_WRITE_BIT,
                                              VK_ACCESS_SHADER_READ_BIT,
                                              VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
                auto outputInit = imageBarrier(output.i, 0, VK_ACCESS_SHADER_WRITE_BIT,
                                               VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
                // Film consumes its input via copyImageToBuffer (transfer
                // read) as well as shader sampling; tonemap only samples.
                // The transfer-read dependency must be explicit — Metal faults
                // without it even though tile-based mobile GPUs tolerate the
                // gap.
                VkPipelineStageFlags beforeFilmStages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
                if (variant.film) {
                    postReady.dstAccessMask |= VK_ACCESS_TRANSFER_READ_BIT;
                    beforeFilmStages |= VK_PIPELINE_STAGE_TRANSFER_BIT;
                }
                const std::array<VkImageMemoryBarrier, 2> beforeTone{postReady, outputInit};
                vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, beforeFilmStages, 0,
                                     0, nullptr, 0, nullptr,
                                     static_cast<uint32_t>(beforeTone.size()), beforeTone.data());

                if (variant.film) {
                    // Film A/B: same demosaiced + post (WB/FCC) input as the
                    // app still path; only the EV placement differs per case.
                    spektrafilm_native::SpektraFilmRecordInfo fri{};
                    fri.commandBuffer = command;
                    fri.input = {post.outputView(), VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_LAYOUT_GENERAL,
                                 config.width, config.height};
                    fri.output = {output.v, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_GENERAL, config.width,
                                  config.height};
                    fri.frameSlot = 0;
                    fri.timeSec = 0.0;
                    fri.look = filmBaseLook;
                    fri.look.filmExposureEv = variant.filmEv;
                    fri.look.printExposureEv = variant.printEv;
                    fri.look.filmPushPullStops = variant.pushStops;
                    static_assert(sizeof(fri.sensorToLinearSrgb) == sizeof(args.filmMatrix),
                                  "film matrix size mismatch");
                    std::memcpy(fri.sensorToLinearSrgb, args.filmMatrix.data(), sizeof(fri.sensorToLinearSrgb));
                    // Glow-quotient tap (mirrors the app still path): the
                    // gain-map HDR numerator is the scene tap times this
                    // post/pre scatter quotient, so halation /
                    // camera-diffusion glow carries headroom with hue.
                    useGlow =
                        wantGainmap && args.gainmapParams.glowStrength > 0.0f &&
                        spektrafilm_native::SpektraFilm::willWriteGlowGain(
                            fri.look, config.width, config.height,
                            spektrafilm_native::GpuRenderTilingMode::LegacyFullFrame,
                            /*tileMemorySaving=*/false);
                    if (useGlow) {
                        glowImg = vktest::mkImg(context.pd, context.dev, config.width, config.height,
                                                VK_FORMAT_R16G16B16A16_SFLOAT,
                                                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
                        glowImgOwned = true;
                        glowReadback = createHostBuffer(
                            context, static_cast<VkDeviceSize>(config.width) * config.height * 8u,
                            VK_BUFFER_USAGE_TRANSFER_DST_BIT);
                        glowReadbackOwned = true;
                        auto glowInit = imageBarrier(glowImg.i, 0, VK_ACCESS_SHADER_WRITE_BIT,
                                                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
                        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                             &glowInit);
                        fri.glowGainOutput = {glowImg.v, VK_FORMAT_R16G16B16A16_SFLOAT,
                                              VK_IMAGE_LAYOUT_GENERAL, config.width, config.height};
                    }
                    const char* recordReason = nullptr;
                    if (!spektrafilm_native::SpektraFilm::validateRecordInfo(fri, 1, config.width, config.height,
                                                                            &recordReason)) {
                        throw std::runtime_error(std::string("film record rejected: ") +
                                                 (recordReason ? recordReason : "?"));
                    }
                    film->record(fri);
                } else {
                VkImageView toneInputView = post.sdrOutputView();
                if (args.rtHighlightCompression > 0.0f) {
                    toneImage = vktest::mkImg(context.pd, context.dev, config.width, config.height,
                        VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT);
                    auto toneInit = imageBarrier(toneImage.i, 0, VK_ACCESS_SHADER_WRITE_BIT,
                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
                    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toneInit);
                    VkDescriptorSetLayoutBinding toneBindings[2]{};
                    for (uint32_t i=0;i<2;++i) {
                        toneBindings[i].binding=i; toneBindings[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                        toneBindings[i].descriptorCount=1; toneBindings[i].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;
                    }
                    VkDescriptorSetLayoutCreateInfo toneLayoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
                    toneLayoutInfo.bindingCount=2; toneLayoutInfo.pBindings=toneBindings;
                    check(vkCreateDescriptorSetLayout(context.dev,&toneLayoutInfo,nullptr,&toneSetLayout),"tone descriptor layout");
                    VkDescriptorPoolSize tonePoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,2};
                    VkDescriptorPoolCreateInfo tonePoolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
                    tonePoolInfo.maxSets=1; tonePoolInfo.poolSizeCount=1; tonePoolInfo.pPoolSizes=&tonePoolSize;
                    check(vkCreateDescriptorPool(context.dev,&tonePoolInfo,nullptr,&toneDescriptorPool),"tone descriptor pool");
                    VkDescriptorSetAllocateInfo toneAlloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                    toneAlloc.descriptorPool=toneDescriptorPool; toneAlloc.descriptorSetCount=1;
                    toneAlloc.pSetLayouts=&toneSetLayout;
                    VkDescriptorSet toneSet=VK_NULL_HANDLE;
                    check(vkAllocateDescriptorSets(context.dev,&toneAlloc,&toneSet),"tone descriptor set");
                    VkDescriptorImageInfo imageInfos[2]{{VK_NULL_HANDLE,post.outputView(),VK_IMAGE_LAYOUT_GENERAL},
                                                      {VK_NULL_HANDLE,toneImage.v,VK_IMAGE_LAYOUT_GENERAL}};
                    VkWriteDescriptorSet toneWrites[2]{};
                    for(uint32_t i=0;i<2;++i) {
                        toneWrites[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                        toneWrites[i].dstSet=toneSet; toneWrites[i].dstBinding=i;
                        toneWrites[i].descriptorCount=1; toneWrites[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                        toneWrites[i].pImageInfo=&imageInfos[i];
                    }
                    vkUpdateDescriptorSets(context.dev,2,toneWrites,0,nullptr);
                    struct TonePush { uint32_t width,height;float compression,exposureGain; };
                    TonePush tonePush{config.width,config.height,args.rtHighlightCompression/100.0f,
                        args.postGain*std::exp2(config.exposureEV)};
                    VkPushConstantRange toneRange{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(TonePush)};
                    VkPipelineLayoutCreateInfo tonePipeInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
                    tonePipeInfo.setLayoutCount=1; tonePipeInfo.pSetLayouts=&toneSetLayout;
                    tonePipeInfo.pushConstantRangeCount=1; tonePipeInfo.pPushConstantRanges=&toneRange;
                    check(vkCreatePipelineLayout(context.dev,&tonePipeInfo,nullptr,&tonePipelineLayout),"tone pipeline layout");
                    auto toneWords=readWords(shaderDir/"coloropp_tone.comp.spv");
                    VkShaderModuleCreateInfo toneModuleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
                    toneModuleInfo.codeSize=toneWords.size()*4; toneModuleInfo.pCode=toneWords.data();
                    check(vkCreateShaderModule(context.dev,&toneModuleInfo,nullptr,&toneShader),"tone shader");
                    VkComputePipelineCreateInfo toneCompute{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
                    toneCompute.layout=tonePipelineLayout;
                    toneCompute.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
                    toneCompute.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;
                    toneCompute.stage.module=toneShader; toneCompute.stage.pName="main";
                    check(vkCreateComputePipelines(context.dev,VK_NULL_HANDLE,1,&toneCompute,nullptr,&tonePipeline),"tone pipeline");
                    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,tonePipeline);
                    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,tonePipelineLayout,0,1,&toneSet,0,nullptr);
                    vkCmdPushConstants(command,tonePipelineLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(tonePush),&tonePush);
                    vkCmdDispatch(command,(config.width+15)/16,(config.height+15)/16,1);
                    auto toneReady=imageBarrier(toneImage.i,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,
                        VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL);
                    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&toneReady);
                    toneInputView=toneImage.v;
                }
                auto params = tonemap::TonemapPresets::NeutralBaseline().params;
                params.exposureEV = config.exposureEV;
                params.highlightBiasEV = highlights;
                params.aePostGain = args.postGain;
                tonemap::TonemapRecordInfo tone{};
                tone.commandBuffer = command;
                tone.input = {toneInputView, VK_FORMAT_R16G16B16A16_SFLOAT,
                              VK_IMAGE_LAYOUT_GENERAL, config.width, config.height};
                tone.output = {output.v, VK_FORMAT_R8G8B8A8_UNORM,
                               VK_IMAGE_LAYOUT_GENERAL, config.width, config.height};
                tone.frameSlot = 0;
                tone.cameraToWorkingColumnMajor3x3 = config.cameraToWorking.data();
                tone.params = params;
                tonemap.record(tone);
                }
                // UltraHDR gain map (tonemap + film paths): the SDR base is
                // always the rendered output; the HDR tap is always the
                // post-WB camera linear with the path's exposure fold, plus
                // the film glow quotient when the film look wrote one.
                if (wantGainmap) {
                    auto sdrReady = imageBarrier(output.i, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                                                 VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
                    auto mapInit = imageBarrier(mapImg.i, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                                VK_IMAGE_LAYOUT_GENERAL);
                    const std::array<VkImageMemoryBarrier, 2> preGain{sdrReady, mapInit};
                    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                                         static_cast<uint32_t>(preGain.size()), preGain.data());
                    if (useGlow) {
                        auto glowReady =
                            imageBarrier(glowImg.i, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                                         VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
                        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                             &glowReady);
                    }
                    // Sensor clip mask may have been written by the highlight
                    // stages above in this same command buffer:
                    // GENERAL->GENERAL read-after-write barrier. Skipped for
                    // --linear-rgb (no clip image there; the mask stays clear
                    // and unmasked brights exercise the ratio path).
                    if (clipImage.i != VK_NULL_HANDLE) {
                        auto clipReady = imageBarrier(clipImage.i,
                                                      VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
                                                      VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                                                      VK_IMAGE_LAYOUT_GENERAL);
                        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                             &clipReady);
                    }
                    gainmap::GainmapRecordInfo gri{};
                    gri.commandBuffer = command;
                    // Effective exposure per tap (mirrors the app still
                    // path): the scene tap is pre-exposure, so film variants
                    // fold the case EV like the app's filmExposureEv fold.
                    // The glow quotient is exposure-free (post/pre cancel).
                    float effHdrExposure = args.gainmapExposure;
                    const char* hdrTap = "post";
                    gri.hdr = {post.outputView(), VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_LAYOUT_GENERAL,
                               config.width, config.height};
                    if (variant.film) {
                        effHdrExposure = std::exp2(variant.filmEv);
                        hdrTap = useGlow ? "post_filmEv+glow" : "post_filmEv";
                    }
                    if (variant.film && useGlow) {
                        gri.glow = {glowImg.v, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_LAYOUT_GENERAL,
                                    config.width, config.height};
                    }
                    gri.sdr = {output.v, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_GENERAL, config.width,
                               config.height};
                    gri.map = {mapImg.v, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_GENERAL, mapW, mapH};
                    // Real sensor clip-state mask (half-res R16UI on the map
                    // grid): exercises the mask-gated specular boost exactly
                    // like single-frame stills.
                    if (clipImage.i != VK_NULL_HANDLE)
                        gri.clip = {clipImage.v, VK_FORMAT_R16_UINT, VK_IMAGE_LAYOUT_GENERAL,
                                    config.width / 2u, config.height / 2u};
                    gri.params = args.gainmapParams;
                    gri.params.hdrExposure = effHdrExposure;
                    if (args.hasGainmapMatrix) {
                        std::memcpy(gri.params.hdrToLinearSrgbRowMajor, args.gainmapMatrix.data(),
                                    sizeof(gri.params.hdrToLinearSrgbRowMajor));
                    }
                    gainmapEngine->record(gri);
                    std::cout << "RAWR_OFFLINE_HDR_TAP file=" << stem << ".ppm tap=" << hdrTap
                              << " exposure=" << effHdrExposure << '\n';
                }

                auto copyReady = imageBarrier(output.i, VK_ACCESS_SHADER_WRITE_BIT,
                                              VK_ACCESS_TRANSFER_READ_BIT,
                                              VK_IMAGE_LAYOUT_GENERAL,
                                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                     0, nullptr, 0, nullptr, 1, &copyReady);
                VkBufferImageCopy copy{};
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.imageExtent = {config.width, config.height, 1};
                vkCmdCopyImageToBuffer(command, output.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       readback.b, 1, &copy);
                if (wantHdrDump) {
                    auto hdrReady = imageBarrier(post.outputImage(), VK_ACCESS_SHADER_READ_BIT,
                                                 VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &hdrReady);
                    VkBufferImageCopy hdrCopy{};
                    hdrCopy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    hdrCopy.imageExtent = {config.width, config.height, 1};
                    vkCmdCopyImageToBuffer(command, post.outputImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                           hdrReadback.b, 1, &hdrCopy);
                }
                if (wantGainmap) {
                    auto mapReady = imageBarrier(mapImg.i, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                                                 VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &mapReady);
                    VkBufferImageCopy mapCopy{};
                    mapCopy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    mapCopy.imageExtent = {mapW, mapH, 1};
                    vkCmdCopyImageToBuffer(command, mapImg.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                           mapReadback.b, 1, &mapCopy);
                }
                if (useGlow) {
                    // Calibration dump of the glow quotient (dimensionless
                    // post/pre scatter ratio, ~1.0 = no glow): pairs with the
                    // .hdr post-WB dump; mean should sit near 1.0.
                    auto glowReady = imageBarrier(glowImg.i, VK_ACCESS_SHADER_READ_BIT,
                                                  VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                         &glowReady);
                    VkBufferImageCopy glowCopy{};
                    glowCopy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    glowCopy.imageExtent = {config.width, config.height, 1};
                    vkCmdCopyImageToBuffer(command, glowImg.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                           glowReadback.b, 1, &glowCopy);
                }

                const auto started = std::chrono::steady_clock::now();
                submitAndWait(context, command);
                const double elapsedMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - started).count();
                if (args.benchmarkPost != 0u) {
                    VkQueryPoolCreateInfo queryInfo{};
                    queryInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
                    queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
                    queryInfo.queryCount = 2;
                    VkQueryPool queries = VK_NULL_HANDLE;
                    check(vkCreateQueryPool(context.dev, &queryInfo, nullptr, &queries), "post benchmark queries");
                    std::vector<double> samples;
                    for (uint32_t iteration = 0; iteration < args.benchmarkPost + 2u; ++iteration) {
                        beginCommand(command);
                        vkCmdResetQueryPool(command, queries, 0, 2);
                        vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, queries, 0);
                        post.record(command, source.i, source.v, clipImage.i, clipImage.v,
                                    config.wbRgb, recovery, {}, VK_NULL_HANDLE,
                                    args.preserveReconstructedHighlights, args.appColoropp ? 1u : 0u,
                                    args.appColoroppThreshold, args.appColoroppCompression,
                                    args.postGain * std::exp2(config.exposureEV), {}, config.cfa, {}, {}, cameraToSrgb.data());
                        vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, queries, 1);
                        submitAndWait(context, command);
                        uint64_t timestamps[2]{};
                        check(vkGetQueryPoolResults(context.dev, queries, 0, 2, sizeof(timestamps), timestamps,
                                                    sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
                              "post benchmark timestamps");
                        if (iteration >= 2u)
                            samples.push_back(double(timestamps[1] - timestamps[0]) *
                                              context.prop.limits.timestampPeriod / 1e6);
                    }
                    std::sort(samples.begin(), samples.end());
                    std::cout << "RAWR_OFFLINE_POST_BENCHMARK dimensions=" << config.width << 'x' << config.height
                              << " recovery=" << recovery << " fccSteps=" << config.fccSteps
                              << " defringe=" << config.defringeStrength << " samples=" << samples.size()
                              << " fccAllocatedBytes=" << post.fccAllocatedBytes()
                              << " medianGpuMs=" << samples[samples.size() / 2]
                              << " p95GpuMs=" << samples[std::min(samples.size() - 1,
                                  static_cast<size_t>(samples.size() * 0.95))] << '\n';
                    vkDestroyQueryPool(context.dev, queries, nullptr);
                }
                void* mapped = nullptr;
                const VkDeviceSize outputBytes = static_cast<VkDeviceSize>(config.width) * config.height * 4u;
                check(vkMapMemory(context.dev, readback.m, 0, outputBytes, 0, &mapped), "map output");
                writePpm(args.outputDir / (stem + ".ppm"), static_cast<const uint8_t*>(mapped),
                         config.width, config.height);
                vkUnmapMemory(context.dev, readback.m);
                if (wantHdrDump) {
                    const VkDeviceSize hdrBytes = static_cast<VkDeviceSize>(config.width) * config.height * 8u;
                    void* hdrMapped = nullptr;
                    check(vkMapMemory(context.dev, hdrReadback.m, 0, hdrBytes, 0, &hdrMapped), "map hdr");
                    std::ofstream hdrOut(args.outputDir / (stem + ".hdr.rgba16f"),
                                         std::ios::binary | std::ios::trunc);
                    if (!hdrOut) throw std::runtime_error("cannot write hdr dump");
                    hdrOut.write(static_cast<const char*>(hdrMapped), static_cast<std::streamsize>(hdrBytes));
                    hdrOut.close();
                    vkUnmapMemory(context.dev, hdrReadback.m);
                    std::ofstream sidecar(args.outputDir / (stem + ".hdr.json"), std::ios::trunc);
                    sidecar << "{\"width\":" << config.width << ",\"height\":" << config.height
                            << ",\"format\":\"R16G16B16A16_SFLOAT\",\"domain\":\"postWB_camera_linear_"
                            << (variant.film ? "film_input" : "tonemap_input") << "\",\"wbRgb\":["
                            << config.wbRgb[0] << "," << config.wbRgb[1] << "," << config.wbRgb[2] << "]}";
                    sidecar.close();
                }
                if (wantGainmap) {
                    const VkDeviceSize mapBytes = static_cast<VkDeviceSize>(mapW) * mapH * 4u;
                    void* mapMapped = nullptr;
                    check(vkMapMemory(context.dev, mapReadback.m, 0, mapBytes, 0, &mapMapped), "map gainmap");
                    std::ofstream mapOut(args.outputDir / (stem + ".gainmap.rgba8"),
                                         std::ios::binary | std::ios::trunc);
                    if (!mapOut) throw std::runtime_error("cannot write gainmap dump");
                    mapOut.write(static_cast<const char*>(mapMapped), static_cast<std::streamsize>(mapBytes));
                    mapOut.close();
                    vkUnmapMemory(context.dev, mapReadback.m);
                    const auto& gp = args.gainmapParams;
                    float dumpExposure = args.gainmapExposure;
                    const char* dumpTap = "post";
                    if (variant.film) {
                        dumpExposure = std::exp2(variant.filmEv);
                        dumpTap = useGlow ? "post_filmEv+glow" : "post_filmEv";
                    }
                    std::ofstream mside(args.outputDir / (stem + ".gainmap.json"), std::ios::trunc);
                    mside << "{\"width\":" << mapW << ",\"height\":" << mapH << ",\"format\":\"R8G8B8A8_UNORM\""
                            << ",\"minLog2\":" << gp.minLog2 << ",\"maxLog2\":" << gp.maxLog2
                            << ",\"gamma\":" << gp.gamma << ",\"offsetSdr\":" << gp.offsetSdr
                            << ",\"offsetHdr\":" << gp.offsetHdr << ",\"hdrExposure\":" << dumpExposure
                            << ",\"hdrTap\":\"" << dumpTap << "\""
                            << ",\"satProtect\":" << gp.satProtect
                            << ",\"multiChannel\":" << (gp.multiChannelMap ? "true" : "false")
                            << ",\"glowStrength\":" << gp.glowStrength
                            << ",\"glowMax\":" << gp.glowMax
                            << ",\"hdrCapacityMin\":" << gp.hdrCapacityMin
                            << ",\"hdrCapacityMax\":" << gp.hdrCapacityMax << "}";
                    mside.close();
                    std::cout << "RAWR_OFFLINE_ULTRAHDR file=" << stem << ".gainmap.rgba8 hdr=" << stem
                              << ".hdr.rgba16f map=" << mapW << "x" << mapH << '\n';
                }
                if (useGlow) {
                    const VkDeviceSize glowBytes =
                        static_cast<VkDeviceSize>(config.width) * config.height * 8u;
                    void* glowMapped = nullptr;
                    check(vkMapMemory(context.dev, glowReadback.m, 0, glowBytes, 0, &glowMapped),
                          "map film glow");
                    std::ofstream glowOut(args.outputDir / (stem + ".glow.rgba16f"),
                                          std::ios::binary | std::ios::trunc);
                    if (!glowOut) throw std::runtime_error("cannot write glow dump");
                    glowOut.write(static_cast<const char*>(glowMapped),
                                  static_cast<std::streamsize>(glowBytes));
                    glowOut.close();
                    vkUnmapMemory(context.dev, glowReadback.m);
                    std::ofstream gside(args.outputDir / (stem + ".glow.json"), std::ios::trunc);
                    gside << "{\"width\":" << config.width << ",\"height\":" << config.height
                          << ",\"format\":\"R16G16B16A16_SFLOAT\",\"domain\":\"film_scatter_quotient_post_over_pre\""
                          << ",\"filmEv\":" << variant.filmEv << "}";
                    gside.close();
                }

                if (variant.film) {
                    std::cout << "RAWR_OFFLINE_FILM file=" << stem << ".ppm filmEv=" << variant.filmEv
                              << " printEv=" << variant.printEv << " push=" << variant.pushStops
                              << " submit_to_idle_ms=" << elapsedMs << '\n';
                    manifest << "render=" << stem << ".ppm filmEv=" << variant.filmEv << " printEv=" << variant.printEv
                             << " push=" << variant.pushStops << " submitToIdleMs=" << elapsedMs << "\n";
                } else {
                std::cout << "RAWR_OFFLINE_RENDER file=" << stem << ".ppm recovery="
                          << (recovery ? "on" : "off") << " highlights=" << highlights
                          << " submit_to_idle_ms=" << elapsedMs << '\n';
                manifest << "render=" << stem << ".ppm recovery=" << (recovery ? "on" : "off")
                         << " highlights=" << highlights << " submitToIdleMs=" << elapsedMs << "\n";
                }
                } catch (const std::exception& variantError) {
                    std::cout << "RAWR_OFFLINE_VARIANT_FAIL file=" << stem << ".ppm error=" << variantError.what()
                              << '\n';
                    manifest << "render=" << stem << ".ppm FAILED error=" << variantError.what() << "\n";
                }
                vktest::delBuf(context.dev, readback);
                if (hdrReadbackOwned) vktest::delBuf(context.dev, hdrReadback);
                if (mapReadbackOwned) vktest::delBuf(context.dev, mapReadback);
                if (mapImgOwned) vktest::delImg(context.dev, mapImg);
                if (glowReadbackOwned) vktest::delBuf(context.dev, glowReadback);
                if (glowImgOwned) vktest::delImg(context.dev, glowImg);
                vktest::delImg(context.dev, output);
                if (tonePipeline) vkDestroyPipeline(context.dev,tonePipeline,nullptr);
                if (toneShader) vkDestroyShaderModule(context.dev,toneShader,nullptr);
                if (tonePipelineLayout) vkDestroyPipelineLayout(context.dev,tonePipelineLayout,nullptr);
                if (toneDescriptorPool) vkDestroyDescriptorPool(context.dev,toneDescriptorPool,nullptr);
                if (toneSetLayout) vkDestroyDescriptorSetLayout(context.dev,toneSetLayout,nullptr);
                vktest::delImg(context.dev,toneImage);
                vktest::delImg(context.dev, source);
            }
        }
        manifest << "resolvedDualContrastPercent=" << (demosaic ? demosaic->resolvedContrastPercent() : 0.f) << "\n"
                 << "autoTileSize=" << (demosaic ? demosaic->autoDetectionTileSize() : 0u) << "\n";
        manifest.close();
        std::cout << "RAWR_OFFLINE_PIPELINE_PASS output_dir=" << args.outputDir << '\n';
        }

        vkDeviceWaitIdle(context.dev);
        vktest::delBuf(context.dev, quadFiltered);
        vktest::delBuf(context.dev, quadInput);
        vktest::delImg(context.dev, clipImage);
        vktest::delImg(context.dev, packedImage);
        vkFreeCommandBuffers(context.dev, pool, 1, &command);
        vkDestroyCommandPool(context.dev, pool, nullptr);
        pool = VK_NULL_HANDLE;
        command = VK_NULL_HANDLE;
        vktest::delCtx(context);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "RAWR_OFFLINE_PIPELINE_FAIL " << error.what() << '\n';
        if (context.dev) {
            vkDeviceWaitIdle(context.dev);
            vktest::delBuf(context.dev, quadFiltered);
            vktest::delBuf(context.dev, quadInput);
            vktest::delImg(context.dev, clipImage);
            vktest::delImg(context.dev, packedImage);
            if (command && pool) vkFreeCommandBuffers(context.dev, pool, 1, &command);
            if (pool) vkDestroyCommandPool(context.dev, pool, nullptr);
        }
        vktest::delCtx(context);
        return 1;
    }
}
