// MoltenVK parity + timing: CPU fp32 mirror vs GainmapCompute shader output.
// Usage: gainmap_vulkan_validation <gainmap.spv> [--tolerance-lsb N] [--cst-ap1] [--agx-tap]
//        [--grade-tap di|logc3] [--bench-base N] [--iters K]
// Exit 0 when max abs diff <= tolerance (default 1 LSB, 2 near black).
// --cst-ap1 runs the production AP1->sRGB CST instead of identity.
// --bench-base N skips verification and times N*N -> N/2*N/2 dispatches.
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "gainmap/GainmapCompute.h"
#include "tonemap/Agx.h"
#include "tonemap/color/ColorSpace.h"

namespace {
void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string(what) + " VkResult=" + std::to_string(int(r)));
}
uint32_t memType(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    throw std::runtime_error("no memory type");
}
std::vector<uint32_t> readSpv(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open SPV: " + path);
    f.seekg(0, std::ios::end);
    auto n = f.tellg();
    f.seekg(0);
    if (n <= 0 || size_t(n) % 4) throw std::runtime_error("bad SPV size");
    std::vector<uint32_t> v(size_t(n) / 4);
    f.read(reinterpret_cast<char*>(v.data()), n);
    return v;
}
float srgbEotf(float c) {
    if (c <= 0.04045f) return c / 12.92f;
    return std::pow((c + 0.055f) / 1.055f, 2.4f);
}
// --agx-tap oracle: the AgX curve/matrices the packed block was built from.
struct AgxTapOracle {
    tonemap::AgxCurve curve;
    tonemap::AgxMatrices matrices;
    float headroom;
};
const AgxTapOracle* gAgxTap = nullptr;

// --grade-tap oracle: CPU mirror of shader_common/shaders/grade.glsl.
struct GradeTapOracle {
    float tone[4];  // Contrast, Highlights, Shadows, Midtones
    float sat, vib, vibStrength, guardStart, guardEnd, pivot, black;
    tonemap::color::TransferFunction transfer;
};
const GradeTapOracle* gGradeTap = nullptr;

float smooth01(float a, float b, float x) {
    const float t = std::fmin(std::fmax((x - a) / (b - a), 0.0f), 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
float gradeToneLuma(float y, const GradeTapOracle& g) {
    if (g.tone[0] != 0.0f && y > g.black) {
        const float k = std::exp2(0.7f * g.tone[0]), d = y - g.pivot;
        y = d >= 0.0f ? g.pivot + d * k
                      : g.black + (g.pivot - g.black) * std::pow((y - g.black) / (g.pivot - g.black), k);
    }
    const float d = y - g.pivot, yr = y;
    const auto bump = [](float x, float c, float w) { return std::exp(-((x - c) / w) * ((x - c) / w)); };
    const float x = std::fmax(d - 0.30f, 0.0f), w = 0.10f;
    const float hl = 0.10f * smooth01(0.02f, 0.30f, d) + 0.25f * (x < w ? x * x / (2.0f * w) : x - 0.5f * w);
    y += g.tone[3] * 0.05f * bump(d, 0.0f, 0.10f);
    y += g.tone[2] * 0.06f * bump(d, -0.14f, 0.10f) * smooth01(g.black, g.black + 0.25f * (g.pivot - g.black), yr);
    y += g.tone[1] * hl;
    return y;
}
float gradeChromaGain(const float lin[3], float linY, const GradeTapOracle& g) {
    if (g.sat == 0.0f && g.vib == 0.0f) return 1.0f;
    const float satGain = g.sat >= 0.0f ? 1.0f + 0.85f * g.sat : std::fmax(0.0f, 1.0f + g.sat);
    const float r = std::fmax(lin[0], 0.0f), gg = std::fmax(lin[1], 0.0f), b = std::fmax(lin[2], 0.0f);
    const float hi = std::fmax(r, std::fmax(gg, b));
    const float occ = std::fmin(std::fmax((hi - std::fmin(r, std::fmin(gg, b))) / std::fmax(hi, 0.06f), 0.0f), 1.0f);
    const float vibGain = g.vib >= 0.0f ? 1.0f + g.vibStrength * g.vib * std::pow(std::fmax(1.0f - occ, 0.0f), 0.30f) *
                                                    smooth01(g.guardStart, g.guardEnd, linY)
                                        : 1.0f + (std::pow(occ, 4.0f) - 1.0f) * -g.vib;
    return satGain * vibGain;
}

void cpuEncode(const float hdr[3], const float sdr[3], const gainmap::GainmapParams& p, const float* cst,
               float out[3], float cover = 0.0f, float flatness = 1.0f, const float* glow = nullptr) {
    // Scene-exposure match (mirrors the shader): the HDR tap is pre-exposure.
    const float e = std::fmax(p.hdrExposure, 1e-6f);
    float hw[3] = {std::isfinite(hdr[0]) ? hdr[0] * e : 0.0f, std::isfinite(hdr[1]) ? hdr[1] * e : 0.0f,
                   std::isfinite(hdr[2]) ? hdr[2] * e : 0.0f};
    // Grade tap (mirrors the shader): tone as a luminance gain in the grade
    // space, chroma gain measured here and applied after the CST.
    float gradeChroma = 1.0f;
    if (p.gradeTap && gGradeTap) {
        float lin[3];
        for (int r = 0; r < 3; ++r) lin[r] = cst[r * 3 + 0] * hw[0] + cst[r * 3 + 1] * hw[1] + cst[r * 3 + 2] * hw[2];
        const float y = 0.21532597f * lin[0] + 0.71600819f * lin[1] + 0.06866589f * lin[2];
        if (y > 1e-8f) {
            const float graded = gradeToneLuma(tonemap::color::encodeTransfer(y, gGradeTap->transfer), *gGradeTap);
            const float k = std::fmax(tonemap::color::decodeTransfer(graded, gGradeTap->transfer), 0.0f) / y;
            for (float& c : hw) c *= k;
        }
        gradeChroma = gradeChromaGain(lin, y, *gGradeTap);
    }
    // Film glow factor (mirrors the shader): dimensionless post/pre
    // quotient applied pre-CST; null keeps the pure scene tap.
    if (glow != nullptr && p.glowStrength > 0.0f) {
        const float gs = std::fmin(std::fmax(p.glowStrength, 0.0f), 1.0f);
        const float gm = std::fmax(p.glowMax, 1.0f);
        for (int c = 0; c < 3; ++c) {
            const float gc = std::isfinite(glow[c]) ? std::fmax(glow[c], 0.0f) : 0.0f;
            hw[c] *= 1.0f + (std::fmin(std::fmax(gc, 1.0f), gm) - 1.0f) * gs;
        }
    }
    float hdrLin[3] = {0, 0, 0};
    for (int r = 0; r < 3; ++r) {
        hdrLin[r] = cst[r * 3 + 0] * hw[0] + cst[r * 3 + 1] * hw[1] + cst[r * 3 + 2] * hw[2];
    }
    if (p.agxTap && gAgxTap) {
        const auto agx = tonemap::agxRender({hw[0], hw[1], hw[2]}, gAgxTap->curve, gAgxTap->matrices);
        for (int r = 0; r < 3; ++r) hdrLin[r] = agx[r] * gAgxTap->headroom;
    }
    if (gradeChroma != 1.0f) {
        const float hy = 0.21532597f * hdrLin[0] + 0.71600819f * hdrLin[1] + 0.06866589f * hdrLin[2];
        for (float& c : hdrLin) c = hy + (c - hy) * gradeChroma;
    }
    float hdrPos[3] = {std::fmax(hdrLin[0], 0.0f), std::fmax(hdrLin[1], 0.0f), std::fmax(hdrLin[2], 0.0f)};
    // Single-channel luminance gain + per-channel RGB gains (matches shader).
    const float h = 0.2126f * hdrPos[0] + 0.7152f * hdrPos[1] + 0.0722f * hdrPos[2];
    float sl[3];
    for (int i = 0; i < 3; ++i) {
        const float c = std::isfinite(sdr[i]) ? sdr[i] : 0.0f;
        sl[i] = std::fmax(srgbEotf(std::fmin(std::fmax(c, 0.0f), 1.0f)), 0.0f);
    }
    const float s = 0.2126f * sl[0] + 0.7152f * sl[1] + 0.0722f * sl[2];
    // Chroma protection (mirrors the shader): attenuate where the SDR base
    // is bright AND saturated.
    const float smxC = std::fmax(sl[0], std::fmax(sl[1], sl[2]));
    const float smnC = std::fmin(sl[0], std::fmin(sl[1], sl[2]));
    const float satC = (smxC - smnC) / std::fmax(smxC, 1e-3f);
    const float st = std::fmin(std::fmax((smxC - 0.5f) / (0.9f - 0.5f), 0.0f), 1.0f);
    const float satW = p.satProtect * (st * st * (3.0f - 2.0f * st)) *
                       std::fmin(std::fmax(satC, 0.0f), 1.0f);
    const float gain = (h + std::fmax(p.offsetHdr, 1e-6f)) / (s + std::fmax(p.offsetSdr, 1e-6f));
    float logGain = std::log2(std::fmax(gain, 1e-9f));
    float logC[3];
    for (int c = 0; c < 3; ++c) {
        const float gc = (hdrPos[c] + std::fmax(p.offsetHdr, 1e-6f)) / (sl[c] + std::fmax(p.offsetSdr, 1e-6f));
        logC[c] = std::log2(std::fmax(gc, 1e-9f));
    }
    // Shadow fade (smoothstep, mirrors the shader): shared toneScale.
    float toneScale = 1.0f;
    {
        const float t = std::fmin(std::fmax((s - 0.03f) / (0.50f - 0.03f), 0.0f), 1.0f);
        toneScale = t * t * (3.0f - 2.0f * t) * (1.0f - satW);
    }
    logGain *= toneScale;
    for (int c = 0; c < 3; ++c) logC[c] *= toneScale;
    // Clipped-core hue fallback (mirrors the shader): white HDR tap uses
    // luma gain so decoded HDR keeps SDR hue.
    const float hdrMaxC = std::fmax(hdrPos[0], std::fmax(hdrPos[1], hdrPos[2]));
    const float hdrMinC = std::fmin(hdrPos[0], std::fmin(hdrPos[1], hdrPos[2]));
    const float hdrSatC = (hdrMaxC - hdrMinC) / std::fmax(hdrMaxC, 1e-3f);
    auto sstep = [](float e0, float e1, float x) {
        const float t = std::fmin(std::fmax((x - e0) / (e1 - e0), 0.0f), 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };
    const float hdrWhiteW = (1.0f - sstep(0.05f, 0.15f, hdrSatC)) * sstep(0.5f, 0.9f, h);
    const float mcOn = p.multiChannelMap ? 1.0f : 0.0f;
    const float useLuma = std::fmax(1.0f - mcOn, hdrWhiteW * mcOn);
    float logF[3];
    for (int c = 0; c < 3; ++c) logF[c] = logC[c] * (1.0f - useLuma) + logGain * useLuma;
    // Specular boost for sensor-clipped texels (mask cover only, like the
    // shader: no tap-based detection), AND local flatness (recovered
    // gradients keep the ratio; only flat-pinned blocks boost), gated on
    // max SDR channel (saturated primaries hit 1.0 in one channel at low
    // luma) so near-white SDR holding gradation keeps the measured ratio.
    if (cover != 0.0f && p.clipBoost > 0.0f) {
        const float smax = std::fmax(sl[0], std::fmax(sl[1], sl[2]));
        const float gt = std::fmin(std::fmax((smax - 0.85f) / (0.98f - 0.85f), 0.0f), 1.0f);
        cover *= gt * gt * (3.0f - 2.0f * gt);
    }
    if (cover != 0.0f && p.clipBoost > 0.0f) {
        cover *= std::fmin(std::fmax(flatness, 0.0f), 1.0f);
        cover *= 1.0f - satW;
        const float boostLog = std::log2(std::fmax(p.clipBoost, 1e-6f));
        logGain = logGain * (1.0f - cover) + boostLog * cover;
        for (int c = 0; c < 3; ++c) logF[c] = logF[c] * (1.0f - cover) + boostLog * cover;
    }
    for (int c = 0; c < 3; ++c) {
        const float norm = std::fmin(
            std::fmax((logF[c] - p.minLog2) / std::fmax(p.maxLog2 - p.minLog2, 1e-6f), 0.0f), 1.0f);
        out[c] = std::fmin(std::fmax(std::floor(std::pow(norm, p.gamma) * 255.0f + 0.5f) / 255.0f, 0.0f),
                           1.0f);
    }
}
uint16_t f2h(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000u;
    uint32_t mant = x & 0x7fffffu;
    int exp = int((x >> 23) & 0xffu) - 127 + 15;
    if (exp <= 0) {
        if (exp < -10) return uint16_t(sign);
        mant = (mant | 0x800000u) >> (1 - exp);
        return uint16_t(sign + ((mant + 0x1000u) >> 13));
    }
    if (exp >= 31) return uint16_t(sign | 0x7c00u);
    uint32_t rounded = (mant + 0x1000u) >> 13;
    if (rounded == 0x400u) {
        rounded = 0;
        if (++exp >= 31) return uint16_t(sign | 0x7c00u);
    }
    return uint16_t(sign + (uint32_t(exp) << 10) + rounded);
}
float h2f(uint16_t h) {
    uint32_t sign = uint32_t(h & 0x8000u) << 16;
    int exp = (h >> 10) & 31;
    uint32_t mant = h & 1023u, x = 0;
    if (exp == 0) {
        if (mant == 0) x = sign;
        else {
            int e = -14;
            while (!(mant & 1024u)) {
                mant <<= 1;
                e--;
            }
            mant &= 1023u;
            x = sign | (uint32_t(e + 127) << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        x = sign | 0x7f800000u | (mant << 13);
    } else {
        x = sign | (uint32_t(exp - 15 + 127) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &x, 4);
    return f;
}
struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};
Image createImage(VkDevice dev, VkPhysicalDevice pd, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage,
                  VkImageLayout initial) {
    Image o{};
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = initial;
    check(vkCreateImage(dev, &ci, nullptr, &o.image), "vkCreateImage");
    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(dev, o.image, &mr);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = memType(pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(dev, &ai, nullptr, &o.memory), "vkAllocateMemory");
    check(vkBindImageMemory(dev, o.image, o.memory, 0), "vkBindImageMemory");
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = o.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    check(vkCreateImageView(dev, &vi, nullptr, &o.view), "vkCreateImageView");
    return o;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: gainmap_vulkan_validation <gainmap.spv>\n";
        return 2;
    }
    const std::string spvPath = argv[1];
    int toleranceLsb = 1;
    bool cstAp1 = false;
    bool clipBoostTest = false;
    bool agxTapTest = false;
    std::string gradeTapSpace;
    uint32_t benchW = 0, benchH = 0;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--tolerance-lsb" && i + 1 < argc) toleranceLsb = std::atoi(argv[++i]);
        if (a == "--cst-ap1") cstAp1 = true;
        // --clip-boost: upload a synthetic half/half clip mask and expect the
        // specular boost on the clipped half. Needs a driver with working
        // integer storage-image reads (Adreno ✓; MoltenVK/M5 reads garbage on
        // binding 3 as of Sep 2026, so this stays off on Mac).
        if (a == "--clip-boost") clipBoostTest = true;
        // --agx-tap: render the HDR tap through AgX for a 2.73x display
        // (default preset) and check it against the CPU AgX reference.
        if (a == "--agx-tap") agxTapTest = true;
        // --grade-tap di|logc3: apply a stacked Grade (all six sliders) to
        // the tap in DaVinci Intermediate or LogC3 (black code above 0).
        if (a == "--grade-tap" && i + 1 < argc) gradeTapSpace = argv[++i];
        // --bench 2048 | --bench 4080x3060 | (legacy) --bench-base N
        auto parseBench = [&](const char* s) {
            unsigned w = 0, h = 0;
            if (std::sscanf(s, "%ux%u", &w, &h) == 2) {
                benchW = w;
                benchH = h;
            } else if (std::sscanf(s, "%u", &w) == 1) {
                benchW = benchH = w;
            }
        };
        if ((a == "--bench" || a == "--bench-base") && i + 1 < argc) parseBench(argv[++i]);
        if (a == "--iters" && i + 1 < argc) ++i;  // accepted, ignored (see note below)
    }
    if (benchW > 8192 || benchH > 8192 || (benchW == 0) != (benchH == 0)) {
        std::cerr << "bench dims must be W[xH], each 1..8192\n";
        return 2;
    }

    // Instance with portability enumeration for MoltenVK.
    std::vector<const char*> exts;
    uint32_t extCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> extProps(extCount);
    vkEnumerateInstanceExtensionProperties(nullptr, &extCount, extProps.data());
    VkInstanceCreateFlags flags = 0;
    for (auto& e : extProps)
        if (std::strcmp(e.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0) {
            exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "gainmap_validation";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.flags = flags;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = (uint32_t)exts.size();
    ici.ppEnabledExtensionNames = exts.data();
    VkInstance instance = VK_NULL_HANDLE;
    check(vkCreateInstance(&ici, nullptr, &instance), "vkCreateInstance");

    uint32_t pdCount = 0;
    check(vkEnumeratePhysicalDevices(instance, &pdCount, nullptr), "enum count");
    if (!pdCount) throw std::runtime_error("no physical device");
    std::vector<VkPhysicalDevice> pds(pdCount);
    check(vkEnumeratePhysicalDevices(instance, &pdCount, pds.data()), "enum");
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    uint32_t qf = 0;
    uint32_t stampBits = 0;
    for (auto d : pds) {
        uint32_t qc = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qc, nullptr);
        std::vector<VkQueueFamilyProperties> qp(qc);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qc, qp.data());
        for (uint32_t q = 0; q < qc; ++q)
            if (qp[q].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                pd = d;
                qf = q;
                stampBits = qp[q].timestampValidBits;
                break;
            }
        if (pd) break;
    }
    if (!pd) throw std::runtime_error("no compute queue");
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(pd, &props);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = qf;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VkDevice dev = VK_NULL_HANDLE;
    check(vkCreateDevice(pd, &dci, nullptr, &dev), "vkCreateDevice");
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(dev, qf, 0, &queue);

    const uint32_t baseW = benchW ? benchW : 64;
    const uint32_t baseH = benchH ? benchH : 64;
    const uint32_t mapW = std::max(1u, baseW / 2);
    const uint32_t mapH = std::max(1u, baseH / 2);
    const bool bench = benchW != 0;
    Image hdr = createImage(dev, pd, baseW, baseH, VK_FORMAT_R16G16B16A16_SFLOAT,
                            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    Image sdr = createImage(dev, pd, baseW, baseH, VK_FORMAT_R8G8B8A8_UNORM,
                            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    Image map = createImage(dev, pd, mapW, mapH * 2, VK_FORMAT_R8G8B8A8_UNORM,
                            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    // Synthetic clip mask on the map grid: left half clear, right half
    // clipped. Exercises the specular-boost path on GPU vs the oracle.
    // Production R16UI bitmask format, matching the app contract.
    Image clip = createImage(dev, pd, mapW, mapH, VK_FORMAT_R16_UINT,
                             VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             VK_IMAGE_LAYOUT_UNDEFINED);

    // Host-visible staging for upload/download.
    auto makeStaging = [&](VkDeviceSize n) {
        VkBuffer b = VK_NULL_HANDLE;
        VkDeviceMemory m = VK_NULL_HANDLE;
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = n;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        check(vkCreateBuffer(dev, &bci, nullptr, &b), "staging buffer");
        VkMemoryRequirements mr{};
        vkGetBufferMemoryRequirements(dev, b, &mr);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = memType(pd, mr.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(vkAllocateMemory(dev, &ai, nullptr, &m), "staging mem");
        check(vkBindBufferMemory(dev, b, m, 0), "staging bind");
        return std::pair<VkBuffer, VkDeviceMemory>(b, m);
    };
    const VkDeviceSize hdrBytes = VkDeviceSize(baseW) * baseH * 8;
    const VkDeviceSize sdrBytes = VkDeviceSize(baseW) * baseH * 4;
    const VkDeviceSize mapBytes = VkDeviceSize(mapW) * mapH * 8;
    const VkDeviceSize clipBytes = VkDeviceSize(mapW) * mapH * 2;
    auto [hdrStaging, hdrMem] = makeStaging(hdrBytes);
    auto [sdrStaging, sdrMem] = makeStaging(sdrBytes);
    auto [mapStaging, mapMem] = makeStaging(mapBytes);
    auto [clipStaging, clipMem] = makeStaging(clipBytes);

    // Deterministic fixture: ramps + saturated patches + black trap.
    // Bench mode uses a flat fill (content is irrelevant to timing).
    std::vector<uint16_t> hdrPx(size_t(baseW) * baseH * 4);
    std::vector<uint8_t> sdrPx(size_t(baseW) * baseH * 4);
    std::vector<std::array<float, 3>> hdrLin(bench ? 0 : size_t(baseW) * baseH);
    std::vector<std::array<float, 3>> sdrEnc(bench ? 0 : size_t(baseW) * baseH);
    if (bench) {
        const uint16_t midH = f2h(0.5f);
        for (size_t i = 0; i < hdrPx.size(); i += 4) {
            hdrPx[i + 0] = midH;
            hdrPx[i + 1] = midH;
            hdrPx[i + 2] = midH;
            hdrPx[i + 3] = f2h(1.0f);
        }
        for (size_t i = 0; i < sdrPx.size(); i += 4) {
            sdrPx[i + 0] = 128;
            sdrPx[i + 1] = 128;
            sdrPx[i + 2] = 128;
            sdrPx[i + 3] = 255;
        }
    }
    for (uint32_t y = 0; y < (bench ? 0u : baseH); ++y)
        for (uint32_t x = 0; x < (bench ? 0u : baseW); ++x) {
            const size_t i = size_t(y) * baseW + x;
            float h = 0.05f * float((x + y) % 64);  // 0..~3.1 linear ramp
            if (x >= 48 && y < 16) h = 8.0f;        // hot patch
            if (x < 8 && y >= 56) h = 0.0f;         // black trap
            float r = h, g = h * 0.6f, b = h * 1.4f;
            if (x >= 32 && x < 40 && y >= 32 && y < 40) {
                r = 6.0f;
                g = 0.2f;
                b = 0.2f;
            }
            // Oracle must compare against stored (quantized) inputs, exactly
            // what the GPU samples: R16F for HDR, 8-bit for SDR.
            hdrPx[i * 4 + 0] = f2h(r);
            hdrPx[i * 4 + 1] = f2h(g);
            hdrPx[i * 4 + 2] = f2h(b);
            hdrPx[i * 4 + 3] = f2h(1.0f);
            hdrLin[i] = {h2f(hdrPx[i * 4 + 0]), h2f(hdrPx[i * 4 + 1]), h2f(hdrPx[i * 4 + 2])};
            const float s = float(x) / float(baseW - 1);
            const float se[3] = {s, 1.0f - s, 0.25f + 0.5f * s};
            sdrPx[i * 4 + 0] = uint8_t(std::round(se[0] * 255));
            sdrPx[i * 4 + 1] = uint8_t(std::round(se[1] * 255));
            sdrPx[i * 4 + 2] = uint8_t(std::round(se[2] * 255));
            sdrPx[i * 4 + 3] = 255;
            sdrEnc[i] = {sdrPx[i * 4 + 0] / 255.0f, sdrPx[i * 4 + 1] / 255.0f, sdrPx[i * 4 + 2] / 255.0f};
        }
    void* p = nullptr;
    check(vkMapMemory(dev, hdrMem, 0, hdrBytes, 0, &p), "map hdr");
    std::memcpy(p, hdrPx.data(), size_t(hdrBytes));
    vkUnmapMemory(dev, hdrMem);
    check(vkMapMemory(dev, sdrMem, 0, sdrBytes, 0, &p), "map sdr");
    std::memcpy(p, sdrPx.data(), size_t(sdrBytes));
    vkUnmapMemory(dev, sdrMem);
    {
        std::vector<uint16_t> clipPx(size_t(mapW) * mapH);
        // Fully blown right half (all 4 R/G1/G2/B bits), like sensor white.
        for (uint32_t y = 0; y < mapH; ++y)
            for (uint32_t x = 0; x < mapW; ++x) clipPx[size_t(y) * mapW + x] = (x >= mapW / 2) ? 0xFu : 0u;
        check(vkMapMemory(dev, clipMem, 0, clipBytes, 0, &p), "map clip");
        std::memcpy(p, clipPx.data(), size_t(clipBytes));
        vkUnmapMemory(dev, clipMem);
    }

    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = qf;
    check(vkCreateCommandPool(dev, &pci, nullptr, &pool), "pool");
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo acai{};
    acai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    acai.commandPool = pool;
    acai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    acai.commandBufferCount = 1;
    check(vkAllocateCommandBuffers(dev, &acai, &cmd), "alloc cmd");
    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    check(vkCreateFence(dev, &fci, nullptr, &fence), "fence");

    auto barrier = [](VkImage img, VkAccessFlags src, VkAccessFlags dst, VkImageLayout oldL, VkImageLayout newL) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = src;
        b.dstAccessMask = dst;
        b.oldLayout = oldL;
        b.newLayout = newL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        return b;
    };

    std::vector<uint32_t> spv = readSpv(spvPath);
    gainmap::GainmapCreateInfo gci{};
    gci.context.physicalDevice = pd;
    gci.context.device = dev;
    gci.shaderSpirv = spv.data();
    gci.shaderSpirvBytes = spv.size() * 4;
    gainmap::GainmapCompute gainmap(gci);
    gainmap::GainmapParams params;  // production default = AP1->sRGB
    params.writeHdrChromaticity = true;
    params.fullResMap = false;  // this test feeds an explicit half-res grid + half-res clip mask
    float cstIdent[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    const float* cst = cstAp1 ? params.hdrToLinearSrgbRowMajor : cstIdent;
    if (!cstAp1)
        for (int i = 0; i < 9; ++i) params.hdrToLinearSrgbRowMajor[i] = cstIdent[i];
    AgxTapOracle agxOracle{};
    if (agxTapTest) {
        // The tap's input is linear sRGB scaled by the test CST; AgX takes it
        // to Rec2020 itself. No chromaticity plane: production writes it only
        // for RAWR NTRL.
        const float headroom = std::exp2(1.45f);
        const tonemap::AgxParams agx = tonemap::agxForHeadroom(tonemap::AgxParams{}, headroom);
        tonemap::color::Mat3 cstM{}, inputToBase{};
        for (int i = 0; i < 9; ++i) cstM[i] = cst[i];
        const auto toBase = tonemap::color::linearGamutMatrix(tonemap::color::Gamut::SRgbRec709,
                                                              tonemap::color::Gamut::Rec2020);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                for (int k = 0; k < 3; ++k) inputToBase[r * 3 + c] += toBase[r * 3 + k] * cstM[k * 3 + c];
        const auto block = tonemap::packAgxGpuBlock(agx, inputToBase, headroom);
        std::memcpy(params.agxTapBlock.data(), &block, sizeof(block));
        params.agxTap = true;
        params.writeHdrChromaticity = false;
        agxOracle.curve = tonemap::compileAgxCurve(agx);
        agxOracle.matrices = tonemap::compileAgxMatrices(agx.primaries);
        agxOracle.matrices.ap1ToBase = inputToBase;
        agxOracle.headroom = headroom;
        gAgxTap = &agxOracle;
    }
    GradeTapOracle gradeOracle{};
    if (!gradeTapSpace.empty()) {
        using TF = tonemap::color::TransferFunction;
        const TF tf = gradeTapSpace == "logc3" ? TF::LogC3 : TF::DaVinciIntermediate;
        gradeOracle = {{0.4f, -0.8f, 0.3f, -0.2f}, 0.5f, 0.6f, 0.7f, 0.02f, 0.10f,
                       tonemap::color::encodeTransfer(0.18f, tf), tonemap::color::encodeTransfer(0.0f, tf), tf};
        const auto& g = gradeOracle;
        params.gradeTapBlock = {g.tone[0], g.tone[1], g.tone[2], g.tone[3], g.sat, g.vib, g.vibStrength,
                                g.guardStart, g.guardEnd, g.pivot, g.black, float(static_cast<uint32_t>(tf))};
        params.gradeTap = true;
        gGradeTap = &gradeOracle;
    }

    // Timestamp pool for dispatch timing (2 queries around the dispatch).
    VkQueryPool tsPool = VK_NULL_HANDLE;
    if (stampBits > 0 && props.limits.timestampPeriod > 0) {
        VkQueryPoolCreateInfo qpi{};
        qpi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpi.queryCount = 2;
        check(vkCreateQueryPool(dev, &qpi, nullptr, &tsPool), "query pool");
    }

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(cmd, &begin), "begin");
    if (tsPool) vkCmdResetQueryPool(cmd, tsPool, 0, 2);
    // Upload staging -> images.
    VkImageMemoryBarrier up[3] = {barrier(hdr.image, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
                                  barrier(sdr.image, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
                                  barrier(clip.image, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 3, up);
    VkBufferImageCopy bic{};
    bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bic.imageExtent = {baseW, baseH, 1};
    vkCmdCopyBufferToImage(cmd, hdrStaging, hdr.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    vkCmdCopyBufferToImage(cmd, sdrStaging, sdr.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    VkBufferImageCopy bicClip{};
    bicClip.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bicClip.imageExtent = {mapW, mapH, 1};
    vkCmdCopyBufferToImage(cmd, clipStaging, clip.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bicClip);
    VkImageMemoryBarrier toGeneral[4] = {
        barrier(hdr.image, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
        barrier(sdr.image, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
        barrier(map.image, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL),
        barrier(clip.image, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL)};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 4, toGeneral);

    gainmap::GainmapRecordInfo ri{};
    ri.commandBuffer = cmd;
    ri.hdr = {hdr.view, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_LAYOUT_GENERAL, baseW, baseH};
    ri.sdr = {sdr.view, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_GENERAL, baseW, baseH};
    ri.map = {map.view, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_GENERAL, mapW, mapH};
    ri.params = params;
    if (clipBoostTest) {
        ri.clip = {clip.view, VK_FORMAT_R16_UINT, VK_IMAGE_LAYOUT_GENERAL, mapW, mapH};
        ri.params.clipBoost = 8.0f;
        params.clipBoost = 8.0f;  // oracle below must use the same boost
    }
    if (tsPool) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, tsPool, 0);
    gainmap.record(ri);
    if (tsPool) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, tsPool, 1);

    VkImageMemoryBarrier toCopy =
        barrier(map.image, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &toCopy);
    VkBufferImageCopy bicOut{};
    bicOut.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bicOut.imageExtent = {mapW, mapH * 2, 1};
    vkCmdCopyImageToBuffer(cmd, map.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mapStaging, 1, &bicOut);
    check(vkEndCommandBuffer(cmd), "end");
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    // NOTE: re-submitting one command buffer trips VK_NOT_READY on MoltenVK,
    // so bench timing is single-submit (wall + dispatch timestamps); average
    // across processes from the shell for stability.
    const int submits = 1;
    auto wall0 = std::chrono::steady_clock::now();
    check(vkQueueSubmit(queue, 1, &si, fence), "submit");
    check(vkWaitForFences(dev, 1, &fence, VK_TRUE, 30ull * 1000000000ull), "fence");
    auto wall1 = std::chrono::steady_clock::now();
    const double wallMs = std::chrono::duration<double, std::milli>(wall1 - wall0).count();

    double gpuMs = -1.0;
    if (tsPool) {
        uint64_t ts[2] = {0, 0};
        if (vkGetQueryPoolResults(dev, tsPool, 0, 2, sizeof(ts), ts, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
            const uint64_t mask = stampBits >= 64 ? ~uint64_t(0) : ((uint64_t(1) << stampBits) - 1u);
            gpuMs = double((ts[1] - ts[0]) & mask) * double(props.limits.timestampPeriod) * 1e-6;
        }
    }
    std::printf("device=%s stampBits=%u periodNs=%.3f base=%ux%u map=%ux%u submits=%d wallMs=%.3f gpuMs=%.3f\n",
                props.deviceName, stampBits, (double)props.limits.timestampPeriod, baseW, baseH, mapW, mapH,
                submits, wallMs, gpuMs);
    if (bench) return 0;

    check(vkMapMemory(dev, mapMem, 0, mapBytes, 0, &p), "map out");
    std::vector<uint8_t> gpu(static_cast<size_t>(mapBytes));
    std::memcpy(gpu.data(), p, size_t(mapBytes));
    vkUnmapMemory(dev, mapMem);

    // CPU oracle with matching bilinear sampling.
    auto bilinearCpu = [&](const std::vector<std::array<float, 3>>& img, uint32_t w, uint32_t h, float u, float v) {
        float stx = u * w - 0.5f, sty = v * h - 0.5f;
        float bx = std::floor(stx), by = std::floor(sty);
        float fx = std::fmin(std::fmax(stx - bx, 0.0f), 1.0f), fy = std::fmin(std::fmax(sty - by, 0.0f), 1.0f);
        int x0 = std::fmin(std::fmax(int(bx), 0), int(w) - 1), y0 = std::fmin(std::fmax(int(by), 0), int(h) - 1);
        int x1 = std::min(x0 + 1, int(w) - 1), y1 = std::min(y0 + 1, int(h) - 1);
        auto at = [&](int xx, int yy) -> std::array<float, 3> { return img[size_t(yy) * w + xx]; };
        auto c00 = at(x0, y0), c10 = at(x1, y0), c01 = at(x0, y1), c11 = at(x1, y1);
        std::array<float, 3> o{};
        for (int c = 0; c < 3; ++c) {
            float top = c00[c] * (1 - fx) + c10[c] * fx;
            float bot = c01[c] * (1 - fx) + c11[c] * fx;
            o[c] = top * (1 - fy) + bot * fy;
        }
        return o;
    };
    int maxDiff = 0;
    long long leftBad = 0, rightBad = 0;
    int worstX = 0, worstY = 0, worstC = 0;
    long long sumDiff = 0;
    long long hist[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    long long total = 0;
    for (uint32_t y = 0; y < mapH; ++y)
        for (uint32_t x = 0; x < mapW; ++x) {
            float u = (x + 0.5f) / mapW, v = (y + 0.5f) / mapH;
            auto h = bilinearCpu(hdrLin, baseW, baseH, u, v);
            auto s = bilinearCpu(sdrEnc, baseW, baseH, u, v);
            float ch[3];
            for (int k = 0; k < 3; ++k)
                ch[k] = cst[k*3]*h[0] + cst[k*3+1]*h[1] + cst[k*3+2]*h[2];
            if (params.gradeTap && gGradeTap) {
                // The sidecar carries the graded color too (tone is a scalar
                // gain and cancels in the fractions; chroma does not).
                const float e = std::fmax(params.hdrExposure, 1e-6f);
                const float lin[3] = {ch[0] * e, ch[1] * e, ch[2] * e};
                const float gain = gradeChromaGain(lin, 0.21532597f * lin[0] + 0.71600819f * lin[1] +
                                                            0.06866589f * lin[2], *gGradeTap);
                const float hy = 0.21532597f * ch[0] + 0.71600819f * ch[1] + 0.06866589f * ch[2];
                for (float& c : ch) c = hy + (c - hy) * gain;
            }
            for (float& c : ch) c = std::max(0.0f, c);
            const float sum = ch[0] + ch[1] + ch[2];
            const auto* cp = gpu.data() + size_t(mapW)*mapH*4 + (size_t(y)*mapW+x)*4;
            const unsigned cr = unsigned(cp[0]) | (unsigned(cp[1]) << 8);
            const unsigned cg = unsigned(cp[2]) | (unsigned(cp[3]) << 8);
            if (!params.writeHdrChromaticity) {
            } else if (sum > 1e-8f) {
                if (std::abs(float(cr)/65535-ch[0]/sum) > .0005f ||
                    std::abs(float(cg)/65535-ch[1]/sum) > .0005f)
                    throw std::runtime_error("capture chromaticity sidecar mismatch");
            } else if (cr != 65535 || cg != 65535) {
                throw std::runtime_error("black capture tap must be invalid");
            }

            float fh[3] = {h[0], h[1], h[2]}, fs[3] = {s[0], s[1], s[2]}, fo[3]{};
            // Channel-weighted cover over the synthetic mask (mirrors the
            // shader: bitCount/4 per texel, center maxed with the clamped 7x7
            // average so small glints drive their own fate). Ungated here:
            // cpuEncode applies the single SDR gate exactly like the shader.
            // The mask is x-split, so only x taps vary. No tap-based cover:
            // unmasked texels stay on the ratio path even when bright.
            // Flatness mirrors the shader exactly: per-channel spatial spread
            // over the texel's own 2x2 base block (clamped), maxed across
            // channels (never across channels: WB spreads channels apart),
            // <1% pinned, >8% detail.
            float cover = 0.0f;
            float flatness = 1.0f;
            {
                int bx0 = int(x) * 2, by0 = int(y) * 2;
                int bx1 = std::min(bx0 + 1, int(baseW) - 1), by1 = std::min(by0 + 1, int(baseH) - 1);
                const int xs[2] = {std::min(std::max(bx0, 0), int(baseW) - 1), bx1};
                const int ys[2] = {std::min(std::max(by0, 0), int(baseH) - 1), by1};
                float chMin[3] = {1e30f, 1e30f, 1e30f}, chMax[3] = {-1e30f, -1e30f, -1e30f};
                for (int qy = 0; qy < 2; ++qy)
                    for (int qx = 0; qx < 2; ++qx) {
                        const auto& t = hdrLin[size_t(ys[qy]) * baseW + xs[qx]];
                        for (int c = 0; c < 3; ++c) {
                            chMin[c] = std::fmin(chMin[c], t[c]);
                            chMax[c] = std::fmax(chMax[c], t[c]);
                        }
                    }
                float rel = 0.0f;
                for (int c = 0; c < 3; ++c)
                    rel = std::fmax(rel, (chMax[c] - chMin[c]) / std::fmax(chMax[c], 1e-3f));
                const float ft = std::fmin(std::fmax((rel - 0.01f) / (0.08f - 0.01f), 0.0f), 1.0f);
                flatness = 1.0f - ft * ft * (3.0f - 2.0f * ft);
            }
            if (clipBoostTest) {
                int bits = 0, center = 0;
                for (int dx = -3; dx <= 3; ++dx) {
                    int cx = std::min(std::max(int(x) + dx, 0), int(mapW) - 1);
                    // Pattern is 0xF (popcount 4) right of center, else 0.
                    int w = (cx >= int(mapW / 2)) ? 4 : 0;
                    bits += w;
                    if (dx == 0) center = w;
                }
                cover = std::fmax(float(bits) / 28.0f, float(center) / 4.0f);
            }
            cpuEncode(fh, fs, params, cst, fo, cover, flatness);
            for (int c = 0; c < 3; ++c) {
                int expect = int(std::round(fo[c] * 255));
                int got = gpu[(size_t(y) * mapW + x) * 4 + c];
                int d = std::abs(expect - got);
                // Near-black SDR is offset-dominated; allow one extra LSB there.
                int allow = toleranceLsb;
                if (s[c] < 4.0f / 255.0f) allow += 1;
                sumDiff += d;
                hist[std::min(d, 7)]++;
                total++;
                if (d > maxDiff) {
                    maxDiff = d;
                    worstX = x;
                    worstY = y;
                    worstC = c;
                }
                if (d > allow) {
                    if (x < mapW / 2) ++leftBad; else ++rightBad;
                    if (leftBad + rightBad < 8)
                        std::printf("MISMATCH x=%u y=%u c=%d expect=%d got=%d diff=%d allow=%d\n", x, y, c, expect,
                                    got, d, allow);
                }
            }
        }
    std::printf("halves: leftBad=%lld rightBad=%lld\n", leftBad, rightBad);
    if (leftBad + rightBad > 0) return 1;
    std::printf("gainmap_vulkan_validation ok cst=%s maxDiff=%d lsb at (%d,%d,c=%d) meanDiff=%.4f tol=%d\n",
                cstAp1 ? "ap1" : "identity", maxDiff, worstX, worstY, worstC,
                (double)sumDiff / (double)total, toleranceLsb);
    std::printf("hist[d0..d6,d7+] = %lld %lld %lld %lld %lld %lld %lld %lld (n=%lld)\n", hist[0], hist[1], hist[2],
                hist[3], hist[4], hist[5], hist[6], hist[7], total);
    return 0;
}
