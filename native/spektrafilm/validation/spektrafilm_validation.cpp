// Validation for the spektrafilm adapter:
// 1. determinism (same input twice -> identical bytes), alpha, non-constant;
// 2. parity vs the vendored reference renderer on identical half-quantized
//    input (tolerance +-1 LSB8; the math should be bit-identical).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "../../tonemap_engine/include/tonemap/AdaptiveTone.h"
#include "../src/DirCouplers.h"
#include "../src/SpektraTables.h"
#include "../tools/vk_bench_common.hpp"
#include "SpektraVulkanRenderer.h"
#include "spektrafilm/SpektraFilm.h"
#include "spektrafilm/SpektraProfileCurves.h"

namespace {

float halfToFloat(uint16_t h) {
    const uint32_t s = (uint32_t)(h & 0x8000u) << 16;
    const uint32_t e = (h >> 10) & 31u;
    const uint32_t m = h & 1023u;
    uint32_t f;
    if (e == 0) {
        if (m == 0) {
            f = s;
        } else {
            int ee = -14;
            uint32_t mm = m;
            while ((mm & 1024u) == 0) {
                mm <<= 1;
                --ee;
            }
            mm &= 1023u;
            f = s | ((uint32_t)(ee + 127) << 23) | (mm << 13);
        }
    } else if (e == 31) {
        f = s | 0x7f800000u | (m << 13);
    } else {
        f = s | (((e - 15 + 127) << 23) | (m << 13));
    }
    float out;
    std::memcpy(&out, &f, 4);
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    const char* spirvDir = nullptr;
    const char* hanatosPath = nullptr;
    const char* gamutPath = nullptr;
    const char* resourceDir = nullptr;  // reference renderer assets root
    const char* onlyCase = nullptr;
    uint32_t width = 256, height = 192;
    bool useVvl = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--spirv-dir") == 0 && i + 1 < argc) {
            spirvDir = argv[++i];
        } else if (std::strcmp(argv[i], "--hanatos") == 0 && i + 1 < argc) {
            hanatosPath = argv[++i];
        } else if (std::strcmp(argv[i], "--gamut") == 0 && i + 1 < argc) {
            gamutPath = argv[++i];
        } else if (std::strcmp(argv[i], "--resource-dir") == 0 && i + 1 < argc) {
            resourceDir = argv[++i];
        } else if (std::strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            width = (uint32_t)std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            height = (uint32_t)std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--vvl") == 0) {
            useVvl = true;
        } else if (std::strcmp(argv[i], "--case") == 0 && i + 1 < argc) {
            onlyCase = argv[++i];
        }
    }
    if (!spirvDir || !hanatosPath || !gamutPath || !resourceDir) {
        std::fprintf(stderr,
                     "usage: spektrafilm_validation --spirv-dir DIR --hanatos "
                     "H.f32 --gamut G.f32 --resource-dir ASSETS [--width N "
                     "--height N --vvl]\n");
        return 2;
    }
    // CPU auto-exposure meter checks (no GPU needed): exact mid-gray EV,
    // degenerate black behavior (upstream returns 0), determinism, and the
    // dark-scene regression (thin ~0.02 negative must meter ≈ +3EV so the
    // print lands instead of crushing to black).
    {
        using spektrafilm_native::tables::BayerMeterInput;
        using spektrafilm_native::tables::autoExposureEvFromBayer;
        const auto makeBayer = [](uint32_t w, uint32_t h, uint16_t v) {
            return std::vector<uint16_t>((size_t)w * h, v);
        };
        BayerMeterInput gray{};
        const std::vector<uint16_t> grayPx =
            makeBayer(8u, 8u, (uint16_t)(64 + 0.5f * (1023 - 64)));
        gray.pixels = grayPx.data();
        gray.width = 8u;
        gray.height = 8u;
        gray.blackRggb[0] = gray.blackRggb[1] = gray.blackRggb[2] =
            gray.blackRggb[3] = 64.0f;
        gray.whiteLevel = 1023.0f;
        const float evCw = autoExposureEvFromBayer(gray, 0);
        const float evMed = autoExposureEvFromBayer(gray, 1);
        const float evWant = -std::log2(0.5 / 0.184);
        std::printf(
            "spektrafilm_validation ok: [aeMeterGray] cw=%.4f med=%.4f want=%.4f\n",
            evCw, evMed, evWant);
        if (std::abs(evCw - evWant) > 0.01f ||
            std::abs(evMed - evWant) > 0.01f) {
            std::fprintf(stderr, "validation: [aeMeterGray] wrong EV\n");
            return 1;
        }
        const std::vector<uint16_t> blackPx = makeBayer(8u, 8u, 64u);
        BayerMeterInput black = gray;
        black.pixels = blackPx.data();
        if (autoExposureEvFromBayer(black, 0) != 0.0f ||
            autoExposureEvFromBayer(black, 1) != 0.0f) {
            std::fprintf(stderr,
                         "validation: [aeMeterBlack] degenerate must be 0\n");
            return 1;
        }
        std::printf("spektrafilm_validation ok: [aeMeterBlack] ev=0\n");
        BayerMeterInput empty{};
        if (autoExposureEvFromBayer(empty, 0) != 0.0f) {
            std::fprintf(stderr, "validation: [aeMeterEmpty] must be 0\n");
            return 1;
        }
        const std::vector<uint16_t> darkPx =
            makeBayer(64u, 48u, (uint16_t)(64 + 0.02f * (1023 - 64)));
        BayerMeterInput dark = gray;
        dark.pixels = darkPx.data();
        dark.width = 64u;
        dark.height = 48u;
        const float evDarkCw = autoExposureEvFromBayer(dark, 0);
        const float evDarkMed = autoExposureEvFromBayer(dark, 1);
        std::printf(
            "spektrafilm_validation ok: [aeMeterDark] cw=%.3f med=%.3f\n",
            evDarkCw, evDarkMed);
        if (evDarkCw < 2.5f || evDarkCw > 4.0f || evDarkMed < 2.5f ||
            evDarkMed > 4.0f) {
            std::fprintf(stderr,
                         "validation: [aeMeterDark] thin negative must meter "
                         "positive EV\n");
            return 1;
        }
        if (autoExposureEvFromBayer(dark, 0) != evDarkCw) {
            std::fprintf(stderr, "validation: [aeMeterDark] nondeterministic\n");
            return 1;
        }
    }
    const std::vector<uint8_t> inputSpv =
        spektra_test::loadFileBytes(std::string(spirvDir) + "/spektra_input.comp.spv");
    const std::vector<uint8_t> exposureSpv =
        spektra_test::loadFileBytes(std::string(spirvDir) + "/SpektraFilmExposure.comp.spv");
    const std::vector<uint8_t> developSpv =
        spektra_test::loadFileBytes(std::string(spirvDir) + "/SpektraCurveDevelop.comp.spv");
    const std::vector<uint8_t> printScanSpv =
        spektra_test::loadFileBytes(std::string(spirvDir) + "/SpektraPrintScan.comp.spv");
    const std::vector<uint8_t> outputSpv =
        spektra_test::loadFileBytes(std::string(spirvDir) + "/spektra_output.comp.spv");
    const std::vector<uint8_t> outputHalfSpv = spektra_test::loadFileBytes(
        std::string(spirvDir) + "/spektra_output_half.comp.spv");
    const std::vector<uint8_t> boostMsSpv = spektra_test::loadFileBytes(
        std::string(spirvDir) + "/spektra_boost_milestone.comp.spv");
    const std::vector<uint8_t> glowRatioSpv = spektra_test::loadFileBytes(
        std::string(spirvDir) + "/spektra_glow_ratio.comp.spv");
    const std::vector<uint8_t> grainSpv =
        spektra_test::loadFileBytes(std::string(spirvDir) + "/SpektraGrain.comp.spv");
    const std::vector<uint8_t> dirSpv =
        spektra_test::loadFileBytes(std::string(spirvDir) + "/SpektraDir.comp.spv");
    const std::vector<uint8_t> halationSpv = spektra_test::loadFileBytes(
        std::string(spirvDir) + "/SpektraHalation.comp.spv");
    const std::vector<uint8_t> diffusionSpv = spektra_test::loadFileBytes(
        std::string(spirvDir) + "/SpektraDiffusion.comp.spv");
    const std::vector<uint8_t> scannerSpv = spektra_test::loadFileBytes(
        std::string(spirvDir) + "/SpektraScannerPost.comp.spv");
    const std::vector<uint8_t> hanatosRaw =
        spektra_test::loadFileBytes(hanatosPath);
    const std::vector<uint8_t> gamutRaw = spektra_test::loadFileBytes(gamutPath);

    spektra_test::Ctx ctx =
        spektra_test::makeCtx("spektrafilm_validation", useVvl);
    VkCommandPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pi.queueFamilyIndex = ctx.queueFamily;
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool pool = VK_NULL_HANDLE;
    spektra_test::check(
        vkCreateCommandPool(ctx.device, &pi, nullptr, &pool), "pool");

    spektrafilm_native::SpektraFilmCreateInfo ci{};
    ci.context.physicalDevice = ctx.physical;
    ci.context.device = ctx.device;
    ci.queue = ctx.queue;
    ci.queueFamilyIndex = ctx.queueFamily;
    ci.maxFramesInFlight = 2;
    ci.maxWidth = width;
    ci.maxHeight = height;
    ci.inputSpirv = reinterpret_cast<const uint32_t*>(inputSpv.data());
    ci.inputSpirvBytes = inputSpv.size();
    ci.exposureSpirv = reinterpret_cast<const uint32_t*>(exposureSpv.data());
    ci.exposureSpirvBytes = exposureSpv.size();
    ci.developSpirv = reinterpret_cast<const uint32_t*>(developSpv.data());
    ci.developSpirvBytes = developSpv.size();
    ci.printScanSpirv = reinterpret_cast<const uint32_t*>(printScanSpv.data());
    ci.printScanSpirvBytes = printScanSpv.size();
    ci.outputSpirv = reinterpret_cast<const uint32_t*>(outputSpv.data());
    ci.outputSpirvBytes = outputSpv.size();
    ci.hdrOutputSpirv = reinterpret_cast<const uint32_t*>(outputHalfSpv.data());
    ci.hdrOutputSpirvBytes = outputHalfSpv.size();
    ci.boostMilestoneSpirv = reinterpret_cast<const uint32_t*>(boostMsSpv.data());
    ci.boostMilestoneSpirvBytes = boostMsSpv.size();
    ci.glowRatioSpirv = reinterpret_cast<const uint32_t*>(glowRatioSpv.data());
    ci.glowRatioSpirvBytes = glowRatioSpv.size();
    ci.grainSpirv = reinterpret_cast<const uint32_t*>(grainSpv.data());
    ci.grainSpirvBytes = grainSpv.size();
    ci.dirSpirv = reinterpret_cast<const uint32_t*>(dirSpv.data());
    ci.dirSpirvBytes = dirSpv.size();
    ci.halationSpirv = reinterpret_cast<const uint32_t*>(halationSpv.data());
    ci.halationSpirvBytes = halationSpv.size();
    ci.diffusionSpirv = reinterpret_cast<const uint32_t*>(diffusionSpv.data());
    ci.diffusionSpirvBytes = diffusionSpv.size();
    ci.scannerSpirv = reinterpret_cast<const uint32_t*>(scannerSpv.data());
    ci.scannerSpirvBytes = scannerSpv.size();
    ci.hanatosSpectra = reinterpret_cast<const float*>(hanatosRaw.data());
    ci.hanatosSpectraFloats = hanatosRaw.size() / sizeof(float);
    ci.gamutCompression = reinterpret_cast<const float*>(gamutRaw.data());
    ci.gamutCompressionFloats = gamutRaw.size() / sizeof(float);

    const char* reason = nullptr;
    if (!spektrafilm_native::SpektraFilm::validateCreateInfo(ci, &reason)) {
        std::fprintf(stderr, "validation: create rejected: %s\n",
                     reason ? reason : "?");
        return 1;
    }

    // Gradient input (linear), alpha 1. Shared across methods.
    std::vector<uint16_t> halves((size_t)width * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            halves[((size_t)y * width + x) * 4 + 0] =
                spektra_test::floatToHalf(0.05f + 0.9f * (float)x / width);
            halves[((size_t)y * width + x) * 4 + 1] =
                spektra_test::floatToHalf(0.05f + 0.9f * (float)y / height);
            halves[((size_t)y * width + x) * 4 + 2] =
                spektra_test::floatToHalf(0.05f + 0.45f * ((float)x / width +
                                                           (float)y / height));
            halves[((size_t)y * width + x) * 4 + 3] =
                spektra_test::floatToHalf(1.0f);
        }
    }
    spektra_test::Image input =
        spektra_test::makeImage(ctx, width, height, VK_FORMAT_R16G16B16A16_SFLOAT);
    spektra_test::uploadRgba16f(ctx, pool, input, width, height, halves);
    spektra_test::Image output =
        spektra_test::makeImage(ctx, width, height, VK_FORMAT_R8G8B8A8_UNORM);
    spektra_test::transitionToGeneral(ctx, pool, output);
    spektra_test::Image outputHalf = spektra_test::makeImage(
        ctx, width, height, VK_FORMAT_R16G16B16A16_SFLOAT);
    spektra_test::transitionToGeneral(ctx, pool, outputHalf);
    // Scratch input for customHalves cases (uploaded per case below).
    spektra_test::Image inputAlt = spektra_test::makeImage(
        ctx, width, height, VK_FORMAT_R16G16B16A16_SFLOAT);
    spektra_test::transitionToGeneral(ctx, pool, inputAlt);

    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    spektra_test::check(vkAllocateCommandBuffers(ctx.device, &cai, &cmd),
                        "alloc cmd");

    // Outlives every record that may call ri.passBoundary.
    struct PassFlush {
        const spektra_test::Ctx* ctx;
        VkCommandBuffer cmd;
    } passFlush{&ctx, cmd};
    void (*const flushPass)(void*) = [](void* user) {
        const auto& f = *static_cast<const PassFlush*>(user);
        spektra_test::check(vkEndCommandBuffer(f.cmd), "pass end");
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &f.cmd;
        spektra_test::check(vkQueueSubmit(f.ctx->queue, 1, &submit, VK_NULL_HANDLE), "pass submit");
        spektra_test::check(vkQueueWaitIdle(f.ctx->queue), "pass wait idle");
        spektra_test::check(vkResetCommandBuffer(f.cmd, 0), "pass reset");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        spektra_test::check(vkBeginCommandBuffer(f.cmd, &begin), "pass begin");
    };
    spektrafilm_native::SpektraFilmRecordInfo ri{};
    ri.commandBuffer = cmd;
    ri.input.view = input.view;
    ri.input.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    ri.input.layout = VK_IMAGE_LAYOUT_GENERAL;
    ri.input.width = width;
    ri.input.height = height;
    ri.output.view = output.view;
    ri.output.format = VK_FORMAT_R8G8B8A8_UNORM;
    ri.output.layout = VK_IMAGE_LAYOUT_GENERAL;
    ri.output.width = width;
    ri.output.height = height;
    ri.frameSlot = 0;

    // Parity reference (switches spectral method per render internally).
    setenv("SPEKTRAFILM_RESOURCE_DIR", resourceDir, 1);
    auto reference = spektrafilm::createNativeRenderer();
    if (!reference || !reference->isAvailable()) {
        std::fprintf(stderr, "validation: reference unavailable: %s\n",
                     reference ? reference->lastError().c_str() : "null");
        return 1;
    }
    std::vector<float> refSource((size_t)width * height * 4);
    for (size_t i = 0; i < (size_t)width * height * 4; ++i) {
        refSource[i] = halfToFloat(halves[i]);
    }
    std::vector<float> refDest((size_t)width * height * 4, 0.0f);

    // Case-driven sweep. Each case sets the adapter look + mirror reference
    // params; engines are cached by baked triple (film, paper, method).
    struct Case {
        std::string label;
        spektrafilm_native::FilmLook look;
        spektrafilm::RenderParams ref;
        double timeSec = 0.0;
        bool needsCamera = false;
        spektrafilm_native::CameraFilters camera{};
        // ProcessNegative paper-table rebuild (filtration/timing bake into
        // paper Hanatos pairs): call updateProcessNegativeTables() on the
        // case engine before record, like the app's debounced path.
        bool needsProcessNeg = false;
        // Baked field outside the (film, paper, method) engine cache key
        // (e.g. output space): build a dedicated engine for the case.
        bool ownEngine = false;
        // Still-style engine: conditional scratch reserves only what this
        // look enables (null bindings dummy-backed, never dispatched).
        bool conditionalEngine = false;
        // Tiled compute dispatch (same buffers, activeRect tiles).
        bool tiled = false;
        uint32_t tileW = 128;
        uint32_t tileH = 96;
        // Half-float output (RGBA16F view, hdrOutputSpirv path).
        bool halfOut = false;
        // Tile-memory engine (tiledMemorySaving arena, working/center path).
        bool tileMemory = false;
        // Custom input image (same w/h halves); nullopt-equivalent when empty.
        // Lets cases render non-gradient inputs (e.g. dark stills).
        std::vector<uint16_t> customHalves{};
        // Brightness gate on the recorded output mean (dark-input regression:
        // metered exposure must land a visible image, not crushed black).
        // 0=off, 1=gate [0.05,0.8], 2=report mean only.
        int expectVisible = 0;
        // Tiled boost two-phase: run recordBoostMilestone + submit/wait/read
        // before the main records, then compare CPU helper vs GPU readback.
        bool boostMilestone = false;
        // Chunked submits: split the tile grid into this many submits
        // (watchdog safety); 1 = single submit. Only meaningful with tiled.
        uint32_t submitChunks = 1u;
        // Negative path: record() must throw (reject loudly, never silently
        // wrong).
        bool expectThrow = false;
        std::array<float, 4> adaptiveTone{0.0f, -4.0f, 0.0f, 1.0f};
    };
    auto baseLook = [] {
        spektrafilm_native::FilmLook look{};
        look.film = 2;
        look.paper = 4;
        look.inputColorSpace = 15;
        look.outputColorSpace = 25;
        look.rgbToRawMethod = 2;
        return look;
    };
    auto baseRef = [] {
        spektrafilm::RenderParams params{};
        params.inputColorSpace = spektrafilm::ColorSpace::LinearRec709;
        params.outputColorSpace = spektrafilm::ColorSpace::Rec709Gamma24;
        params.paper = 4;
        params.rgbToRawMethod = spektrafilm::RgbToRawMethod::Hanatos2026;
        return params;
    };
    const char* methodName[3] = {"h2025", "mallett", "h2026"};
    std::vector<Case> cases;
    for (const int method : {2, 1, 0}) {
        Case c{"method-" + std::string(methodName[method]), baseLook(), baseRef()};
        c.look.rgbToRawMethod = method;
        c.ref.rgbToRawMethod =
            static_cast<spektrafilm::RgbToRawMethod>(method);
        cases.push_back(c);
    }
    // Compare the production adaptive film input against a CPU-warped source
    // through the reference renderer, in both full-frame and tiled dispatches.
    for (bool tiled : {false, true}) {
        Case c{tiled ? "adaptive-tone-tiled" : "adaptive-tone", baseLook(), baseRef()};
        c.adaptiveTone = {2.0f, -5.0f, -1.0f, 2.0f};
        c.look.filmExposureEv = c.ref.filmExposureEv = 1.0f;  // HAL 2x, applied once.
        c.tiled = tiled;
        c.customHalves.resize(halves.size());
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                const float v = std::exp2(-12.0f + 13.0f * float(x) / float(width - 1));
                const size_t p = (size_t(y) * width + x) * 4;
                c.customHalves[p] = spektra_test::floatToHalf(v);
                c.customHalves[p + 1] = spektra_test::floatToHalf(v * (y % 2 ? .8f : 1.0f));
                c.customHalves[p + 2] = spektra_test::floatToHalf(v * (y % 2 ? .6f : 1.0f));
                c.customHalves[p + 3] = spektra_test::floatToHalf(1.0f);
            }
        }
        cases.push_back(c);
    }
    auto addCase = [&](const char* label, auto setLook, auto setRef) {
        Case c{label, baseLook(), baseRef()};
        setLook(c.look);
        setRef(c.ref);
        cases.push_back(c);
    };
    addCase("filmEV+1",
            [](auto& l) { l.filmExposureEv = 1.0f; },
            [](auto& r) { r.filmExposureEv = 1.0f; });
    addCase("printEV+1",
            [](auto& l) { l.printExposureEv = 1.0f; },
            [](auto& r) { r.printExposureEv = 1.0f; });
    addCase("pushStd+1",
            [](auto& l) { l.filmPushPullMode = 0; l.filmPushPullStops = 1.0f; },
            [](auto& r) {
                r.filmPushPullMode = spektrafilm::PushPullMode::Standard;
                r.filmPushPullStops = 1.0f;
            });
    addCase("pushExp+1",
            [](auto& l) { l.filmPushPullMode = 1; l.filmPushPullStops = 1.0f; },
            [](auto& r) {
                r.filmPushPullMode = spektrafilm::PushPullMode::Experimental;
                r.filmPushPullStops = 1.0f;
            });
    addCase("filmGamma1.25",
            [](auto& l) { l.filmGamma = 1.25f; },
            [](auto& r) { r.filmGamma = 1.25f; });
    addCase("printPush+1",
            [](auto& l) { l.printPushPullStops = 1.0f; },
            [](auto& r) { r.printPushPullStops = 1.0f; });
    addCase("printGamma1.25",
            [](auto& l) { l.printGamma = 1.25f; },
            [](auto& r) { r.printGamma = 1.25f; });
    addCase("shadow+0.5",
            [](auto& l) { l.printShadowShape = 0.5f; },
            [](auto& r) { r.printShadowShape = 0.5f; });
    addCase("highlight-0.5",
            [](auto& l) { l.printHighlightShape = -0.5f; },
            [](auto& r) { r.printHighlightShape = -0.5f; });
    addCase("negBleach0.5",
            [](auto& l) { l.negativeBleachBypassAmount = 0.5f; },
            [](auto& r) { r.negativeBleachBypassAmount = 0.5f; });
    addCase("leuco0.5",
            [](auto& l) { l.negativeLeucoCyanCoupling = 0.5f; },
            [](auto& r) { r.negativeLeucoCyanCoupling = 0.5f; });
    addCase("printBleach0.5",
            [](auto& l) { l.printBleachBypassAmount = 0.5f; },
            [](auto& r) { r.printBleachBypassAmount = 0.5f; });
    addCase("preflash0.3",
            [](auto& l) { l.preflashExposure = 0.3f; },
            [](auto& r) { r.preflashExposure = 0.3f; });
    addCase("preflashColor",
            [](auto& l) {
                l.preflashExposure = 0.3f;
                l.preflashMFilterShift = 10.0f;
                l.preflashYFilterShift = -10.0f;
            },
            [](auto& r) {
                r.preflashExposure = 0.3f;
                r.preflashMFilterShift = 10.0f;
                r.preflashYFilterShift = -10.0f;
            });
    addCase("printerR+2",
            [](auto& l) { l.printerLightsR = 2.0f; },
            [](auto& r) { r.printerLightsR = 2.0f; });
    addCase("printerGang",
            [](auto& l) { l.printerLightsGang = true; l.printerLightsG = 1.0f; },
            [](auto& r) {
                r.printerLightsGang = true;
                r.printerLightsG = 1.0f;
            });
    addCase("enlarger2x",
            [](auto& l) { l.enlargerScale = 2.0f; },
            [](auto& r) { r.enlargerScale = 2.0f; });
    addCase("enlargerOffset",
            [](auto& l) {
                l.enlargerOffsetXPercent = 10.0f;
                l.enlargerOffsetYPercent = -10.0f;
            },
            [](auto& r) {
                r.enlargerOffsetXPercent = 10.0f;
                r.enlargerOffsetYPercent = -10.0f;
            });
    addCase("stock-ektachrome",
            [](auto& l) { l.film = 16; l.paper = 1; },
            [](auto& r) { r.film = 16; r.paper = 1; });
    // ScanNegative mode: developed-film presentation (correct for positive/
    // reversal stocks, which print negative). No baked state involved.
    addCase("velvia-scan",
            [](auto& l) { l.film = 18; l.process = 1; },
            [](auto& r) {
                r.film = 18;
                r.process = spektrafilm::ProcessMode::ScanNegative;
            });
    addCase("velvia-scan-print-controls",
            [](auto& l) {
                l.film = 18;
                l.process = 1;
                l.paper = 5;
                l.printExposureEv = 2.0f;
                l.printDiffusionEnabled = true;
            },
            [](auto& r) {
                r.film = 18;
                r.process = spektrafilm::ProcessMode::ScanNegative;
                r.paper = 5;
                r.printExposureEv = 2.0f;
                r.printDiffusionEnabled = true;
            });
    // Scan is the display path for every reversal stock. High DIR differs
    // intentionally from upstream: normalize its strength before asking the
    // reference renderer for the expected image.
    for (const int stock : {16, 17, 18, 19}) {
        for (const float amount : {0.5f, 1.0f}) {
            Case c{"positive-scan-dir-" + std::to_string(stock) + "-" +
                       std::to_string(amount), baseLook(), baseRef()};
            c.look.film = stock;
            c.look.process = 1;
            c.look.dirCouplersAmount = amount;
            c.ref.film = stock;
            c.ref.process = spektrafilm::ProcessMode::ScanNegative;
            const auto* profile = spektrafilm::filmProfileCurves(stock);
            if (!profile) return 1;
            const float effective =
                spektrafilm_native::dir::effectiveAmount(*profile, c.look);
            if (!(effective > 0.0f && effective <= amount)) {
                std::fprintf(stderr, "validation: invalid positive DIR strength\n");
                return 1;
            }
            c.ref.dirCouplersAmount = effective;
            cases.push_back(c);
        }
    }
    // Positive stock through the print path: photochemically a negative.
    // Parity-guards the adapter; the app auto-routes positive stocks to scan.
    addCase("velvia-print-expect-negative",
            [](auto& l) { l.film = 18; l.process = 0; },
            [](auto& r) {
                r.film = 18;
                r.process = spektrafilm::ProcessMode::PrintSimulation;
            });
    addCase("velvia-scan-inv",
            [](auto& l) {
                l.film = 18;
                l.process = 1;
                l.scanNegativeInvert = true;
            },
            [](auto& r) {
                r.film = 18;
                r.process = spektrafilm::ProcessMode::ScanNegative;
                r.scanNegativeInvert = true;
            });
    addCase("portra-scan-inv",
            [](auto& l) { l.process = 1; l.scanNegativeInvert = true; },
            [](auto& r) {
                r.process = spektrafilm::ProcessMode::ScanNegative;
                r.scanNegativeInvert = true;
            });
    addCase("velvia-scan-grain",
            [](auto& l) {
                l.film = 18;
                l.process = 1;
                l.grainEnabled = true;
            },
            [](auto& r) {
                r.film = 18;
                r.process = spektrafilm::ProcessMode::ScanNegative;
                r.grainEnabled = true;
            });
    // DIR couplers: correction -> (blur) -> (tail) -> redevelop over the
    // developed densities. Amount 0 disables (default); the full-defaults
    // case exercises all 9 dispatches.
    addCase("dirFull",
            [](auto& l) { l.dirCouplersAmount = 0.5f; },
            [](auto& r) { r.dirCouplersAmount = 0.5f; });
    addCase("dirNoBlur",
            [](auto& l) {
                l.dirCouplersAmount = 0.5f;
                l.dirCouplersDiffusionUm = 0.0f;
            },
            [](auto& r) {
                r.dirCouplersAmount = 0.5f;
                r.dirCouplersDiffusionUm = 0.0f;
            });
    addCase("dirNoTail",
            [](auto& l) {
                l.dirCouplersAmount = 0.5f;
                l.dirCouplersDiffusionTailWeight = 0.0f;
            },
            [](auto& r) {
                r.dirCouplersAmount = 0.5f;
                r.dirCouplersDiffusionTailWeight = 0.0f;
            });
    addCase("dirGamma",
            [](auto& l) {
                l.dirCouplersAmount = 0.7f;
                l.dirCouplersInhibitionInterlayer = 0.5f;
                l.dirCouplersGammaSameLayerR = 0.5f;
                l.dirCouplersGammaBToG = 0.1f;
            },
            [](auto& r) {
                r.dirCouplersAmount = 0.7f;
                r.dirCouplersInhibitionInterlayer = 0.5f;
                r.dirCouplersGammaSameLayerR = 0.5f;
                r.dirCouplersGammaBToG = 0.1f;
            });
    // Halation: scatter + bounce on the linear raw before develop; boost
    // lifts highlights first. Upstream defaults run scatter+bounce, so
    // isolate each sub-path first. Stock presets drive strengths/sigmas
    // unless overridden (halCoupling).
    addCase("halScatterOnly",
            [](auto& l) {
                l.halationEnabled = true;
                l.halationAmount = 0.0f;
            },
            [](auto& r) {
                r.halationEnabled = true;
                r.halationAmount = 0.0f;
            });
    addCase("halBounceOnly",
            [](auto& l) {
                l.halationEnabled = true;
                l.scatterAmount = 0.0f;
            },
            [](auto& r) {
                r.halationEnabled = true;
                r.scatterAmount = 0.0f;
            });
    addCase("halBoostOnly",
            [](auto& l) {
                l.halationEnabled = true;
                l.scatterAmount = 0.0f;
                l.halationAmount = 0.0f;
                l.halationBoostEv = 1.0f;
            },
            [](auto& r) {
                r.halationEnabled = true;
                r.scatterAmount = 0.0f;
                r.halationAmount = 0.0f;
                r.halationBoostEv = 1.0f;
            });
    addCase("halFull",
            [](auto& l) { l.halationEnabled = true; },
            [](auto& r) { r.halationEnabled = true; });
    addCase("halCombo",
            [](auto& l) {
                l.halationEnabled = true;
                l.halationBoostEv = 1.0f;
            },
            [](auto& r) {
                r.halationEnabled = true;
                r.halationBoostEv = 1.0f;
            });
    addCase("halCoupling",
            [](auto& l) {
                l.halationEnabled = true;
                l.halationStrengthR = 0.1f;
                l.halationStrengthG = 0.05f;
                l.halationStrengthB = 0.01f;
                l.halationFirstSigmaUmR = 80.0f;
                l.halationFirstSigmaUmG = 80.0f;
                l.halationFirstSigmaUmB = 80.0f;
            },
            [](auto& r) {
                r.halationEnabled = true;
                r.halationStrengthR = 0.1f;
                r.halationStrengthG = 0.05f;
                r.halationStrengthB = 0.01f;
                r.halationFirstSigmaUmR = 80.0f;
                r.halationFirstSigmaUmG = 80.0f;
                r.halationFirstSigmaUmB = 80.0f;
            });
    addCase("halVelviaScan",
            [](auto& l) {
                l.film = 18;
                l.process = 1;
                l.halationEnabled = true;
            },
            [](auto& r) {
                r.film = 18;
                r.process = spektrafilm::ProcessMode::ScanNegative;
                r.halationEnabled = true;
            });
    // Cross-path: DIR redevelop + grain read post-develop densities, but
    // DIR set6 binding 0 must track the develop input (log raw vs film
    // raw) when a raw consumer (halation/diffusion) flips exposure mode.
    addCase("dirHal",
            [](auto& l) {
                l.dirCouplersAmount = 0.5f;
                l.halationEnabled = true;
            },
            [](auto& r) {
                r.dirCouplersAmount = 0.5f;
                r.halationEnabled = true;
            });
    addCase("dirDiffusion",
            [](auto& l) {
                l.dirCouplersAmount = 0.5f;
                l.cameraDiffusionEnabled = true;
            },
            [](auto& r) {
                r.dirCouplersAmount = 0.5f;
                r.cameraDiffusionEnabled = true;
            });
    addCase("dirHalGrain",
            [](auto& l) {
                l.dirCouplersAmount = 0.8f;
                l.halationEnabled = true;
                l.grainEnabled = true;
            },
            [](auto& r) {
                r.dirCouplersAmount = 0.8f;
                r.halationEnabled = true;
                r.grainEnabled = true;
            });
    // Conditional scratch (still engines): same looks as above, but the
    // engine reserves only enabled effects. Parity must be identical —
    // absent scratch dummy-binds and never dispatches.
    auto condBase = [&](const char* label) {
        Case c{label, baseLook(), baseRef()};
        c.conditionalEngine = true;
        return c;
    };
    {
        Case c = condBase("condBare");
        cases.push_back(c);
    }
    {
        Case c = condBase("condHal");
        c.look.halationEnabled = true;
        c.ref.halationEnabled = true;
        cases.push_back(c);
    }
    {
        Case c = condBase("condDiffusion");
        c.look.cameraDiffusionEnabled = true;
        c.ref.cameraDiffusionEnabled = true;
        cases.push_back(c);
    }
    {
        Case c = condBase("condDirHalGrain");
        c.look.dirCouplersAmount = 0.8f;
        c.look.halationEnabled = true;
        c.look.grainEnabled = true;
        c.ref.dirCouplersAmount = 0.8f;
        c.ref.halationEnabled = true;
        c.ref.grainEnabled = true;
        cases.push_back(c);
    }
    {
        Case c = condBase("condProdGrain");
        c.look.grainEnabled = true;
        c.look.grainModel = 1;
        c.ref.grainEnabled = true;
        c.ref.grainModel = spektrafilm::GrainModel::Production;
        cases.push_back(c);
    }
    {
        Case c = condBase("condScanner");
        c.look.scannerEnabled = true;
        c.ref.scannerEnabled = true;
        cases.push_back(c);
    }
    // Camera diffusion: solver-driven Gaussian mixture on the linear raw.
    // Defaults (BlackProMist 0.5) exercise grouped + downsampled blurs.
    addCase("diffCamera",
            [](auto& l) { l.cameraDiffusionEnabled = true; },
            [](auto& r) { r.cameraDiffusionEnabled = true; });
    addCase("diffGlimmer",
            [](auto& l) {
                l.cameraDiffusionEnabled = true;
                l.cameraDiffusionFamily = 0;
                l.cameraDiffusionStrength = 1.0f;
            },
            [](auto& r) {
                r.cameraDiffusionEnabled = true;
                r.cameraDiffusionFamily =
                    spektrafilm::DiffusionFilterFamily::Glimmerglass;
                r.cameraDiffusionStrength = 1.0f;
            });
    addCase("diffWarmBloom",
            [](auto& l) {
                l.cameraDiffusionEnabled = true;
                l.cameraDiffusionHaloWarmth = 0.8f;
                l.cameraDiffusionBloomIntensity = 2.0f;
                l.cameraDiffusionCoreSize = 0.5f;
            },
            [](auto& r) {
                r.cameraDiffusionEnabled = true;
                r.cameraDiffusionHaloWarmth = 0.8f;
                r.cameraDiffusionBloomIntensity = 2.0f;
                r.cameraDiffusionCoreSize = 0.5f;
            });
    addCase("diffWithHalation",
            [](auto& l) {
                l.cameraDiffusionEnabled = true;
                l.halationEnabled = true;
            },
            [](auto& r) {
                r.cameraDiffusionEnabled = true;
                r.halationEnabled = true;
            });
    addCase("diffVelviaScan",
            [](auto& l) {
                l.film = 18;
                l.process = 1;
                l.cameraDiffusionEnabled = true;
            },
            [](auto& r) {
                r.film = 18;
                r.process = spektrafilm::ProcessMode::ScanNegative;
                r.cameraDiffusionEnabled = true;
            });
    // Print diffusion: same solver in the enlarger path (PrintRaw ->
    // sequence -> FinalFromPrintRaw). Print workflow only; the scan case
    // locks the process gate (no-op must match exactly).
    addCase("printDiff",
            [](auto& l) { l.printDiffusionEnabled = true; },
            [](auto& r) { r.printDiffusionEnabled = true; });
    addCase("printDiffCine",
            [](auto& l) {
                l.printDiffusionEnabled = true;
                l.printDiffusionFamily = 3;
                l.printDiffusionStrength = 1.0f;
                l.printDiffusionBloomSize = 0.5f;
            },
            [](auto& r) {
                r.printDiffusionEnabled = true;
                r.printDiffusionFamily =
                    spektrafilm::DiffusionFilterFamily::CineBloom;
                r.printDiffusionStrength = 1.0f;
                r.printDiffusionBloomSize = 0.5f;
            });
    addCase("printDiffScanner",
            [](auto& l) {
                l.printDiffusionEnabled = true;
                l.scannerEnabled = true;
            },
            [](auto& r) {
                r.printDiffusionEnabled = true;
                r.scannerEnabled = true;
            });
    addCase("printDiffCamera",
            [](auto& l) {
                l.printDiffusionEnabled = true;
                l.cameraDiffusionEnabled = true;
            },
            [](auto& r) {
                r.printDiffusionEnabled = true;
                r.cameraDiffusionEnabled = true;
            });
    addCase("printDiffOnScan",
            [](auto& l) {
                l.film = 18;
                l.process = 1;
                l.printDiffusionEnabled = true;
            },
            [](auto& r) {
                r.film = 18;
                r.process = spektrafilm::ProcessMode::ScanNegative;
                r.printDiffusionEnabled = true;
            });
    // Scanner post: glare (print finals), MTF blur + unsharp (all finals).
    // Upstream defaults enable all three sub-paths, so isolate each first.
    addCase("scanUnsharpOnly",
            [](auto& l) {
                l.scannerEnabled = true;
                l.glarePercent = 0.0f;
                l.scannerMtf50LpMm = 0.0f;
            },
            [](auto& r) {
                r.scannerEnabled = true;
                r.glarePercent = 0.0f;
                r.scannerMtf50LpMm = 0.0f;
            });
    addCase("scanBlurOnly",
            [](auto& l) {
                l.scannerEnabled = true;
                l.glarePercent = 0.0f;
                l.scannerUnsharpAmount = 0.0f;
            },
            [](auto& r) {
                r.scannerEnabled = true;
                r.glarePercent = 0.0f;
                r.scannerUnsharpAmount = 0.0f;
            });
    addCase("scanGlareOnly",
            [](auto& l) {
                l.scannerEnabled = true;
                l.scannerMtf50LpMm = 0.0f;
                l.scannerUnsharpAmount = 0.0f;
            },
            [](auto& r) {
                r.scannerEnabled = true;
                r.scannerMtf50LpMm = 0.0f;
                r.scannerUnsharpAmount = 0.0f;
            });
    addCase("scanCombo",
            [](auto& l) { l.scannerEnabled = true; },
            [](auto& r) { r.scannerEnabled = true; });
    addCase("scanWhiteBlack",
            [](auto& l) {
                l.scannerEnabled = true;
                l.scannerWhiteCorrection = true;
                l.scannerBlackCorrection = true;
                l.glarePercent = 0.0f;
                l.scannerMtf50LpMm = 0.0f;
                l.scannerUnsharpAmount = 0.0f;
            },
            [](auto& r) {
                r.scannerEnabled = true;
                r.scannerWhiteCorrection = true;
                r.scannerBlackCorrection = true;
                r.glarePercent = 0.0f;
                r.scannerMtf50LpMm = 0.0f;
                r.scannerUnsharpAmount = 0.0f;
            });
    addCase("scanVelviaUnsharp",
            [](auto& l) {
                l.film = 18;
                l.process = 1;
                l.scannerEnabled = true;
                l.glarePercent = 0.0f;
                l.scannerMtf50LpMm = 0.0f;
            },
            [](auto& r) {
                r.film = 18;
                r.process = spektrafilm::ProcessMode::ScanNegative;
                r.scannerEnabled = true;
                r.glarePercent = 0.0f;
                r.scannerMtf50LpMm = 0.0f;
            });
    // Grain cases: preview model first, then production (Synthesis lands later).
    auto grainBase = [&](const char* label) {        Case c{label, baseLook(), baseRef()};
        c.look.grainEnabled = true;
        c.ref.grainEnabled = true;
        return c;
    };
    {
        Case c = grainBase("grain1.0");
        cases.push_back(c);
    }
    {
        Case c = grainBase("grain2.0");
        c.look.grainAmount = 2.0f;
        c.ref.grainAmount = 2.0f;
        cases.push_back(c);
    }
    {
        Case c = grainBase("grainSeed37");
        c.look.grainSeed = 37u;
        c.ref.grainSeed = 37u;
        cases.push_back(c);
    }
    {
        Case c = grainBase("grainSat0");
        c.look.grainSaturation = 0.0f;
        c.ref.grainSaturation = 0.0f;
        cases.push_back(c);
    }
    {
        Case c = grainBase("grainAnimateT2");
        c.look.grainAnimate = true;
        c.ref.grainAnimate = true;
        c.timeSec = 2.0;
        cases.push_back(c);
    }
    {
        Case c = grainBase("grainImax");
        c.look.filmFormat = 7;
        c.ref.filmFormat = spektrafilm::FilmFormat::Imax70;
        cases.push_back(c);
    }
    {
        Case c = grainBase("grainParticle");
        c.look.grainParticleAreaUm2 = 0.2f;
        c.look.grainParticleScaleR = 2.0f;
        c.look.grainDensityMinB = 0.1f;
        c.look.grainUniformityG = 0.9f;
        c.ref.grainParticleAreaUm2 = 0.2f;
        c.ref.grainParticleScaleR = 2.0f;
        c.ref.grainDensityMinB = 0.1f;
        c.ref.grainUniformityG = 0.9f;
        cases.push_back(c);
    }
    {
        Case c = grainBase("grainSubOff");
        c.look.grainSublayersEnabled = false;
        c.look.grainSubLayerCount = 4;
        c.ref.grainSublayersEnabled = false;
        c.ref.grainSubLayerCount = 4;
        cases.push_back(c);
    }
    // Production grain: 10-dispatch dye-cloud path (layer tables +
    // microstructure + blurs). Same params as preview, model selects path.
    auto prodGrainBase = [&](const char* label) {
        Case c{label, baseLook(), baseRef()};
        c.look.grainEnabled = true;
        c.look.grainModel = 1;
        c.ref.grainEnabled = true;
        c.ref.grainModel = spektrafilm::GrainModel::Production;
        return c;
    };
    {
        Case c = prodGrainBase("prodGrain1.0");
        cases.push_back(c);
    }
    {
        Case c = prodGrainBase("prodGrain2.0");
        c.look.grainAmount = 2.0f;
        c.ref.grainAmount = 2.0f;
        cases.push_back(c);
    }
    {
        Case c = prodGrainBase("prodGrainLayers");
        c.look.grainParticleScaleLayer0 = 3.0f;
        c.look.grainBlurDyeCloudsUm = 2.0f;
        c.look.grainFinalBlurUm = 5.0f;
        c.ref.grainParticleScaleLayer0 = 3.0f;
        c.ref.grainBlurDyeCloudsUm = 2.0f;
        c.ref.grainFinalBlurUm = 5.0f;
        cases.push_back(c);
    }
    {
        Case c = prodGrainBase("prodGrainDir");
        c.look.dirCouplersAmount = 0.5f;
        c.ref.dirCouplersAmount = 0.5f;
        cases.push_back(c);
    }
    {
        Case c = prodGrainBase("prodGrainVelviaScan");
        c.look.film = 18;
        c.look.process = 1;
        c.ref.film = 18;
        c.ref.process = spektrafilm::ProcessMode::ScanNegative;
        cases.push_back(c);
    }
    // Filtration is live per-frame on this path (frame constants only).
    addCase("filterC+10",
            [](auto& l) { l.filterC = 10.0f; },
            [](auto& r) { r.filterC = 10.0f; });
    addCase("preflash0.5+color",
            [](auto& l) {
                l.preflashExposure = 0.5f;
                l.preflashMFilterShift = 8.0f;
                l.preflashYFilterShift = -8.0f;
            },
            [](auto& r) {
                r.preflashExposure = 0.5f;
                r.preflashMFilterShift = 8.0f;
                r.preflashYFilterShift = -8.0f;
            });
    {
        Case c{"uv400", baseLook(), baseRef()};
        c.look.cameraUvFilterEnabled = true;
        c.look.cameraUvCutNm = 400.0f;
        c.ref.cameraUvFilterEnabled = true;
        c.ref.cameraUvCutNm = 400.0f;
        c.needsCamera = true;
        c.camera.uvEnabled = true;
        c.camera.uvCutNm = 400.0f;
        cases.push_back(c);
    }
    {
        Case c{"ir680", baseLook(), baseRef()};
        c.look.cameraIrFilterEnabled = true;
        c.look.cameraIrCutNm = 680.0f;
        c.ref.cameraIrFilterEnabled = true;
        c.ref.cameraIrCutNm = 680.0f;
        c.needsCamera = true;
        c.camera.irEnabled = true;
        c.camera.irCutNm = 680.0f;
        cases.push_back(c);
    }

    // sRGB output: the app default for stills (upstream defaults to
    // Rec709Gamma24). Output space is baked, outside the shared engine cache
    // key, so this case gets a dedicated engine.
    {
        Case c{"srgb-output", baseLook(), baseRef()};
        c.look.outputColorSpace = 17;
        c.ref.outputColorSpace = spektrafilm::ColorSpace::Srgb;
        c.ownEngine = true;
        cases.push_back(c);
    }
    // Grain synthesis: 10-dispatch sampling path (ops 11,2,3,4,5,6,12,8,9,13).
    // Harness vector mirrors SpektraVulkanCopyHarness grainSynthesisPass.
    {
        Case c{"synthBase", baseLook(), baseRef()};
        c.look.grainEnabled = true;
        c.look.grainModel = 2;
        c.look.grainSeed = 37u;
        c.look.grainSublayersEnabled = true;
        c.look.grainBlurDyeCloudsUm = 1.0f;
        c.look.grainSynthesisQuality = 0.75f;
        c.look.grainSynthesisSamples = 64;
        c.look.grainSynthesisMeanRadiusUm = 0.22f;
        c.look.grainSynthesisObservationSigmaUm = 0.65f;
        c.look.grainSynthesisMaxGrainsPerCell = 24;
        c.ref.grainEnabled = true;
        c.ref.grainModel = spektrafilm::GrainModel::GrainSynthesis;
        c.ref.grainSeed = 37u;
        c.ref.grainSublayersEnabled = true;
        c.ref.grainBlurDyeCloudsUm = 1.0f;
        c.ref.grainSynthesisQuality = 0.75f;
        c.ref.grainSynthesisSamples = 64;
        c.ref.grainSynthesisMeanRadiusUm = 0.22f;
        c.ref.grainSynthesisObservationSigmaUm = 0.65f;
        c.ref.grainSynthesisMaxGrainsPerCell = 24;
        cases.push_back(c);
    }
    {
        Case c{"synthLayeredOff", baseLook(), baseRef()};
        c.look.grainEnabled = true;
        c.look.grainModel = 2;
        c.look.grainSeed = 37u;
        c.look.grainSynthesisLayered = false;
        c.look.grainSynthesisRadiusStdDevRatio = 0.5f;
        c.look.grainSynthesisQuality = 0.75f;
        c.look.grainSynthesisSamples = 64;
        c.look.grainSynthesisMeanRadiusUm = 0.22f;
        c.look.grainSynthesisObservationSigmaUm = 0.65f;
        c.look.grainSynthesisMaxGrainsPerCell = 24;
        c.ref.grainEnabled = true;
        c.ref.grainModel = spektrafilm::GrainModel::GrainSynthesis;
        c.ref.grainSeed = 37u;
        c.ref.grainSynthesisLayered = false;
        c.ref.grainSynthesisRadiusStdDevRatio = 0.5f;
        c.ref.grainSynthesisQuality = 0.75f;
        c.ref.grainSynthesisSamples = 64;
        c.ref.grainSynthesisMeanRadiusUm = 0.22f;
        c.ref.grainSynthesisObservationSigmaUm = 0.65f;
        c.ref.grainSynthesisMaxGrainsPerCell = 24;
        cases.push_back(c);
    }
    // ProcessNegative: direct negative development (skips film-path effects).
    {
        Case c{"processNeg", baseLook(), baseRef()};
        c.look.process = 2;
        c.ref.process = spektrafilm::ProcessMode::ProcessNegative;
        c.needsProcessNeg = true;
        cases.push_back(c);
    }
    {
        Case c{"processNegFilter", baseLook(), baseRef()};
        c.look.process = 2;
        c.look.filterC = 10.0f;
        c.look.filterMShift = 5.0f;
        c.look.preflashExposure = 0.3f;
        c.look.preflashMFilterShift = 8.0f;
        c.ref.process = spektrafilm::ProcessMode::ProcessNegative;
        c.ref.filterC = 10.0f;
        c.ref.filterMShift = 5.0f;
        c.ref.preflashExposure = 0.3f;
        c.ref.preflashMFilterShift = 8.0f;
        c.needsProcessNeg = true;
        cases.push_back(c);
    }
    {
        Case c{"processNegLights", baseLook(), baseRef()};
        c.look.process = 2;
        c.look.printerLightsR = 2.0f;
        c.look.printerLightsGang = true;
        c.ref.process = spektrafilm::ProcessMode::ProcessNegative;
        c.ref.printerLightsR = 2.0f;
        c.ref.printerLightsGang = true;
        c.needsProcessNeg = true;
        cases.push_back(c);
    }
    // Color adaptation: SDR gamut compress + curve smoothing + input compress.
    addCase("adaptSdr",
            [](auto& l) { l.colorAdaptation = true; },
            [](auto& r) { r.colorAdaptation = true; });
    addCase("adaptOffPartial",
            [](auto& l) {
                l.colorAdaptation = true;
                l.colorAdaptationOutputLightnessCompression = false;
                l.colorAdaptationOutputChromaCompression = false;
            },
            [](auto& r) {
                r.colorAdaptation = true;
                r.colorAdaptationOutputLightnessCompression = false;
                r.colorAdaptationOutputChromaCompression = false;
            });
    // HDR roles (PQ/HLG signal 0..1 packed to RGBA8; same LSB8 harness).
    addCase("hdrPqSoft",
            [](auto& l) {
                l.outputRole = 1;
                l.hdrTransfer = 0;
                l.hdrToneMapping = 0;
            },
            [](auto& r) {
                r.outputRole = spektrafilm::OutputRole::DisplayHdr;
                r.hdrTransfer = spektrafilm::HdrTransfer::Pq;
                r.hdrToneMapping = spektrafilm::HdrToneMapping::SoftRolloff;
            });
    addCase("hdrHlgHard",
            [](auto& l) {
                l.outputRole = 1;
                l.hdrTransfer = 1;
                l.hdrToneMapping = 1;
                l.hdrPeakNits = 1000.0f;
            },
            [](auto& r) {
                r.outputRole = spektrafilm::OutputRole::DisplayHdr;
                r.hdrTransfer = spektrafilm::HdrTransfer::Hlg;
                r.hdrToneMapping = spektrafilm::HdrToneMapping::HardClip;
                r.hdrPeakNits = 1000.0f;
            });
    addCase("rcmLinear",
            [](auto& l) { l.outputRole = 2; },
            [](auto& r) {
                r.outputRole = spektrafilm::OutputRole::Rcm;
            });
    // Half-float output: PQ signal and RCM linear keep full precision.
    {
        Case c{"hdrPqHalf", baseLook(), baseRef()};
        c.look.outputRole = 1;
        c.look.hdrTransfer = 0;
        c.look.hdrToneMapping = 0;
        c.ref.outputRole = spektrafilm::OutputRole::DisplayHdr;
        c.ref.hdrTransfer = spektrafilm::HdrTransfer::Pq;
        c.ref.hdrToneMapping = spektrafilm::HdrToneMapping::SoftRolloff;
        c.halfOut = true;
        cases.push_back(c);
    }
    {
        Case c{"rcmHalf", baseLook(), baseRef()};
        c.look.outputRole = 2;
        c.ref.outputRole = spektrafilm::OutputRole::Rcm;
        c.halfOut = true;
        cases.push_back(c);
    }
    // Tile-memory parity: working-size arena + center accumulation must
    // match full-frame (and the reference) exactly.
    {
        Case c{"memBare", baseLook(), baseRef()};
        c.tiled = true;
        c.tileMemory = true;
        cases.push_back(c);
    }
    // Tile-memory per-effect coverage (each effect alone through the
    // working/center path).
    auto memSolo = [&](const char* label, auto setLook, auto setRef) {
        Case c{label, baseLook(), baseRef()};
        setLook(c.look);
        setRef(c.ref);
        c.tiled = true;
        c.tileW = 128;
        c.tileH = 96;
        c.tileMemory = true;
        cases.push_back(c);
    };
    memSolo("memHalOnly",
            [](auto& l) { l.halationEnabled = true; },
            [](auto& r) { r.halationEnabled = true; });
    memSolo("memDiffOnly",
            [](auto& l) { l.cameraDiffusionEnabled = true; },
            [](auto& r) { r.cameraDiffusionEnabled = true; });
    memSolo("memDirOnly",
            [](auto& l) { l.dirCouplersAmount = 0.5f; },
            [](auto& r) { r.dirCouplersAmount = 0.5f; });
    memSolo("memProdOnly",
            [](auto& l) {
                l.grainEnabled = true;
                l.grainModel = 1;
            },
            [](auto& r) {
                r.grainEnabled = true;
                r.grainModel = spektrafilm::GrainModel::Production;
            });
    memSolo("memScanOnly",
            [](auto& l) { l.scannerEnabled = true; },
            [](auto& r) { r.scannerEnabled = true; });
    {
        Case c{"memHalDiffDir", baseLook(), baseRef()};
        c.look.halationEnabled = true;
        c.look.cameraDiffusionEnabled = true;
        c.look.dirCouplersAmount = 0.5f;
        c.look.grainEnabled = true;
        c.look.grainModel = 1;
        c.look.scannerEnabled = true;
        c.ref.halationEnabled = true;
        c.ref.cameraDiffusionEnabled = true;
        c.ref.dirCouplersAmount = 0.5f;
        c.ref.grainEnabled = true;
        c.ref.grainModel = spektrafilm::GrainModel::Production;
        c.ref.scannerEnabled = true;
        c.tiled = true;
        c.tileW = 128;
        c.tileH = 96;
        c.tileMemory = true;
        cases.push_back(c);
    }
    {
        Case c{"memProcessNeg", baseLook(), baseRef()};
        c.look.process = 2;
        c.look.printDiffusionEnabled = true;
        c.ref.process = spektrafilm::ProcessMode::ProcessNegative;
        c.ref.printDiffusionEnabled = true;
        c.needsProcessNeg = true;
        c.tiled = true;
        c.tileW = 128;
        c.tileH = 96;
        c.tileMemory = true;
        cases.push_back(c);
    }
    // Tiled boost two-phase: milestone submit+wait+read, then main records.
    auto memBoost = [&](const char* label, auto setLook, auto setRef) {
        Case c{label, baseLook(), baseRef()};
        setLook(c.look);
        setRef(c.ref);
        c.look.halationEnabled = true;
        c.look.halationBoostEv = 1.0f;
        c.ref.halationEnabled = true;
        c.ref.halationBoostEv = 1.0f;
        c.tiled = true;
        c.tileW = 128;
        c.tileH = 96;
        c.tileMemory = true;
        c.boostMilestone = true;
        cases.push_back(c);
    };
    memBoost("memBoostFull",
             [](auto& l) { (void)l; },
             [](auto& r) { (void)r; });
    memBoost("memBoostOnly",
             [](auto& l) {
                 l.scatterAmount = 0.0f;
                 l.halationAmount = 0.0f;
             },
             [](auto& r) {
                 r.scatterAmount = 0.0f;
                 r.halationAmount = 0.0f;
             });
    // Exact app-still repro: C200 + paper 5 + correction-only DIR (no blur).
    {
        Case c{"memFilm14DirCorr", baseLook(), baseRef()};
        c.look.film = 14;
        c.look.paper = 5;
        c.look.outputColorSpace = 17;
        c.look.dirCouplersAmount = 0.88f;
        c.look.dirCouplersDiffusionUm = 0.0f;
        c.ref.film = 14;
        c.ref.paper = 5;
        c.ref.outputColorSpace = spektrafilm::ColorSpace::Srgb;
        c.ref.dirCouplersAmount = 0.88f;
        c.ref.dirCouplersDiffusionUm = 0.0f;
        c.tiled = true;
        c.tileW = 128;
        c.tileH = 96;
        c.tileMemory = true;
        c.conditionalEngine = true;
        cases.push_back(c);
    }
    // End-to-end incident replay: dark uniform input (thin ~0.02 negative,
    // like the black-JPEG still), unmetered (must stay dark) and metered
    // from its Bayer mosaic (must lift clearly), both through the app-exact
    // engine with reference parity.
    auto aeDarkCase = [&](const char* label, float filmEv, int visible,
                          const std::vector<uint16_t>& darkHalves) {
        Case c{label, baseLook(), baseRef()};
        c.look.film = 14;
        c.look.paper = 5;
        c.look.outputColorSpace = 17;
        c.look.dirCouplersAmount = 0.88f;
        c.look.dirCouplersDiffusionUm = 0.0f;
        c.look.filmExposureEv = filmEv;
        c.ref.film = 14;
        c.ref.paper = 5;
        c.ref.outputColorSpace = spektrafilm::ColorSpace::Srgb;
        c.ref.dirCouplersAmount = 0.88f;
        c.ref.dirCouplersDiffusionUm = 0.0f;
        c.ref.filmExposureEv = filmEv;
        c.customHalves = darkHalves;
        c.tiled = true;
        c.tileW = 128;
        c.tileH = 96;
        c.tileMemory = true;
        c.conditionalEngine = true;
        c.expectVisible = visible;
        cases.push_back(c);
    };
    std::vector<uint16_t> darkHalves((size_t)width * height * 4, 0);
    for (size_t px = 0; px < (size_t)width * height; ++px) {
        darkHalves[px * 4 + 0] = spektra_test::floatToHalf(0.02f);
        darkHalves[px * 4 + 1] = spektra_test::floatToHalf(0.02f);
        darkHalves[px * 4 + 2] = spektra_test::floatToHalf(0.02f);
        darkHalves[px * 4 + 3] = spektra_test::floatToHalf(1.0f);
    }
    aeDarkCase("aeDarkRenderUnmetered", 0.0f, 2, darkHalves);
    {
        // Mosaic to RGGB Bayer DN and meter it (the app stills flow: meter
        // snapshot, set EV, skip the gain fold). NOTE: halves are 0..1
        // linear; DN rescales by black/white.
        std::vector<uint16_t> bayer((size_t)width * height, 0);
        for (size_t px = 0; px < (size_t)width * height; ++px) {
            // Uniform input: every Bayer phase sees the same value.
            const float v = halfToFloat(darkHalves[px * 4]);
            const float dn = 64.0f + v * (1023.0f - 64.0f);
            bayer[px] =
                (uint16_t)std::clamp((int)std::lround(dn), 0, 65535);
        }
        spektrafilm_native::tables::BayerMeterInput mi{};
        mi.pixels = bayer.data();
        mi.width = width;
        mi.height = height;
        mi.blackRggb[0] = mi.blackRggb[1] = mi.blackRggb[2] =
            mi.blackRggb[3] = 64.0f;
        mi.whiteLevel = 1023.0f;
        const float meterEv =
            spektrafilm_native::tables::autoExposureEvFromBayer(mi, 0);
        std::printf("spektrafilm_validation info: [aeDarkRender] meterEv=%.3f\n",
                    meterEv);
        aeDarkCase("aeDarkRender", std::clamp(meterEv, -10.0f, 10.0f), 1,
                   darkHalves);
    }
    {
        Case c{"memSynthReject", baseLook(), baseRef()};
        c.look.grainEnabled = true;
        c.look.grainModel = 2;
        c.ref.grainEnabled = true;
        c.ref.grainModel = spektrafilm::GrainModel::GrainSynthesis;
        c.tiled = true;
        c.tileMemory = true;
        c.expectThrow = true;
        cases.push_back(c);
    }
    {
        Case c{"tiledBare", baseLook(), baseRef()};
        c.tiled = true;
        cases.push_back(c);
    }
    {
        Case c{"tiledHalDiffDir", baseLook(), baseRef()};
        c.look.halationEnabled = true;
        c.look.cameraDiffusionEnabled = true;
        c.look.dirCouplersAmount = 0.5f;
        c.look.grainEnabled = true;
        c.look.grainModel = 1;
        c.look.scannerEnabled = true;
        c.ref.halationEnabled = true;
        c.ref.cameraDiffusionEnabled = true;
        c.ref.dirCouplersAmount = 0.5f;
        c.ref.grainEnabled = true;
        c.ref.grainModel = spektrafilm::GrainModel::Production;
        c.ref.scannerEnabled = true;
        c.tiled = true;
        c.tileW = 128;
        c.tileH = 96;
        cases.push_back(c);
    }
    // Chunked submits: memHalDiffDir split into 4 submits must match.
    {
        Case c{"memChunked4", baseLook(), baseRef()};
        c.look.halationEnabled = true;
        c.look.cameraDiffusionEnabled = true;
        c.look.dirCouplersAmount = 0.5f;
        c.look.grainEnabled = true;
        c.look.grainModel = 1;
        c.look.scannerEnabled = true;
        c.ref.halationEnabled = true;
        c.ref.cameraDiffusionEnabled = true;
        c.ref.dirCouplersAmount = 0.5f;
        c.ref.grainEnabled = true;
        c.ref.grainModel = spektrafilm::GrainModel::Production;
        c.ref.scannerEnabled = true;
        c.tiled = true;
        c.tileW = 128;
        c.tileH = 96;
        c.tileMemory = true;
        c.submitChunks = 4u;
        cases.push_back(c);
    }

    // Engine cache key mirrors the allocation contract: production grain
    // layers are baked per grain model (a model flip needs a new engine;
    // record() skips effects whose buffers were never reserved).
    using BakedKey = std::tuple<int32_t, int32_t, int32_t, int32_t>;
    std::map<BakedKey, std::unique_ptr<spektrafilm_native::SpektraFilm>> engines;
    const auto engineFor = [&](const spektrafilm_native::FilmLook& look) {
        const BakedKey key{look.film, look.paper, look.rgbToRawMethod, look.grainModel};
        auto found = engines.find(key);
        if (found != engines.end()) {
            return found->second.get();
        }
        ci.look = look;
        auto engine = std::make_unique<spektrafilm_native::SpektraFilm>(ci);
        auto* ptr = engine.get();
        engines.emplace(key, std::move(engine));
        return ptr;
    };

    // Outputs stashed for the cross-case polarity check below.
    std::map<std::string, std::vector<uint8_t>> polarityOutputs;
    bool matchedCase = false;
    for (const Case& c : cases) {
        if (onlyCase && c.label != onlyCase) continue;
        matchedCase = true;
        spektrafilm_native::SpektraFilm* engine = nullptr;
        std::unique_ptr<spektrafilm_native::SpektraFilm> ownedEngine;
        if (c.needsCamera) {
            // Update path mutates baked state: dedicated engine per case,
            // driven exactly like the app (update API, then record).
            ci.look = c.look;
            ownedEngine =
                std::make_unique<spektrafilm_native::SpektraFilm>(ci);
            engine = ownedEngine.get();
            engine->updateCameraFilters(c.camera);
        } else if (c.needsProcessNeg) {
            // ProcessNegative paper tables bake filtration/timing.
            ci.look = c.look;
            ci.tiledMemorySaving = c.tileMemory;
            ownedEngine =
                std::make_unique<spektrafilm_native::SpektraFilm>(ci);
            engine = ownedEngine.get();
            engine->updateProcessNegativeTables(c.look);
            ci.tiledMemorySaving = false;
        } else if (c.tileMemory) {
            // Tile-memory engine: working-size arena instead of full-frame
            // effect scratch. Honors conditionalEngine (app stills config).
            ci.look = c.look;
            ci.tiledMemorySaving = true;
            ci.conditionalEffectScratch = c.conditionalEngine;
            ownedEngine =
                std::make_unique<spektrafilm_native::SpektraFilm>(ci);
            engine = ownedEngine.get();
            ci.tiledMemorySaving = false;
            ci.conditionalEffectScratch = false;
        } else if (c.ownEngine) {
            // Baked field outside the shared cache key: dedicated engine.
            ci.look = c.look;
            ci.conditionalEffectScratch = false;
            ownedEngine =
                std::make_unique<spektrafilm_native::SpektraFilm>(ci);
            engine = ownedEngine.get();
        } else if (c.conditionalEngine) {
            // Still-style engine: conditional scratch (the crash fix — null
            // scratch must dummy-bind and never dispatch).
            ci.look = c.look;
            ci.conditionalEffectScratch = true;
            ownedEngine =
                std::make_unique<spektrafilm_native::SpektraFilm>(ci);
            engine = ownedEngine.get();
            ci.conditionalEffectScratch = false;
        } else {
            engine = engineFor(c.look);
        }
        ri.look = c.look;
        ri.adaptiveTone = c.adaptiveTone;
        ri.timeSec = c.timeSec;
        if (!c.customHalves.empty()) {
            spektra_test::uploadRgba16f(ctx, pool, inputAlt, width, height,
                                        c.customHalves);
            ri.input.view = inputAlt.view;
        } else {
            ri.input.view = input.view;
        }
        ri.input.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        ri.input.layout = VK_IMAGE_LAYOUT_GENERAL;
        ri.input.width = width;
        ri.input.height = height;
        if (c.halfOut) {
            ri.output.view = outputHalf.view;
            ri.output.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        } else {
            ri.output.view = output.view;
            ri.output.format = VK_FORMAT_R8G8B8A8_UNORM;
        }
        ri.output.layout = VK_IMAGE_LAYOUT_GENERAL;
        ri.output.width = width;
        ri.output.height = height;
        if (c.tiled) {
            ri.tilingMode =
                spektrafilm_native::GpuRenderTilingMode::Tiled;
            ri.tileWidth = c.tileW;
            ri.tileHeight = c.tileH;
        } else {
            ri.tilingMode =
                spektrafilm_native::GpuRenderTilingMode::LegacyFullFrame;
            ri.tileWidth = 512;
            ri.tileHeight = 256;
        }
        if (c.expectThrow) {
            bool threw = false;
            try {
                VkCommandBufferBeginInfo begin{};
                begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                spektra_test::check(vkBeginCommandBuffer(cmd, &begin),
                                    "begin");
                engine->record(ri);
                spektra_test::check(vkEndCommandBuffer(cmd), "end");
                spektra_test::check(vkResetCommandBuffer(cmd, 0),
                                    "reset cmd");
            } catch (const std::invalid_argument&) {
                threw = true;
                spektra_test::check(vkResetCommandBuffer(cmd, 0),
                                    "reset cmd");
            }
            if (!threw) {
                std::fprintf(stderr, "validation: [%s] expected reject\n",
                             c.label.c_str());
                return 1;
            }
            std::printf("spektrafilm_validation ok: [%s] rejected as expected\n",
                        c.label.c_str());
            continue;
        }
        if (c.boostMilestone) {
            // Phase 1: milestone submit + host wait + readback, then feed
            // phase 2. Also cross-checks the CPU helper vs GPU ReduceMax.
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            spektra_test::check(vkBeginCommandBuffer(cmd, &begin), "begin");
            engine->recordBoostMilestone(ri);
            spektra_test::check(vkEndCommandBuffer(cmd), "end");
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cmd;
            spektra_test::check(
                vkQueueSubmit(ctx.queue, 1, &submit, VK_NULL_HANDLE),
                "submit");
            spektra_test::check(vkQueueWaitIdle(ctx.queue), "wait idle");
            spektra_test::check(vkResetCommandBuffer(cmd, 0), "reset cmd");
            const spektrafilm_native::SpektraFilmBoostMilestone ms =
                engine->readBoostMilestone(0);
            const spektrafilm_native::SpektraFilmBoostMilestone cpu =
                spektrafilm_native::SpektraFilm::computeBoostInfo(
                    ms.values[0], c.look.halationProtectEv,
                    c.look.halationBoostRange, c.look.halationBoostEv);
            double msDiff = 0.0;
            for (int i = 1; i < 4; ++i) {
                msDiff = std::max(msDiff,
                                  std::abs((double)ms.values[i] -
                                           (double)cpu.values[i]));
            }
            std::printf(
                "spektrafilm_validation ok: [%s-milestone] maxRaw=%.4f "
                "cpu_gpu_diff=%.2e\n",
                c.label.c_str(), ms.values[0], msDiff);
            if (!(ms.values[0] > 0.0f) || msDiff > 1e-3) {
                std::fprintf(stderr,
                             "validation: [%s] milestone mismatch\n",
                             c.label.c_str());
                return 1;
            }
            ri.hasBoostInfo = true;
            std::memcpy(ri.boostInfo, ms.values, sizeof(ri.boostInfo));
        } else {
            ri.hasBoostInfo = false;
        }

        // Tiled records rewrite descriptor sets between passes, so like the
        // app's still renderer they must submit and wait at each pass
        // boundary before the next pass is recorded.
        if (c.tiled) {
            ri.passBoundaryUserData = &passFlush;
            ri.passBoundary = flushPass;
        } else {
            ri.passBoundary = nullptr;
            ri.passBoundaryUserData = nullptr;
        }
        const auto runOnce = [&]() {
            // Chunked submits partition the tile grid into contiguous
            // row-major ranges, each submitted+waited separately.
            uint32_t chunkCount = 1u;
            uint32_t tilesTotal = 1u;
            if (c.submitChunks > 1u && c.tiled) {
                const uint32_t tw =
                    std::max(ri.tileWidth, 1u);
                const uint32_t th =
                    std::max(ri.tileHeight, 1u);
                tilesTotal = ((width + tw - 1u) / tw) *
                             ((height + th - 1u) / th);
                chunkCount = std::min(c.submitChunks, tilesTotal);
            }
            for (uint32_t k = 0; k < chunkCount; ++k) {
                if (chunkCount > 1u) {
                    ri.tileFirst =
                        (uint32_t)((uint64_t)k * tilesTotal / chunkCount);
                    const uint32_t end =
                        (uint32_t)((uint64_t)(k + 1u) * tilesTotal /
                                   chunkCount);
                    ri.tileCount = end - ri.tileFirst;
                    ri.tileFinalize = (k + 1u == chunkCount);
                }
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            spektra_test::check(vkBeginCommandBuffer(cmd, &begin), "begin");
            engine->record(ri);
            spektra_test::check(vkEndCommandBuffer(cmd), "end");
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cmd;
            spektra_test::check(
                vkQueueSubmit(ctx.queue, 1, &submit, VK_NULL_HANDLE), "submit");
            spektra_test::check(vkQueueWaitIdle(ctx.queue), "wait idle");
            spektra_test::check(vkResetCommandBuffer(cmd, 0), "reset cmd");
            }  // chunks
        };
        runOnce();
        if (c.halfOut) {
            // Half path: exact determinism + float parity vs reference
            // (half rounding only; RCM values unclamped on both sides).
            const std::vector<uint16_t> halfFirst =
                spektra_test::downloadRgba16f(ctx, pool, outputHalf, width,
                                              height);
            runOnce();
            const std::vector<uint16_t> halfSecond =
                spektra_test::downloadRgba16f(ctx, pool, outputHalf, width,
                                              height);
            if (halfFirst != halfSecond) {
                std::fprintf(stderr, "validation: [%s] nondeterministic\n",
                             c.label.c_str());
                return 1;
            }
            std::fill(refDest.begin(), refDest.end(), 0.0f);
            spektrafilm::ImageView refSourceView{};
            refSourceView.data = refSource.data();
            refSourceView.width = (int32_t)width;
            refSourceView.height = (int32_t)height;
            refSourceView.rowBytes = (int32_t)(width * 16);
            refSourceView.components = 4;
            refSourceView.bytesPerComponent = 4;
            spektrafilm::MutableImageView refDestView{};
            refDestView.data = refDest.data();
            refDestView.width = (int32_t)width;
            refDestView.height = (int32_t)height;
            refDestView.rowBytes = (int32_t)(width * 16);
            refDestView.components = 4;
            refDestView.bytesPerComponent = 4;
            spektrafilm::RenderWindow window{0, 0, (int32_t)width,
                                             (int32_t)height};
            spektrafilm::RenderParams params = c.ref;
            if (!reference->render(refSourceView, refDestView, window, params,
                                   c.timeSec)) {
                std::fprintf(stderr,
                             "validation: reference render failed: %s\n",
                             reference->lastError().c_str());
                return 1;
            }
            double maxDiff = 0.0, totalDiff = 0.0;
            size_t bad = 0;
            bool allFinite = true;
            for (size_t i = 0; i < (size_t)width * height * 4; ++i) {
                const float got = halfToFloat(halfFirst[i]);
                const float want = refDest[i];
                if (!std::isfinite(got)) {
                    allFinite = false;
                    break;
                }
                const double diff = std::abs((double)got - (double)want);
                maxDiff = std::max(maxDiff, diff);
                totalDiff += diff;
                if (diff > 0.005) {
                    ++bad;
                }
            }
            if (!allFinite) {
                std::fprintf(stderr, "validation: [%s] non-finite half\n",
                             c.label.c_str());
                return 1;
            }
            std::printf(
                "spektrafilm_validation ok: [%s] %ux%u deterministic "
                "half_max_abs=%.5f half_mean_abs=%.6f half_bad_px=%zu\n",
                c.label.c_str(), width, height, maxDiff,
                totalDiff / ((size_t)width * height * 4), bad);
            if (maxDiff > 0.005) {
                std::fprintf(stderr,
                             "validation: [%s] half parity exceeded\n",
                             c.label.c_str());
                return 1;
            }
            continue;
        }
        const std::vector<uint8_t> first =
            spektra_test::downloadRgba8(ctx, pool, output, width, height);
        runOnce();
        const std::vector<uint8_t> second =
            spektra_test::downloadRgba8(ctx, pool, output, width, height);

        if (first != second) {
            std::fprintf(stderr, "validation: [%s] nondeterministic\n",
                         c.label.c_str());
            return 1;
        }
        uint64_t alphaSum = 0;
        bool constant = true;
        for (size_t i = 0; i < (size_t)width * height; ++i) {
            alphaSum += first[i * 4 + 3];
            if (first[i * 4] != first[0] || first[i * 4 + 1] != first[1] ||
                first[i * 4 + 2] != first[2]) {
                constant = false;
            }
        }
        const double meanAlpha = (double)alphaSum / (width * height);
        if (std::abs(meanAlpha - 255.0) > 1.0) {
            std::fprintf(stderr, "validation: [%s] alpha mean=%.1f\n",
                         c.label.c_str(), meanAlpha);
            return 1;
        }
        if (constant && c.expectVisible == 0) {
            std::fprintf(stderr, "validation: [%s] constant output\n",
                         c.label.c_str());
            return 1;
        }
        if (c.expectVisible != 0) {
            double m = 0.0;
            for (size_t i = 0; i < (size_t)width * height; ++i) {
                m += first[i * 4] + first[i * 4 + 1] + first[i * 4 + 2];
            }
            m /= (double)((size_t)width * height * 3) * 255.0;
            std::printf("spektrafilm_validation ok: [%s-visible] mean=%.3f\n",
                        c.label.c_str(), m);
            if (c.expectVisible == 1 && (m < 0.05 || m > 0.8)) {
                std::fprintf(stderr,
                             "validation: [%s] metered render not visible "
                             "(mean %.3f)\n",
                             c.label.c_str(), m);
                return 1;
            }
        }

        // Parity: reference renderer on the identical half-quantized input.
        std::fill(refDest.begin(), refDest.end(), 0.0f);
        std::vector<float> refSrcCustom;
        const float* refSrc = refSource.data();
        if (!c.customHalves.empty()) {
            refSrcCustom.resize((size_t)width * height * 4);
            for (size_t i = 0; i < (size_t)width * height * 4; ++i) {
                refSrcCustom[i] = halfToFloat(c.customHalves[i]);
            }
            refSrc = refSrcCustom.data();
        }
        if (c.adaptiveTone[0] > 0.0f) {
            std::vector<float> warped(refSrc, refSrc + size_t(width) * height * 4);
            const tonemap::AdaptiveToneParams curve{c.adaptiveTone[0], c.adaptiveTone[1], c.adaptiveTone[2]};
            for (size_t p = 0; p < warped.size(); p += 4) {
                const float r = warped[p], g = warped[p + 1], b = warped[p + 2];
                const float y = .2722287168f * (.613097f * r + .339523f * g + .047380f * b) +
                                .6740817658f * (.070194f * r + .916354f * g + .013452f * b) +
                                .0536895174f * (.020616f * r + .109570f * g + .869815f * b);
                const float gain = tonemap::adaptiveToneGain(y * c.adaptiveTone[3], curve);
                for (int ch = 0; ch < 3; ++ch) warped[p + ch] *= gain;
            }
            refSrcCustom = std::move(warped);
            refSrc = refSrcCustom.data();
        }
    spektrafilm::ImageView refSourceView{};
    refSourceView.data = refSrc;
    refSourceView.width = (int32_t)width;
    refSourceView.height = (int32_t)height;
    refSourceView.rowBytes = (int32_t)(width * 16);
    refSourceView.components = 4;
    refSourceView.bytesPerComponent = 4;
    spektrafilm::MutableImageView refDestView{};
    refDestView.data = refDest.data();
    refDestView.width = (int32_t)width;
    refDestView.height = (int32_t)height;
    refDestView.rowBytes = (int32_t)(width * 16);
    refDestView.components = 4;
    refDestView.bytesPerComponent = 4;
    spektrafilm::RenderWindow window{0, 0, (int32_t)width, (int32_t)height};
    spektrafilm::RenderParams params = c.ref;
    if (!reference->render(refSourceView, refDestView, window, params,
                           c.timeSec)) {
        std::fprintf(stderr, "validation: reference render failed: %s\n",
                     reference->lastError().c_str());
        return 1;
    }
    uint32_t maxDiff = 0;
    uint64_t totalDiff = 0;
    size_t diffPixels = 0;
    for (size_t i = 0; i < (size_t)width * height * 4; ++i) {
        const int channel = (int)(i % 4);
        float expected = refDest[i];
        if (channel < 3) {
            expected = std::fmin(std::fmax(expected, 0.0f), 1.0f);
        }
        const uint32_t want =
            (uint32_t)std::lround(expected * 255.0f);
        const uint32_t got = first[i];
        const uint32_t diff = want > got ? want - got : got - want;
        maxDiff = std::max(maxDiff, diff);
        totalDiff += diff;
        if (diff > 1) {
            ++diffPixels;
        }
    }
    const double meanDiff =
        (double)totalDiff / ((size_t)width * height * 4);
    std::printf(
        "spektrafilm_validation ok: [%s] %ux%u deterministic alpha=%.1f "
        "parity_max_lsb=%u parity_mean_lsb=%.3f parity_bad_px=%zu\n",
        c.label.c_str(), width, height, meanAlpha, maxDiff, meanDiff,
        diffPixels);
    if (maxDiff > 1) {
        std::fprintf(stderr, "validation: [%s] parity exceeded (+-1 LSB8)\n",
                     c.label.c_str());
        return 1;
    }
        if (c.label == "velvia-print-expect-negative" || c.label == "velvia-scan" ||
            c.label == "velvia-scan-print-controls") {
            polarityOutputs[c.label] = first;
        }
    }  // cases
    if (onlyCase) return matchedCase ? 0 : 2;
    // Product invariant: for a positive stock, the print path must invert
    // relative to the scan path (optical print of a slide is a negative).
    // This is what the app's auto-switch (positive -> scan) protects against.
    {
        const auto printIt = polarityOutputs.find("velvia-print-expect-negative");
        const auto scanIt = polarityOutputs.find("velvia-scan");
        const auto controlsIt = polarityOutputs.find("velvia-scan-print-controls");
        if (printIt == polarityOutputs.end() || scanIt == polarityOutputs.end() ||
            controlsIt == polarityOutputs.end()) {
            std::fprintf(stderr, "validation: polarity cases missing\n");
            return 1;
        }
        if (scanIt->second != controlsIt->second) {
            std::fprintf(stderr, "validation: scan leaked print/paper controls\n");
            return 1;
        }
        const std::vector<uint8_t>& print = printIt->second;
        const std::vector<uint8_t>& scan = scanIt->second;
        double same = 0.0, inverted = 0.0;
        size_t n = 0;
        for (size_t i = 0; i < (size_t)width * height; ++i) {
            for (int ch = 0; ch < 3; ++ch) {
                const double p = print[i * 4 + ch] / 255.0;
                const double s = scan[i * 4 + ch] / 255.0;
                const double dSame = p - s;
                const double dInv = p - (1.0 - s);
                same += dSame * dSame;
                inverted += dInv * dInv;
                ++n;
            }
        }
        same /= n;
        inverted /= n;
        std::printf("spektrafilm_validation polarity: print-vs-scan MSE=%.5f print-vs-inverted-scan MSE=%.5f\n",
                    same, inverted);
        if (!(inverted * 4.0 < same)) {
            std::fprintf(stderr, "validation: print path does not invert relative to scan path\n");
            return 1;
        }
    }
    // Sensor-matrix check (adapter self-consistency): the in-shader
    // sensor->linear-sRGB conversion must equal premultiplying the input on
    // CPU and recording with identity. No reference coverage: the reference
    // renderer takes linear-sRGB input directly.
    {
        // Non-trivial but sane matrix (gain + cross-talk, row-major).
        const float kM[9] = {1.15f, -0.10f, -0.05f, -0.12f, 1.08f,
                             0.04f, 0.03f, -0.18f, 1.15f};
        std::vector<uint16_t> preHalves(halves.size());
        for (size_t px = 0; px < (size_t)width * height; ++px) {
            const float r = halfToFloat(halves[px * 4 + 0]);
            const float g = halfToFloat(halves[px * 4 + 1]);
            const float b = halfToFloat(halves[px * 4 + 2]);
            preHalves[px * 4 + 0] = spektra_test::floatToHalf(
                kM[0] * r + kM[1] * g + kM[2] * b);
            preHalves[px * 4 + 1] = spektra_test::floatToHalf(
                kM[3] * r + kM[4] * g + kM[5] * b);
            preHalves[px * 4 + 2] = spektra_test::floatToHalf(
                kM[6] * r + kM[7] * g + kM[8] * b);
            preHalves[px * 4 + 3] = halves[px * 4 + 3];
        }
        spektra_test::Image preInput = spektra_test::makeImage(
            ctx, width, height, VK_FORMAT_R16G16B16A16_SFLOAT);
        spektra_test::uploadRgba16f(ctx, pool, preInput, width, height,
                                    preHalves);
        spektrafilm_native::FilmLook mLook = baseLook();
        spektrafilm_native::SpektraFilm* mEngine = engineFor(mLook);
        const auto runMatrix = [&](const float m[9], VkImageView view) {
            ri.look = mLook;
            ri.timeSec = 0.0;
            std::memcpy(ri.sensorToLinearSrgb, m,
                        sizeof(ri.sensorToLinearSrgb));
            ri.input.view = view;
            ri.output.view = output.view;
            ri.output.format = VK_FORMAT_R8G8B8A8_UNORM;
            ri.output.layout = VK_IMAGE_LAYOUT_GENERAL;
            ri.output.width = width;
            ri.output.height = height;
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            spektra_test::check(vkBeginCommandBuffer(cmd, &begin), "begin");
            mEngine->record(ri);
            spektra_test::check(vkEndCommandBuffer(cmd), "end");
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cmd;
            spektra_test::check(
                vkQueueSubmit(ctx.queue, 1, &submit, VK_NULL_HANDLE),
                "submit");
            spektra_test::check(vkQueueWaitIdle(ctx.queue), "wait idle");
            spektra_test::check(vkResetCommandBuffer(cmd, 0), "reset cmd");
            return spektra_test::downloadRgba8(ctx, pool, output, width,
                                               height);
        };
        const float kIdentity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        const std::vector<uint8_t> viaShader = runMatrix(kM, input.view);
        const std::vector<uint8_t> viaCpu = runMatrix(kIdentity, preInput.view);
        uint32_t mMax = 0;
        for (size_t i = 0; i < viaShader.size(); ++i) {
            const uint32_t a = viaShader[i], b = viaCpu[i];
            mMax = std::max(mMax, a > b ? a - b : b - a);
        }
        std::printf(
            "spektrafilm_validation ok: [sensorMatrix] parity_max_lsb=%u\n",
            mMax);
        if (mMax > 1) {
            std::fprintf(stderr,
                         "validation: [sensorMatrix] shader/CPU mismatch\n");
            spektra_test::destroyImage(ctx, preInput);
            return 1;
        }
        spektra_test::destroyImage(ctx, preInput);
    }
    // Cross-resolution grade match (stills vs preview): the same look
    // through a quarter-size and a half-size engine must agree after
    // downsampling. Grain stays off (particle scale is resolution-native by
    // design); DIR/scanner sigmas are physical so they scale correctly.
    {
        const uint32_t w2 = width * 2u;
        const uint32_t h2 = height * 2u;
        std::vector<uint16_t> halves2((size_t)w2 * h2 * 4);
        for (uint32_t y = 0; y < h2; ++y) {
            for (uint32_t x = 0; x < w2; ++x) {
                halves2[((size_t)y * w2 + x) * 4 + 0] =
                    spektra_test::floatToHalf(0.05f + 0.9f * (float)x / w2);
                halves2[((size_t)y * w2 + x) * 4 + 1] =
                    spektra_test::floatToHalf(0.05f + 0.9f * (float)y / h2);
                halves2[((size_t)y * w2 + x) * 4 + 2] =
                    spektra_test::floatToHalf(0.05f + 0.45f * ((float)x / w2 +
                                                               (float)y / h2));
                halves2[((size_t)y * w2 + x) * 4 + 3] =
                    spektra_test::floatToHalf(1.0f);
            }
        }
        spektra_test::Image input2 =
            spektra_test::makeImage(ctx, w2, h2, VK_FORMAT_R16G16B16A16_SFLOAT);
        spektra_test::uploadRgba16f(ctx, pool, input2, w2, h2, halves2);
        spektra_test::Image output2 =
            spektra_test::makeImage(ctx, w2, h2, VK_FORMAT_R8G8B8A8_UNORM);
        spektra_test::transitionToGeneral(ctx, pool, output2);
        spektrafilm_native::SpektraFilmCreateInfo ci2 = ci;
        ci2.maxWidth = w2;
        ci2.maxHeight = h2;
        ci2.look = baseLook();
        ci2.look.dirCouplersAmount = 0.5f;
        ci2.look.scannerEnabled = true;
        auto engine2 = std::make_unique<spektrafilm_native::SpektraFilm>(ci2);
        spektrafilm_native::SpektraFilm* engine1 = engineFor(baseLook());
        const auto runRes = [&](spektrafilm_native::SpektraFilm* engine,
                                VkImageView inView, uint32_t w, uint32_t h,
                                const spektra_test::Image& outImage) {
            static const float kEye[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
            ri.look = baseLook();
            ri.look.dirCouplersAmount = 0.5f;
            ri.look.scannerEnabled = true;
            ri.timeSec = 0.0;
            std::memcpy(ri.sensorToLinearSrgb, kEye,
                        sizeof(ri.sensorToLinearSrgb));
            ri.input.view = inView;
            ri.input.width = w;
            ri.input.height = h;
            ri.output.view = outImage.view;
            ri.output.format = VK_FORMAT_R8G8B8A8_UNORM;
            ri.output.layout = VK_IMAGE_LAYOUT_GENERAL;
            ri.output.width = w;
            ri.output.height = h;
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            spektra_test::check(vkBeginCommandBuffer(cmd, &begin), "begin");
            engine->record(ri);
            spektra_test::check(vkEndCommandBuffer(cmd), "end");
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cmd;
            spektra_test::check(
                vkQueueSubmit(ctx.queue, 1, &submit, VK_NULL_HANDLE),
                "submit");
            spektra_test::check(vkQueueWaitIdle(ctx.queue), "wait idle");
            spektra_test::check(vkResetCommandBuffer(cmd, 0), "reset cmd");
            return spektra_test::downloadRgba8(ctx, pool, outImage, w, h);
        };
        const std::vector<uint8_t> small =
            runRes(engine1, input.view, width, height, output);
        const std::vector<uint8_t> big =
            runRes(engine2.get(), input2.view, w2, h2, output2);
        uint32_t xMax = 0;
        uint64_t xTotal = 0;
        size_t xCount = 0;
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                for (int ch = 0; ch < 3; ++ch) {
                    const uint32_t s = small[((size_t)y * width + x) * 4 + ch];
                    uint32_t acc = 0;
                    for (uint32_t dy = 0; dy < 2; ++dy) {
                        for (uint32_t dx = 0; dx < 2; ++dx) {
                            acc += big[((size_t)(y * 2 + dy) * w2 + (x * 2 + dx)) * 4 + ch];
                        }
                    }
                    const uint32_t b = (acc + 2u) / 4u;
                    const uint32_t d = s > b ? s - b : b - s;
                    xMax = std::max(xMax, d);
                    xTotal += d;
                    ++xCount;
                }
            }
        }
        const double xMean = (double)xTotal / (double)xCount;
        std::printf("spektrafilm_validation ok: [xresMatch] max_lsb=%u mean_lsb=%.3f\n",
                    xMax, xMean);
        if (xMax > 4 || xMean > 1.0) {
            std::fprintf(stderr, "validation: [xresMatch] grade diverged\n");
            spektra_test::destroyImage(ctx, input2);
            spektra_test::destroyImage(ctx, output2);
            return 1;
        }
        spektra_test::destroyImage(ctx, input2);
        spektra_test::destroyImage(ctx, output2);
    }
    // Negative test: APD timing without academy data must reject loudly
    // (tables are zeros; silent black is worse than an error).
    {
        spektrafilm_native::FilmLook apdLook = baseLook();
        apdLook.printTiming = 1;
        spektrafilm_native::SpektraFilm* apdEngine = engineFor(baseLook());
        ri.look = apdLook;
        ri.timeSec = 0.0;
        ri.output.view = output.view;
        ri.output.format = VK_FORMAT_R8G8B8A8_UNORM;
        ri.output.layout = VK_IMAGE_LAYOUT_GENERAL;
        ri.output.width = width;
        ri.output.height = height;
        ri.input.view = input.view;
        ri.input.width = width;
        ri.input.height = height;
        bool threw = false;
        try {
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            spektra_test::check(vkBeginCommandBuffer(cmd, &begin), "begin");
            apdEngine->record(ri);
            spektra_test::check(vkEndCommandBuffer(cmd), "end");
            spektra_test::check(vkResetCommandBuffer(cmd, 0), "reset cmd");
        } catch (const std::invalid_argument&) {
            threw = true;
            spektra_test::check(vkResetCommandBuffer(cmd, 0), "reset cmd");
        }
        if (!threw) {
            std::fprintf(stderr, "validation: [apdReject] expected reject\n");
            return 1;
        }
        std::printf("spektrafilm_validation ok: [apdReject] rejected as expected\n");
    }
    // Dither test: env-gated TPDF must be deterministic and within 1 LSB
    // mean of the undithered output (no reference parity: reference has no
    // output dither).
    {
        spektrafilm_native::SpektraFilm* dEngine = engineFor(baseLook());
        ri.look = baseLook();
        ri.timeSec = 0.0;
        ri.output.view = output.view;
        ri.output.format = VK_FORMAT_R8G8B8A8_UNORM;
        ri.output.layout = VK_IMAGE_LAYOUT_GENERAL;
        ri.output.width = width;
        ri.output.height = height;
        ri.input.view = input.view;
        ri.input.width = width;
        ri.input.height = height;
        ri.tilingMode =
            spektrafilm_native::GpuRenderTilingMode::LegacyFullFrame;
        const auto runD = [&]() {
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            spektra_test::check(vkBeginCommandBuffer(cmd, &begin), "begin");
            dEngine->record(ri);
            spektra_test::check(vkEndCommandBuffer(cmd), "end");
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cmd;
            spektra_test::check(
                vkQueueSubmit(ctx.queue, 1, &submit, VK_NULL_HANDLE),
                "submit");
            spektra_test::check(vkQueueWaitIdle(ctx.queue), "wait idle");
            spektra_test::check(vkResetCommandBuffer(cmd, 0), "reset cmd");
            return spektra_test::downloadRgba8(ctx, pool, output, width,
                                               height);
        };
        unsetenv("SPEKTRA_FILM_OUTPUT_DITHER");
        const std::vector<uint8_t> plain = runD();
        setenv("SPEKTRA_FILM_OUTPUT_DITHER", "1", 1);
        const std::vector<uint8_t> d1 = runD();
        const std::vector<uint8_t> d2 = runD();
        unsetenv("SPEKTRA_FILM_OUTPUT_DITHER");
        if (d1 != d2) {
            std::fprintf(stderr, "validation: [dither] nondeterministic\n");
            return 1;
        }
        uint32_t dMax = 0;
        uint64_t dTotal = 0;
        for (size_t i = 0; i < plain.size(); ++i) {
            const uint32_t a = plain[i], b = d1[i];
            const uint32_t d = a > b ? a - b : b - a;
            dMax = std::max(dMax, d);
            dTotal += d;
        }
        const double dMean = (double)dTotal / (double)plain.size();
        std::printf(
            "spektrafilm_validation ok: [dither] deterministic max_lsb=%u mean_lsb=%.3f\n",
            dMax, dMean);
        if (dMax > 2 || dMean > 1.0) {
            std::fprintf(stderr, "validation: [dither] out of bound\n");
            return 1;
        }
    }
    // Glow-factor tap (film+UltraHDR gain-map input): the export must leave
    // the main output bit-identical, must carry a finite ~1.0 quotient field,
    // and the willWriteGlowGain gate must agree with record() exactly.
    {
        using Mode = spektrafilm_native::GpuRenderTilingMode;
        spektrafilm_native::FilmLook halLook = baseLook();
        halLook.halationEnabled = true;
        const bool wHal = spektrafilm_native::SpektraFilm::willWriteGlowGain(
            halLook, width, height, Mode::LegacyFullFrame, false);
        const bool wPlain = spektrafilm_native::SpektraFilm::willWriteGlowGain(
            baseLook(), width, height, Mode::LegacyFullFrame, false);
        spektrafilm_native::FilmLook negLook = baseLook();
        negLook.process = 2;
        const bool wNeg = spektrafilm_native::SpektraFilm::willWriteGlowGain(
            negLook, width, height, Mode::LegacyFullFrame, false);
        const bool wTiled = spektrafilm_native::SpektraFilm::willWriteGlowGain(
            halLook, width, height, Mode::Tiled, false);
        const bool wTileMem = spektrafilm_native::SpektraFilm::willWriteGlowGain(
            halLook, width, height, Mode::LegacyFullFrame, true);
        const bool wTileMemTiled = spektrafilm_native::SpektraFilm::willWriteGlowGain(
            halLook, width, height, Mode::Tiled, true);
        if (!wHal || wPlain || wNeg || wTiled || wTileMem || !wTileMemTiled) {
            std::fprintf(stderr,
                         "validation: [glowGainTap] gate mismatch hal=%d plain=%d neg=%d tiled=%d "
                         "tilemem=%d\n",
                         (int)wHal, (int)wPlain, (int)wNeg, (int)wTiled, (int)wTileMem);
            return 1;
        }
        static const float kEye[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        spektrafilm_native::SpektraFilmRecordInfo tri{};
        tri.commandBuffer = cmd;
        tri.input.view = input.view;
        tri.input.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        tri.input.layout = VK_IMAGE_LAYOUT_GENERAL;
        tri.input.width = width;
        tri.input.height = height;
        tri.output.view = output.view;
        tri.output.format = VK_FORMAT_R8G8B8A8_UNORM;
        tri.output.layout = VK_IMAGE_LAYOUT_GENERAL;
        tri.output.width = width;
        tri.output.height = height;
        tri.frameSlot = 0;
        tri.timeSec = 0.0;
        tri.look = halLook;
        std::memcpy(tri.sensorToLinearSrgb, kEye, sizeof(tri.sensorToLinearSrgb));
        tri.tilingMode = Mode::LegacyFullFrame;
        tri.glowGainOutput.view = outputHalf.view;
        tri.glowGainOutput.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        tri.glowGainOutput.layout = VK_IMAGE_LAYOUT_GENERAL;
        tri.glowGainOutput.width = width;
        tri.glowGainOutput.height = height;
        const char* vr = nullptr;
        if (!spektrafilm_native::SpektraFilm::validateRecordInfo(tri, 2, width, height, &vr)) {
            std::fprintf(stderr, "validation: [glowGainTap] good tap rejected: %s\n",
                         vr ? vr : "?");
            return 1;
        }
        tri.glowGainOutput.format = VK_FORMAT_R8G8B8A8_UNORM;
        if (spektrafilm_native::SpektraFilm::validateRecordInfo(tri, 2, width, height, nullptr)) {
            std::fprintf(stderr, "validation: [glowGainTap] bad format accepted\n");
            return 1;
        }
        tri.glowGainOutput.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        tri.glowGainOutput.width = width + 1u;
        if (spektrafilm_native::SpektraFilm::validateRecordInfo(tri, 2, width, height, nullptr)) {
            std::fprintf(stderr, "validation: [glowGainTap] bad dims accepted\n");
            return 1;
        }
        tri.glowGainOutput.width = width;
        // Still-style engine: conditional scratch baked for the halation look.
        ci.look = halLook;
        ci.conditionalEffectScratch = true;
        auto tapEngine = std::make_unique<spektrafilm_native::SpektraFilm>(ci);
        ci.conditionalEffectScratch = false;
        const auto runTap = [&](bool withTap) {
            if (withTap) {
                tri.glowGainOutput.view = outputHalf.view;
            } else {
                tri.glowGainOutput.view = VK_NULL_HANDLE;
            }
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            spektra_test::check(vkBeginCommandBuffer(cmd, &begin), "begin");
            tapEngine->record(tri);
            spektra_test::check(vkEndCommandBuffer(cmd), "end");
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cmd;
            spektra_test::check(vkQueueSubmit(ctx.queue, 1, &submit, VK_NULL_HANDLE), "submit");
            spektra_test::check(vkQueueWaitIdle(ctx.queue), "wait idle");
            spektra_test::check(vkResetCommandBuffer(cmd, 0), "reset cmd");
        };
        // Heavier look on a second fresh image (different stocks, DIR, print
        // diffusion): the tap must stay non-degenerate there too.
        spektra_test::Image freshTap =
            spektra_test::makeImage(ctx, width, height, VK_FORMAT_R16G16B16A16_SFLOAT);
        spektra_test::transitionToGeneral(ctx, pool, freshTap);
        // Fresh-image export with the halation-only look. Pre-filled with a
        // sentinel so a silent no-op export (valid layout, zero writes) fails
        // loudly instead of reading back plausible-looking data.
        {
            std::vector<uint16_t> sentinel((size_t)width * height * 4,
                                           spektra_test::floatToHalf(3.0f));
            spektra_test::uploadRgba16f(ctx, pool, freshTap, width, height, sentinel);
        }
        tri.look = halLook;
        tri.output.view = output.view;
        tri.output.format = VK_FORMAT_R8G8B8A8_UNORM;
        tri.glowGainOutput.view = freshTap.view;
        tri.glowGainOutput.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        tri.glowGainOutput.layout = VK_IMAGE_LAYOUT_GENERAL;
        tri.glowGainOutput.width = width;
        tri.glowGainOutput.height = height;
        {
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            spektra_test::check(vkBeginCommandBuffer(cmd, &begin), "begin");
            tapEngine->record(tri);
            spektra_test::check(vkEndCommandBuffer(cmd), "end");
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cmd;
            spektra_test::check(vkQueueSubmit(ctx.queue, 1, &submit, VK_NULL_HANDLE), "submit");
            spektra_test::check(vkQueueWaitIdle(ctx.queue), "wait idle");
            spektra_test::check(vkResetCommandBuffer(cmd, 0), "reset cmd");
        }
        {
            const std::vector<uint16_t> b2 =
                spektra_test::downloadRgba16f(ctx, pool, freshTap, width, height);
            double bMin = 1e30, bMax = -1e30;
            for (size_t i = 0; i < (size_t)width * height * 4; ++i) {
                if (i % 4 == 3) continue;
                const float v = halfToFloat(b2[i]);
                if (!std::isfinite(v)) {
                    std::fprintf(stderr, "validation: [glowGainTap] B2 non-finite\n");
                    return 1;
                }
                bMin = std::min(bMin, (double)v);
                bMax = std::max(bMax, (double)v);
            }
            std::printf("spektrafilm_validation ok: [glowGainTap-B2] fresh+halOnly min=%.4f max=%.4f\n",
                        bMin, bMax);
            if (bMin == 3.0 && bMax == 3.0) {
                std::fprintf(stderr, "validation: [glowGainTap] B2 export never wrote (sentinel intact)\n");
                return 1;
            }
            // Glow quotient on a smooth gradient: scatter is ~energy-preserving,
            // so the field must sit inside the shader clamp range. A linear
            // ramp blurs to itself, so the field may legitimately be uniform
            // here (variation is covered by the spot test below); the mean
            // must still sit near 1.0.
            if (!(bMin >= 0.0) || !(bMax <= 8.0)) {
                std::fprintf(stderr, "validation: [glowGainTap] B2 out of ratio range\n");
                return 1;
            }
        }
        // Spot pattern (bright square on dark field): scatter MUST reshape the
        // field — halo above 1 around the square, dip below 1 inside it. This
        // is the variation check the smooth gradient cannot provide, and it
        // fails loudly on a constant-1.0 no-op export. Wide kernels: the
        // default 256px validation geometry undersamples stock sigmas
        // (sub-pixel blur nets to identity), so force a wide scatter/bounce
        // scale here — scratch depends only on enables, which are unchanged.
        {
            spektrafilm_native::FilmLook spotLook = halLook;
            spotLook.scatterScale = 16.0f;
            spotLook.halationScale = 16.0f;
            spektra_test::Image spotImg =
                spektra_test::makeImage(ctx, width, height, VK_FORMAT_R16G16B16A16_SFLOAT);
            spektra_test::transitionToGeneral(ctx, pool, spotImg);
            std::vector<uint16_t> spot((size_t)width * height * 4);
            for (uint32_t y = 0; y < height; ++y) {
                for (uint32_t x = 0; x < width; ++x) {
                    const bool inside = x >= width / 4 && x < 3 * width / 4 && y >= height / 4 &&
                                        y < 3 * height / 4;
                    const float v = inside ? 4.0f : 0.05f;
                    for (int c = 0; c < 3; ++c) spot[((size_t)y * width + x) * 4 + c] = spektra_test::floatToHalf(v);
                    spot[((size_t)y * width + x) * 4 + 3] = spektra_test::floatToHalf(1.0f);
                }
            }
            spektra_test::uploadRgba16f(ctx, pool, spotImg, width, height, spot);
            std::vector<uint16_t> sentinel2((size_t)width * height * 4,
                                            spektra_test::floatToHalf(3.0f));
            spektra_test::uploadRgba16f(ctx, pool, freshTap, width, height, sentinel2);
            tri.look = spotLook;
            tri.input.view = spotImg.view;
            tri.input.format = VK_FORMAT_R16G16B16A16_SFLOAT;
            tri.input.layout = VK_IMAGE_LAYOUT_GENERAL;
            tri.input.width = width;
            tri.input.height = height;
            tri.output.view = output.view;
            tri.output.format = VK_FORMAT_R8G8B8A8_UNORM;
            tri.glowGainOutput.view = freshTap.view;
            tri.glowGainOutput.format = VK_FORMAT_R16G16B16A16_SFLOAT;
            tri.glowGainOutput.layout = VK_IMAGE_LAYOUT_GENERAL;
            tri.glowGainOutput.width = width;
            tri.glowGainOutput.height = height;
            {
                VkCommandBufferBeginInfo begin{};
                begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                spektra_test::check(vkBeginCommandBuffer(cmd, &begin), "begin");
                tapEngine->record(tri);
                spektra_test::check(vkEndCommandBuffer(cmd), "end");
                VkSubmitInfo submit{};
                submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                submit.commandBufferCount = 1;
                submit.pCommandBuffers = &cmd;
                spektra_test::check(vkQueueSubmit(ctx.queue, 1, &submit, VK_NULL_HANDLE), "submit");
                spektra_test::check(vkQueueWaitIdle(ctx.queue), "wait idle");
                spektra_test::check(vkResetCommandBuffer(cmd, 0), "reset cmd");
            }
            const std::vector<uint16_t> spotPx =
                spektra_test::downloadRgba16f(ctx, pool, freshTap, width, height);
            double sMin = 1e30, sMax = -1e30, sSum = 0.0;
            size_t sCount = 0;
            for (size_t i = 0; i < (size_t)width * height * 4; ++i) {
                if (i % 4 == 3) continue;
                const float v = halfToFloat(spotPx[i]);
                if (!std::isfinite(v)) {
                    std::fprintf(stderr, "validation: [glowGainTap] spot non-finite\n");
                    spektra_test::destroyImage(ctx, spotImg);
                    return 1;
                }
                sMin = std::min(sMin, (double)v);
                sMax = std::max(sMax, (double)v);
                sSum += v;
                ++sCount;
            }
            const double sMean = sSum / (double)sCount;
            std::printf("spektrafilm_validation ok: [glowGainTap-spot] min=%.4f mean=%.4f max=%.4f\n",
                        sMin, sMean, sMax);
            spektra_test::destroyImage(ctx, spotImg);
            tri.input.view = input.view;
            if (!(sMax > 1.02)) {
                std::fprintf(stderr, "validation: [glowGainTap] spot produced no halo (max=%.4f)\n", sMax);
                return 1;
            }
            if (!(sMin < 1.0)) {
                std::fprintf(stderr, "validation: [glowGainTap] spot produced no core dip (min=%.4f)\n", sMin);
                return 1;
            }
            if (!(sMean > 0.5) || !(sMean < 2.0)) {
                std::fprintf(stderr, "validation: [glowGainTap] spot mean off (%.4f)\n", sMean);
                return 1;
            }
        }
        spektrafilm_native::FilmLook offLook = halLook;
        offLook.film = 15;
        offLook.paper = 5;
        offLook.dirCouplersAmount = 0.8f;
        offLook.printDiffusionEnabled = true;
        ci.look = offLook;
        ci.conditionalEffectScratch = true;
        auto offEngine = std::make_unique<spektrafilm_native::SpektraFilm>(ci);
        ci.conditionalEffectScratch = false;
        if (!spektrafilm_native::SpektraFilm::willWriteGlowGain(
                offLook, width, height, Mode::LegacyFullFrame, false)) {
            std::fprintf(stderr, "validation: [glowGainTap] offline-like look gated off\n");
            return 1;
        }
        tri.look = offLook;
        tri.output.view = output.view;
        tri.output.format = VK_FORMAT_R8G8B8A8_UNORM;
        tri.glowGainOutput.view = freshTap.view;
        tri.glowGainOutput.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        tri.glowGainOutput.layout = VK_IMAGE_LAYOUT_GENERAL;
        tri.glowGainOutput.width = width;
        tri.glowGainOutput.height = height;
        {
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            spektra_test::check(vkBeginCommandBuffer(cmd, &begin), "begin");
            offEngine->record(tri);
            spektra_test::check(vkEndCommandBuffer(cmd), "end");
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cmd;
            spektra_test::check(vkQueueSubmit(ctx.queue, 1, &submit, VK_NULL_HANDLE), "submit");
            spektra_test::check(vkQueueWaitIdle(ctx.queue), "wait idle");
            spektra_test::check(vkResetCommandBuffer(cmd, 0), "reset cmd");
        }
        const std::vector<uint16_t> freshTapPx =
            spektra_test::downloadRgba16f(ctx, pool, freshTap, width, height);
        double fMin = 1e30, fMax = -1e30, fSum = 0.0;
        size_t fCount = 0;
        for (size_t i = 0; i < (size_t)width * height * 4; ++i) {
            if (i % 4 == 3) continue;
            const float v = halfToFloat(freshTapPx[i]);
            if (!std::isfinite(v)) {
                std::fprintf(stderr, "validation: [glowGainTap] fresh tap non-finite\n");
                return 1;
            }
            fMin = std::min(fMin, (double)v);
            fMax = std::max(fMax, (double)v);
            fSum += v;
            ++fCount;
        }
        // Glow quotient over the heavier look: scatter preserves energy on a
        // smooth gradient, so the field mean must sit near 1.0 (agnostic of
        // stock/tables by construction: response cancels in the quotient).
        const double fMean = fSum / (double)fCount;
        std::printf("spektrafilm_validation ok: [glowGainTap-fresh] min=%.4f mean=%.4f max=%.4f\n",
                    fMin, fMean, fMax);
        std::printf("spektrafilm_validation glow-quotient: mean=%.4f (expect ~1.0)\n", fMean);
        spektra_test::destroyImage(ctx, freshTap);
        if (!(fMean > 0.5) || !(fMean < 2.0)) {
            std::fprintf(stderr, "validation: [glowGainTap] fresh tap degenerate\n");
            return 1;
        }
        // Restore the halation-look record state for the stability check below.
        tri.look = halLook;
        tri.output.view = output.view;
        tri.output.format = VK_FORMAT_R8G8B8A8_UNORM;
        runTap(true);
        const std::vector<uint8_t> outWithTap =
            spektra_test::downloadRgba8(ctx, pool, output, width, height);
        const std::vector<uint16_t> tap =
            spektra_test::downloadRgba16f(ctx, pool, outputHalf, width, height);
        runTap(false);
        const std::vector<uint8_t> outPlain =
            spektra_test::downloadRgba8(ctx, pool, output, width, height);
        if (outWithTap != outPlain) {
            std::fprintf(stderr, "validation: [glowGainTap] tap perturbed main output\n");
            return 1;
        }
        // A tile-memory engine must export the same full-size quotient and
        // leave the SDR output unchanged. All four 128x96 centers are tested.
        ci.look = halLook;
        ci.conditionalEffectScratch = true;
        ci.tiledMemorySaving = true;
        auto tiledTapEngine = std::make_unique<spektrafilm_native::SpektraFilm>(ci);
        ci.tiledMemorySaving = false;
        ci.conditionalEffectScratch = false;
        tri.tilingMode = Mode::Tiled;
        tri.tileWidth = 128u;
        tri.tileHeight = 96u;
        tri.glowGainOutput.view = outputHalf.view;
        tri.passBoundary = flushPass;
        tri.passBoundaryUserData = &passFlush;
        {
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            spektra_test::check(vkBeginCommandBuffer(cmd, &begin), "tile tap begin");
            tiledTapEngine->record(tri);
            spektra_test::check(vkEndCommandBuffer(cmd), "tile tap end");
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cmd;
            spektra_test::check(vkQueueSubmit(ctx.queue, 1, &submit, VK_NULL_HANDLE), "tile tap submit");
            spektra_test::check(vkQueueWaitIdle(ctx.queue), "tile tap wait");
            spektra_test::check(vkResetCommandBuffer(cmd, 0), "tile tap reset");
        }
        const auto tiledOutput = spektra_test::downloadRgba8(ctx, pool, output, width, height);
        const auto tiledTap = spektra_test::downloadRgba16f(ctx, pool, outputHalf, width, height);
        if (tiledOutput != outWithTap || tiledTap != tap) {
            std::fprintf(stderr, "validation: [glowGainTap] tiled output/tap differs from full frame\n");
            return 1;
        }
        std::printf("spektrafilm_validation ok: [glowGainTap-tiled] output and quotient parity\n");
        double tMin = 1e30, tMax = -1e30, tSum = 0.0;
        size_t tCount = 0;
        for (size_t i = 0; i < (size_t)width * height * 4; ++i) {
            if (i % 4 == 3) continue;  // RGB only; alpha passes through
            const float v = halfToFloat(tap[i]);
            if (!std::isfinite(v)) {
                std::fprintf(stderr, "validation: [glowGainTap] non-finite tap\n");
                return 1;
            }
            tMin = std::min(tMin, (double)v);
            tMax = std::max(tMax, (double)v);
            tSum += v;
            ++tCount;
        }
        const double tMean = tSum / (double)tCount;
        std::printf("spektrafilm_validation ok: [glowGainTap] output_stable min=%.4f mean=%.4f max=%.4f\n",
                    tMin, tMean, tMax);
        // Smooth-gradient field may be near-uniform (blur of a ramp nets to
        // ~1.0); the spot test above covers variation. Mean must sit near 1.
        if (!(tMean > 0.5) || !(tMean < 2.0)) {
            std::fprintf(stderr, "validation: [glowGainTap] degenerate tap\n");
            return 1;
        }
        // No linear scatter path (plain look) + tap must reject loudly rather
        // than record an unwritten image.
        tri.look = baseLook();
        tri.glowGainOutput.view = outputHalf.view;
        bool threw = false;
        try {
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            spektra_test::check(vkBeginCommandBuffer(cmd, &begin), "begin");
            tapEngine->record(tri);
            spektra_test::check(vkEndCommandBuffer(cmd), "end");
            spektra_test::check(vkResetCommandBuffer(cmd, 0), "reset cmd");
        } catch (const std::invalid_argument&) {
            threw = true;
            spektra_test::check(vkResetCommandBuffer(cmd, 0), "reset cmd");
        }
        if (!threw) {
            std::fprintf(stderr, "validation: [glowGainTap] missing-path tap accepted\n");
            return 1;
        }
        std::printf("spektrafilm_validation ok: [glowGainTap] rejects as expected\n");
    }
    // Engines alias ctx.device: tear them (and images) down first.
    engines.clear();
    spektra_test::destroyImage(ctx, input);
    spektra_test::destroyImage(ctx, output);
    spektra_test::destroyImage(ctx, outputHalf);
    spektra_test::destroyImage(ctx, inputAlt);
    vkDestroyCommandPool(ctx.device, pool, nullptr);
    spektra_test::destroyCtx(ctx);
    return 0;
}
