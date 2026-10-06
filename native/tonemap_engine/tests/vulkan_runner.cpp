#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "../src/RawrNeutralTechnicalLut.h"
#include "tonemap/TonemapEngine.h"
#include "tonemap/lut/Lut3D.h"
#include "tonemap/lut/LutChain.h"
#include "tonemap/DirectRender.h"

namespace {
using tonemap::TonemapConfig;
using tonemap::TonemapCreateInfo;
using tonemap::TonemapEngine;
using tonemap::TonemapParams;
using tonemap::TonemapRecordInfo;
constexpr float AP1Y[3] = {0.2722287168f, 0.6740817658f, 0.0536895174f};
constexpr float EPS = 0.00000095367431640625f;
// Column-major non-identity camera->AP1 test matrix; catches CPU/GPU packing mistakes.
constexpr float CAM[9] = {0.92f, 0.04f, 0.01f, 0.05f, 0.94f, 0.02f, 0.03f, 0.02f, 0.97f};
void ck(VkResult r, const char* s) {
    if (r != VK_SUCCESS) throw std::runtime_error(s);
}
std::vector<uint32_t> readSpv(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open SPIR-V: " + p);
    f.seekg(0, std::ios::end);
    auto n = f.tellg();
    f.seekg(0);
    if (n <= 0 || size_t(n) % 4) throw std::runtime_error("bad SPIR-V size");
    std::vector<uint32_t> v(size_t(n) / 4);
    f.read((char*)v.data(), n);
    return v;
}
bool hasExt(const std::vector<VkExtensionProperties>& e, const char* n) {
    for (auto& x : e)
        if (std::strcmp(x.extensionName, n) == 0) return true;
    return false;
}
uint32_t memType(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    throw std::runtime_error("memory type unavailable");
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
        if (mant == 0)
            x = sign;
        else {
            int e = -14;
            while ((mant & 0x400u) == 0) {
                mant <<= 1;
                --e;
            }
            mant &= 0x3ffu;
            x = sign | (uint32_t(e + 127) << 23) | (mant << 13);
        }
    } else if (exp == 31)
        x = sign | 0x7f800000u | (mant << 13);
    else
        x = sign | (uint32_t(exp - 15 + 127) << 23) | (mant << 13);
    float f;
    std::memcpy(&f, &x, 4);
    return f;
}
struct Buf {
    VkDevice d{};
    VkBuffer b{};
    VkDeviceMemory m{};
    void* p{};
    Buf() = default;
    Buf(const Buf&) = delete;
    Buf& operator=(const Buf&) = delete;
    Buf(Buf&& o) noexcept : d(o.d), b(o.b), m(o.m), p(o.p) {
        o.d = {};
        o.b = {};
        o.m = {};
        o.p = nullptr;
    }
    ~Buf() {
        if (d) {
            if (p) vkUnmapMemory(d, m);
            if (b) vkDestroyBuffer(d, b, nullptr);
            if (m) vkFreeMemory(d, m, nullptr);
        }
    }
};
struct Img {
    VkDevice d{};
    VkImage i{};
    VkImageView v{};
    VkDeviceMemory m{};
    Img() = default;
    Img(const Img&) = delete;
    Img& operator=(const Img&) = delete;
    Img(Img&& o) noexcept : d(o.d), i(o.i), v(o.v), m(o.m) {
        o.d = {};
        o.i = {};
        o.v = {};
        o.m = {};
    }
    ~Img() {
        if (d) {
            if (v) vkDestroyImageView(d, v, nullptr);
            if (i) vkDestroyImage(d, i, nullptr);
            if (m) vkFreeMemory(d, m, nullptr);
        }
    }
};
struct Ctx {
    VkInstance inst{};
    VkPhysicalDevice pd{};
    VkDevice dev{};
    VkQueue q{};
    uint32_t qf{};
    VkCommandPool pool{};
    VkPhysicalDeviceProperties prop{};
    Ctx() = default;
    Ctx(const Ctx&) = delete;
    Ctx& operator=(const Ctx&) = delete;
    Ctx(Ctx&& o) noexcept : inst(o.inst), pd(o.pd), dev(o.dev), q(o.q), qf(o.qf), pool(o.pool), prop(o.prop) {
        o.inst = {};
        o.pd = {};
        o.dev = {};
        o.q = {};
        o.pool = {};
    }
    ~Ctx() {
        if (dev) {
            if (pool) vkDestroyCommandPool(dev, pool, nullptr);
            vkDestroyDevice(dev, nullptr);
        }
        if (inst) vkDestroyInstance(inst, nullptr);
    }
};
Ctx makeCtx() {
    Ctx c;
    uint32_t ec = 0;
    ck(vkEnumerateInstanceExtensionProperties(nullptr, &ec, nullptr), "enum instance ext");
    std::vector<VkExtensionProperties> ex(ec);
    vkEnumerateInstanceExtensionProperties(nullptr, &ec, ex.data());
    std::vector<const char*> enabled;
    VkInstanceCreateFlags flags = 0;
#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
    if (hasExt(ex, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        enabled.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
#endif
    VkApplicationInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pApplicationName = "tonemap_v1_test";
    ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.flags = flags;
    ci.pApplicationInfo = &ai;
    ci.enabledExtensionCount = uint32_t(enabled.size());
    ci.ppEnabledExtensionNames = enabled.data();
    ck(vkCreateInstance(&ci, nullptr, &c.inst), "vkCreateInstance");
    uint32_t pc = 0;
    ck(vkEnumeratePhysicalDevices(c.inst, &pc, nullptr), "enum devices");
    if (!pc) throw std::runtime_error("no Vulkan physical device");
    std::vector<VkPhysicalDevice> pds(pc);
    vkEnumeratePhysicalDevices(c.inst, &pc, pds.data());
    for (auto pd : pds) {
        uint32_t qc = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qc, nullptr);
        std::vector<VkQueueFamilyProperties> qp(qc);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qc, qp.data());
        for (uint32_t i = 0; i < qc; i++)
            if (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                c.pd = pd;
                c.qf = i;
                break;
            }
        if (c.pd) break;
    }
    if (!c.pd) throw std::runtime_error("no compute queue");
    vkGetPhysicalDeviceProperties(c.pd, &c.prop);
    uint32_t dc = 0;
    vkEnumerateDeviceExtensionProperties(c.pd, nullptr, &dc, nullptr);
    std::vector<VkExtensionProperties> de(dc);
    vkEnumerateDeviceExtensionProperties(c.pd, nullptr, &dc, de.data());
    std::vector<const char*> dex;
#ifdef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
    if (hasExt(de, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME)) dex.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
#endif
    float pri = 1;
    VkDeviceQueueCreateInfo qi{};
    qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = c.qf;
    qi.queueCount = 1;
    qi.pQueuePriorities = &pri;
    VkDeviceCreateInfo di{};
    di.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = uint32_t(dex.size());
    di.ppEnabledExtensionNames = dex.data();
    ck(vkCreateDevice(c.pd, &di, nullptr, &c.dev), "vkCreateDevice");
    vkGetDeviceQueue(c.dev, c.qf, 0, &c.q);
    VkCommandPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = c.qf;
    ck(vkCreateCommandPool(c.dev, &pi, nullptr, &c.pool), "command pool");
    return c;
}
Buf makeBuf(Ctx& c, VkDeviceSize n, VkBufferUsageFlags usage) {
    Buf x;
    x.d = c.dev;
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = n;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ck(vkCreateBuffer(c.dev, &bi, nullptr, &x.b), "buffer");
    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(c.dev, x.b, &mr);
    VkMemoryAllocateInfo ma{};
    ma.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ma.allocationSize = mr.size;
    ma.memoryTypeIndex =
        memType(c.pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    ck(vkAllocateMemory(c.dev, &ma, nullptr, &x.m), "buffer memory");
    ck(vkBindBufferMemory(c.dev, x.b, x.m, 0), "bind buffer");
    ck(vkMapMemory(c.dev, x.m, 0, n, 0, &x.p), "map buffer");
    return x;
}
Img makeImg(Ctx& c, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage) {
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(c.pd, fmt, &fp);
    if ((fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) == 0)
        throw std::runtime_error("required storage-image format unsupported");
    Img x;
    x.d = c.dev;
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent = {w, h, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ck(vkCreateImage(c.dev, &ii, nullptr, &x.i), "image");
    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(c.dev, x.i, &mr);
    VkMemoryAllocateInfo ma{};
    ma.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ma.allocationSize = mr.size;
    try {
        ma.memoryTypeIndex = memType(c.pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    } catch (...) {
        ma.memoryTypeIndex = memType(c.pd, mr.memoryTypeBits, 0);
    }
    ck(vkAllocateMemory(c.dev, &ma, nullptr, &x.m), "image memory");
    ck(vkBindImageMemory(c.dev, x.i, x.m, 0), "bind image");
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = x.i;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    ck(vkCreateImageView(c.dev, &vi, nullptr, &x.v), "view");
    return x;
}
VkCommandBuffer cmd(Ctx& c) {
    VkCommandBufferAllocateInfo a{};
    a.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    a.commandPool = c.pool;
    a.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    a.commandBufferCount = 1;
    VkCommandBuffer x{};
    ck(vkAllocateCommandBuffers(c.dev, &a, &x), "cmd alloc");
    return x;
}
void submit(Ctx& c, VkCommandBuffer b) {
    ck(vkEndCommandBuffer(b), "cmd end");
    VkSubmitInfo s{};
    s.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    s.commandBufferCount = 1;
    s.pCommandBuffers = &b;
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence f{};
    ck(vkCreateFence(c.dev, &fi, nullptr, &f), "fence");
    ck(vkQueueSubmit(c.q, 1, &s, f), "submit");
    ck(vkWaitForFences(c.dev, 1, &f, VK_TRUE, UINT64_MAX), "wait");
    vkDestroyFence(c.dev, f, nullptr);
    vkFreeCommandBuffers(c.dev, c.pool, 1, &b);
}
void begin(VkCommandBuffer b) {
    VkCommandBufferBeginInfo i{};
    i.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    i.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ck(vkBeginCommandBuffer(b, &i), "cmd begin");
}
void imageBarrier(VkCommandBuffer b, VkImage im, VkImageLayout oldL, VkImageLayout newL, VkAccessFlags src,
                  VkAccessFlags dst, VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
    VkImageMemoryBarrier x{};
    x.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    x.srcAccessMask = src;
    x.dstAccessMask = dst;
    x.oldLayout = oldL;
    x.newLayout = newL;
    x.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    x.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    x.image = im;
    x.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    x.subresourceRange.levelCount = 1;
    x.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(b, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &x);
}
struct Pixel {
    float r, g, b;
};
Pixel synth(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    float u = w > 1 ? float(x) / float(w - 1) : 0, v = h > 1 ? float(y) / float(h - 1) : 0;
    float s = .18f * std::exp2(-8.f + 14.f * u);
    switch ((y * 8u) / std::max(1u, h)) {
        case 0:
            return {s, s, s};
        case 1:
            return {s, 0.35f * s, 0.05f * s};
        case 2:
            return {.05f * s, .8f * s, s};
        case 3:
            return {s, .05f * s, .8f * s};
        case 4:
            return {.8f * s, .45f * s, .25f * s};
        case 5:
            return {.05f * s, .05f * s, s};
        case 6:
            return {(u < .5f ? 1.f : .05f) * s, (u < .5f ? .05f : 1.f) * s, .05f * s};
        default:
            return {(v - .5f) * .2f + s, .6f * s, 1.2f * s};
    }
}
Pixel synthGamut(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    float u = w > 1 ? float(x) / float(w - 1) : 0;
    float v = h > 1 ? float(y) / float(h - 1) : 0;
    float e = .18f * std::exp2(1.f + 7.f * u);
    uint32_t band = (y * 6u) / std::max(1u, h);
    switch (band) {
        case 0:
            return {e, .03f * e, .02f * e};
        case 1:
            return {e, .22f * e, .03f * e};
        case 2:
            return {.02f * e, .9f * e, e};
        case 3:
            return {.03f * e, .12f * e, e};
        case 4:
            return {e, .03f * e, .75f * e};
        default:
            return {(.2f + .8f * v) * e, (1.f - .7f * v) * e, .05f * e};
    }
}

struct V3 {
    float x, y, z;
};

float dither(uint32_t x, uint32_t y) {
    float xf = float(x), yf = float(y);
    auto fract = [](float v) { return v - std::floor(v); };
    float u = fract(52.9829189f * fract(0.06711056f * xf + 0.00583715f * yf));
    return (u - .5f) * (1.f / 255.f);
}

float diCpu(float x) {
    constexpr float A = .0075f, B = 7.f, C = .07329248f, M = 10.44426855f, CUT = .00262409f;
    return x <= CUT ? x * M : (std::log2(x + A) + B) * C;
}
V3 ap1LinearToDwgDi(V3 x) {
    // Same AP1->DWG column-major matrix and DaVinci Intermediate encode as cst.glsl.
    V3 d{.91483184f * x.x + .00441859f * x.y + .08080475f * x.z, .03081775f * x.x + .78448185f * x.y + .18465208f * x.z,
         .06743513f * x.x + .08296681f * x.y + .84942500f * x.z};
    return {diCpu(d.x), diCpu(d.y), diCpu(d.z)};
}
V3 lutFetch(int r, int g, int b) {
    constexpr int N = int(tonemap::rawr_neutral_technical::kSize);
    size_t i = size_t(r + N * (g + N * b)) * 4;
    const auto& a = tonemap::rawr_neutral_technical::kRgba;
    return {a[i], a[i + 1], a[i + 2]};
}
V3 lutSample(V3 q) {
    constexpr int N = int(tonemap::rawr_neutral_technical::kSize);
    float z[3] = {std::clamp(q.x, 0.f, 1.f) * (N - 1), std::clamp(q.y, 0.f, 1.f) * (N - 1),
                  std::clamp(q.z, 0.f, 1.f) * (N - 1)};
    int i0[3] = {int(std::floor(z[0])), int(std::floor(z[1])), int(std::floor(z[2]))};
    int i1[3] = {std::min(i0[0] + 1, N - 1), std::min(i0[1] + 1, N - 1), std::min(i0[2] + 1, N - 1)};
    float f[3] = {z[0] - i0[0], z[1] - i0[1], z[2] - i0[2]};
    auto add = [](V3 a, V3 b, float t) { return V3{a.x + t * b.x, a.y + t * b.y, a.z + t * b.z}; };
    auto sub = [](V3 a, V3 b) { return V3{a.x - b.x, a.y - b.y, a.z - b.z}; };
    V3 c000 = lutFetch(i0[0], i0[1], i0[2]), o{};
    if (f[0] >= f[1]) {
        if (f[1] >= f[2]) {
            auto c100 = lutFetch(i1[0], i0[1], i0[2]), c110 = lutFetch(i1[0], i1[1], i0[2]),
                 c111 = lutFetch(i1[0], i1[1], i1[2]);
            o = add(add(add(c000, sub(c100, c000), f[0]), sub(c110, c100), f[1]), sub(c111, c110), f[2]);
        } else if (f[0] >= f[2]) {
            auto c100 = lutFetch(i1[0], i0[1], i0[2]), c101 = lutFetch(i1[0], i0[1], i1[2]),
                 c111 = lutFetch(i1[0], i1[1], i1[2]);
            o = add(add(add(c000, sub(c100, c000), f[0]), sub(c101, c100), f[2]), sub(c111, c101), f[1]);
        } else {
            auto c001 = lutFetch(i0[0], i0[1], i1[2]), c101 = lutFetch(i1[0], i0[1], i1[2]),
                 c111 = lutFetch(i1[0], i1[1], i1[2]);
            o = add(add(add(c000, sub(c001, c000), f[2]), sub(c101, c001), f[0]), sub(c111, c101), f[1]);
        }
    } else {
        if (f[2] >= f[1]) {
            auto c001 = lutFetch(i0[0], i0[1], i1[2]), c011 = lutFetch(i0[0], i1[1], i1[2]),
                 c111 = lutFetch(i1[0], i1[1], i1[2]);
            o = add(add(add(c000, sub(c001, c000), f[2]), sub(c011, c001), f[1]), sub(c111, c011), f[0]);
        } else if (f[2] >= f[0]) {
            auto c010 = lutFetch(i0[0], i1[1], i0[2]), c011 = lutFetch(i0[0], i1[1], i1[2]),
                 c111 = lutFetch(i1[0], i1[1], i1[2]);
            o = add(add(add(c000, sub(c010, c000), f[1]), sub(c011, c010), f[2]), sub(c111, c011), f[0]);
        } else {
            auto c010 = lutFetch(i0[0], i1[1], i0[2]), c110 = lutFetch(i1[0], i1[1], i0[2]),
                 c111 = lutFetch(i1[0], i1[1], i1[2]);
            o = add(add(add(c000, sub(c010, c000), f[1]), sub(c110, c010), f[0]), sub(c111, c110), f[2]);
        }
    }
    return {std::clamp(o.x, 0.f, 1.f), std::clamp(o.y, 0.f, 1.f), std::clamp(o.z, 0.f, 1.f)};
}
std::array<float, 3> cpu(Pixel in, const TonemapParams& p, const TonemapConfig& cfg, uint32_t px, uint32_t py, uint32_t bits = 8) {
    float w[3] = {CAM[0] * in.r + CAM[3] * in.g + CAM[6] * in.b, CAM[1] * in.r + CAM[4] * in.g + CAM[7] * in.b,
                  CAM[2] * in.r + CAM[5] * in.g + CAM[8] * in.b};
    if (p.renderTransform != tonemap::RenderTransform::Existing) {
        auto encoded = tonemap::renderDirectReference({w[0], w[1], w[2]}, p);
        for (float& v : encoded) v = std::clamp(v + dither(px, py) * (bits == 10 ? 255.0f / 1023.0f : 1.0f), 0.0f, 1.0f);
        return encoded;
    }
    float yt = w[0] * AP1Y[0] + w[1] * AP1Y[1] + w[2] * AP1Y[2];
    for (int i = 0; i < 3; i++) w[i] = yt + p.saturation * (w[i] - yt);
    float span = std::max(w[0], std::max(w[1], w[2])) - std::min(w[0], std::min(w[1], w[2]));
    float peak = std::max(std::abs(w[0]), std::max(std::abs(w[1]), std::abs(w[2])));
    float proxy = std::clamp(span / std::max(peak, EPS), 0.f, 1.f);
    float sel = (1 - proxy) * (1 - proxy);
    float tt = std::clamp((yt - cfg.vibranceShadowStart) / (cfg.vibranceShadowEnd - cfg.vibranceShadowStart), 0.f, 1.f);
    float guard = tt * tt * (3 - 2 * tt);
    float vg = 1 + std::clamp(p.vibrance, -1.f, 1.f) * cfg.vibranceStrength * sel * guard;
    for (int i = 0; i < 3; i++) w[i] = yt + vg * (w[i] - yt);
    float gain = std::exp2(std::log2(p.aePostGain) + p.exposureEV);
    V3 di = ap1LinearToDwgDi({w[0] * gain, w[1] * gain, w[2] * gain});
    V3 enc = lutSample(di);
    float dn = dither(px, py);
    return {std::clamp(enc.x + dn, 0.f, 1.f), std::clamp(enc.y + dn, 0.f, 1.f), std::clamp(enc.z + dn, 0.f, 1.f)};
}

void writePPM(const std::string& p, const uint8_t* rgba, uint32_t w, uint32_t h) {
    std::ofstream f(p, std::ios::binary);
    f << "P6\n" << w << " " << h << "\n255\n";
    for (size_t i = 0; i < size_t(w) * h; i++) f.write((const char*)rgba + 4 * i, 3);
}

struct ProducerPipeline {
    VkDevice d{};
    VkDescriptorSetLayout dsl{};
    VkDescriptorPool dp{};
    VkDescriptorSet ds{};
    VkPipelineLayout pl{};
    VkPipeline pipe{};
    ProducerPipeline() = default;
    ProducerPipeline(const ProducerPipeline&) = delete;
    ProducerPipeline& operator=(const ProducerPipeline&) = delete;
    ProducerPipeline(ProducerPipeline&& o) noexcept : d(o.d), dsl(o.dsl), dp(o.dp), ds(o.ds), pl(o.pl), pipe(o.pipe) {
        o.d = {};
        o.dsl = {};
        o.dp = {};
        o.ds = {};
        o.pl = {};
        o.pipe = {};
    }
    ~ProducerPipeline() {
        if (d) {
            if (pipe) vkDestroyPipeline(d, pipe, nullptr);
            if (pl) vkDestroyPipelineLayout(d, pl, nullptr);
            if (dp) vkDestroyDescriptorPool(d, dp, nullptr);
            if (dsl) vkDestroyDescriptorSetLayout(d, dsl, nullptr);
        }
    }
};
ProducerPipeline makeProducer(Ctx& c, const std::vector<uint32_t>& spv, VkImageView view) {
    ProducerPipeline x;
    x.d = c.dev;
    VkDescriptorSetLayoutBinding lb{};
    lb.binding = 0;
    lb.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    lb.descriptorCount = 1;
    lb.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 1;
    li.pBindings = &lb;
    ck(vkCreateDescriptorSetLayout(c.dev, &li, nullptr, &x.dsl), "producer descriptor layout");
    VkDescriptorPoolSize ps{};
    ps.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    ps.descriptorCount = 1;
    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.maxSets = 1;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    ck(vkCreateDescriptorPool(c.dev, &dpi, nullptr, &x.dp), "producer descriptor pool");
    VkDescriptorSetAllocateInfo dai{};
    dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dai.descriptorPool = x.dp;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &x.dsl;
    ck(vkAllocateDescriptorSets(c.dev, &dai, &x.ds), "producer descriptor set");
    VkDescriptorImageInfo ii{};
    ii.imageView = view;
    ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet wd{};
    wd.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wd.dstSet = x.ds;
    wd.dstBinding = 0;
    wd.descriptorCount = 1;
    wd.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    wd.pImageInfo = &ii;
    vkUpdateDescriptorSets(c.dev, 1, &wd, 0, nullptr);
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &x.dsl;
    ck(vkCreatePipelineLayout(c.dev, &pli, nullptr, &x.pl), "producer pipeline layout");
    VkShaderModuleCreateInfo smi{};
    smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smi.codeSize = spv.size() * sizeof(uint32_t);
    smi.pCode = spv.data();
    VkShaderModule sm{};
    ck(vkCreateShaderModule(c.dev, &smi, nullptr, &sm), "producer shader module");
    VkComputePipelineCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pci.stage.module = sm;
    pci.stage.pName = "main";
    pci.layout = x.pl;
    VkResult r = vkCreateComputePipelines(c.dev, VK_NULL_HANDLE, 1, &pci, nullptr, &x.pipe);
    vkDestroyShaderModule(c.dev, sm, nullptr);
    ck(r, "producer pipeline");
    return x;
}
void recordProducer(VkCommandBuffer b, const ProducerPipeline& p, uint32_t w, uint32_t h, uint32_t lx, uint32_t ly) {
    vkCmdBindPipeline(b, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipe);
    vkCmdBindDescriptorSets(b, VK_PIPELINE_BIND_POINT_COMPUTE, p.pl, 0, 1, &p.ds, 0, nullptr);
    vkCmdDispatch(b, (w + lx - 1) / lx, (h + ly - 1) / ly, 1);
}
struct Stats {
    double best{}, median{}, mean{}, p95{}, p99{}, worst{};
};
Stats statsOf(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    Stats s{};
    s.best = v.front();
    s.worst = v.back();
    s.mean = std::accumulate(v.begin(), v.end(), 0.0) / v.size();
    auto pct = [&](double q) { return v[size_t(std::floor(q * (v.size() - 1)))]; };
    s.median = pct(.5);
    s.p95 = pct(.95);
    s.p99 = pct(.99);
    return s;
}
void printStats(const char* name, const Stats& s) {
    std::cout << name << " best=" << s.best << " median=" << s.median << " mean=" << s.mean << " p95=" << s.p95
              << " p99=" << s.p99 << " worst=" << s.worst << "\n";
}
struct Args {
    std::string spv, out, inputFile, rawOutput, referenceShader;
    std::array<float, 9> cameraMatrix{{CAM[0], CAM[1], CAM[2], CAM[3], CAM[4], CAM[5], CAM[6], CAM[7], CAM[8]}};
    uint32_t outputBits = 8, renderTransform = 0, outputGamut = 0, outputTransfer = 1;
    bool neutralTexture = false, userTexture = false, videoMonitor = false;
    float highlightCompression = 0, highlightGain = 1, exposureEV = 0;
    // Blacks, Shadows, Contrast, Midtones, Highlights, Whites, Saturation, Vibrance (UI -100..+100).
    std::array<float, 8> tone{};
    bool toneSet = false;
    tonemap::color::ColorSpace lutInput{tonemap::color::Gamut::DaVinciWideGamut, tonemap::color::TransferFunction::DaVinciIntermediate};
    tonemap::color::ColorSpace lutOutput{tonemap::color::Gamut::SRgbRec709, tonemap::color::TransferFunction::SRgb};
    tonemap::lut::LutPlacement placement = tonemap::lut::LutPlacement::RenderTransform;
    tonemap::lut::LutAfterAction afterAction = tonemap::lut::LutAfterAction::ConvertToSrgb;
    float intensity = 1;
    std::vector<std::string> lutFiles;
    uint32_t lx = 16, ly = 8, w = 257, h = 129, it = 200, warm = 30;
    bool validate = true, bench = true, gamutStress = false, colorStress = false, controlsStress = false,
         postGainSequence = false, handoffBench = false;
    std::string producerSpv;
    float aePostGain = 1.0f;
};
Args args(int ac, char** av) {
    Args a;
    for (int i = 1; i < ac; i++) {
        std::string k = av[i];
        auto val = [&]() {
            if (++i >= ac) throw std::runtime_error("missing arg");
            return std::string(av[i]);
        };
        if (k == "--shader")
            a.spv = val();
        else if (k == "--local-x")
            a.lx = std::stoul(val());
        else if (k == "--local-y")
            a.ly = std::stoul(val());
        else if (k == "--width")
            a.w = std::stoul(val());
        else if (k == "--height")
            a.h = std::stoul(val());
        else if (k == "--iterations")
            a.it = std::stoul(val());
        else if (k == "--warmup")
            a.warm = std::stoul(val());
        else if (k == "--output")
            a.out = val();
        else if (k == "--validate-only")
            a.bench = false;
        else if (k == "--bench-only")
            a.validate = false;
        else if (k == "--gamut-stress")
            a.gamutStress = true;
        else if (k == "--color-stress")
            a.colorStress = true;
        else if (k == "--controls-stress")
            a.controlsStress = true;
        else if (k == "--handoff-bench") {
            a.handoffBench = true;
            a.validate = false;
            a.bench = false;
        } else if (k == "--producer-shader")
            a.producerSpv = val();
        else if (k == "--reference-shader") { a.referenceShader = val(); a.validate = false; }
        else if (k == "--input-rgba16f") a.inputFile = val();
        else if (k == "--dump-output") a.rawOutput = val();
        else if (k == "--output-bits") a.outputBits = std::stoul(val());
        else if (k == "--user-texture") { a.userTexture = true; a.neutralTexture = true; }
        else if (k == "--neutral-texture") a.neutralTexture = true;
        else if (k == "--video-monitor") a.videoMonitor = true;
        else if (k == "--highlight-compression") a.highlightCompression = std::stof(val());
        else if (k == "--highlight-gain") a.highlightGain = std::stof(val());
        else if (k == "--exposure-ev") a.exposureEV = std::stof(val());
        else if (k == "--tone") {
            std::istringstream values(val());
            for (size_t n = 0; n < a.tone.size(); ++n) {
                if (!(values >> a.tone[n]) || !std::isfinite(a.tone[n]) || std::fabs(a.tone[n]) > 100.0f)
                    throw std::runtime_error("--tone needs eight comma-separated values in -100..100");
                if (n != 7 && values.get() != ',') throw std::runtime_error("--tone needs eight comma-separated values");
            }
            a.toneSet = true;
        }
        else if (k == "--render-transform") a.renderTransform = std::stoul(val());
        else if (k == "--output-gamut") a.outputGamut = std::stoul(val());
        else if (k == "--output-transfer") a.outputTransfer = std::stoul(val());
        else if (k == "--lut-input-gamut") a.lutInput.gamut = static_cast<tonemap::color::Gamut>(std::stoul(val()));
        else if (k == "--lut-input-transfer") a.lutInput.transfer = static_cast<tonemap::color::TransferFunction>(std::stoul(val()));
        else if (k == "--lut-output-gamut") a.lutOutput.gamut = static_cast<tonemap::color::Gamut>(std::stoul(val()));
        else if (k == "--lut-output-transfer") a.lutOutput.transfer = static_cast<tonemap::color::TransferFunction>(std::stoul(val()));
        else if (k == "--lut-placement") a.placement = static_cast<tonemap::lut::LutPlacement>(std::stoul(val()));
        else if (k == "--lut-after-action") a.afterAction = static_cast<tonemap::lut::LutAfterAction>(std::stoul(val()));
        else if (k == "--lut-intensity") a.intensity = std::stof(val());
        else if (k == "--camera-matrix") {
            std::istringstream values(val());
            for (size_t n = 0; n < a.cameraMatrix.size(); ++n) {
                if (!(values >> a.cameraMatrix[n]) || !std::isfinite(a.cameraMatrix[n])) throw std::runtime_error("invalid camera matrix");
                if (n != 8 && values.get() != ',') throw std::runtime_error("camera matrix needs nine comma-separated column-major floats");
            }
            if (values.peek() != std::char_traits<char>::eof()) throw std::runtime_error("extra camera matrix values");
        }
        else if (k == "--lut")
            a.lutFiles.push_back(val());
        else if (k == "--ae-post-gain")
            a.aePostGain = std::stof(val());
        else if (k == "--ae-post-gain-sequence") {
            a.postGainSequence = true;
            a.validate = false;
            a.bench = false;
        } else
            throw std::runtime_error("unknown arg " + k);
    }
    if (a.spv.empty()) throw std::runtime_error("--shader required");
    if (a.handoffBench && a.producerSpv.empty())
        throw std::runtime_error("--producer-shader required with --handoff-bench");
    if (!a.w || !a.h || !a.it || (a.outputBits != 8 && a.outputBits != 10 && a.outputBits != 16 && a.outputBits != 32))
        throw std::runtime_error("invalid extent, iterations, or output bits");
    if (uint32_t(a.lutInput.gamut) > 7 || uint32_t(a.lutOutput.gamut) > 7 ||
        uint32_t(a.lutInput.transfer) > 10 || uint32_t(a.lutOutput.transfer) > 10 || uint32_t(a.placement) > 1 ||
        uint32_t(a.afterAction) > 1 || !std::isfinite(a.intensity) || a.intensity < 0 || a.intensity > 1)
        throw std::runtime_error("invalid LUT configuration");
    if (a.outputBits == 10 && !a.videoMonitor) throw std::runtime_error("10-bit shader requires --video-monitor");
    if (a.validate && (!a.inputFile.empty() || !a.lutFiles.empty() || a.toneSet ||
        (a.renderTransform == 0 && (a.outputBits != 8 || a.videoMonitor)) ||
        (a.renderTransform != 0 && a.outputBits != 8 && a.outputBits != 10)))
        throw std::runtime_error("CPU oracle is synthetic Neutral RGBA8 only; use --bench-only and compare GPU dumps");
    return a;
}
}  // namespace
int main(int argc, char** argv) {
    try {
        auto a = args(argc, argv);
        auto c = makeCtx();
        std::cout << "GPU " << c.prop.deviceName << "\n";
        std::cout << "WORKGROUP " << a.lx << "x" << a.ly << " SIZE " << a.w << "x" << a.h
                  << (a.gamutStress ? " GAMUT_STRESS" : (a.colorStress ? " COLOR_STRESS" : " NORMAL")) << "\n";
        auto spv = readSpv(a.spv);
        auto input = makeImg(c, a.w, a.h, VK_FORMAT_R16G16B16A16_SFLOAT,
                             VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        const VkFormat outputFormat = a.outputBits == 32 ? VK_FORMAT_R32G32B32A32_SFLOAT :
            a.outputBits == 16 ? VK_FORMAT_R16G16B16A16_SFLOAT :
            a.outputBits == 10 ? VK_FORMAT_A2B10G10R10_UNORM_PACK32 : VK_FORMAT_R8G8B8A8_UNORM;
        const size_t outputStride = a.outputBits == 32 ? 16 : a.outputBits == 16 ? 8 : 4;
        auto output = makeImg(c, a.w, a.h, outputFormat,
                              VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        size_t n = size_t(a.w) * a.h;
        auto up = makeBuf(c, n * 8, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        auto monitor = a.videoMonitor ? makeImg(c, a.w, a.h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT) : Img{};
        auto down = makeBuf(c, n * outputStride, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        auto* hp = (uint16_t*)up.p;
        std::vector<Pixel> quant(n);
        for (uint32_t y = 0; y < a.h; y++)
            for (uint32_t x = 0; x < a.w; x++) {
                auto p = a.gamutStress ? synthGamut(x, y, a.w, a.h) : synth(x, y, a.w, a.h);
                size_t j = size_t(y) * a.w + x;
                float vals[4] = {p.r, p.g, p.b, 1};
                for (int q = 0; q < 4; q++) {
                    hp[4 * j + q] = f2h(vals[q]);
                }
                quant[j] = {h2f(hp[4 * j]), h2f(hp[4 * j + 1]), h2f(hp[4 * j + 2])};
            }
        if (!a.inputFile.empty()) {
            std::ifstream file(a.inputFile, std::ios::binary | std::ios::ate);
            if (!file || file.tellg() != std::streamoff(n * 8)) throw std::runtime_error("RGBA16F input size mismatch");
            file.seekg(0);
            if (!file.read(reinterpret_cast<char*>(hp), n * 8)) throw std::runtime_error("RGBA16F input read failed");
        }
        {
            auto b = cmd(c);
            begin(b);
            imageBarrier(b, input.i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                         VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy cp{};
            cp.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            cp.imageSubresource.layerCount = 1;
            cp.imageExtent = {a.w, a.h, 1};
            vkCmdCopyBufferToImage(b, up.b, input.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
            imageBarrier(b, input.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                         VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            imageBarrier(b, output.i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            if (a.videoMonitor) imageBarrier(b, monitor.i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
                VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            submit(c, b);
        }
        tonemap::lut::LutChain userChain{};
        if (!a.lutFiles.empty()) {
            userChain.inputSpace = a.lutInput;
            userChain.outputSpace = a.lutOutput;
            userChain.placement = a.placement;
            userChain.afterAction = a.afterAction;
            userChain.intensity = a.intensity;
            for (const auto& path : a.lutFiles) userChain.stages.push_back(tonemap::lut::parseCubeFile(path));
        }
        TonemapCreateInfo ci{};
        ci.context.physicalDevice = c.pd;
        ci.context.device = c.dev;
        ci.shaderSpirv = spv.data();
        ci.shaderSpirvBytes = spv.size() * 4;
        ci.workgroupSizeX = a.lx;
        ci.workgroupSizeY = a.ly;
        ci.maxFramesInFlight = 1;
        ci.outputFormat = outputFormat;
        ci.neutralLutTexture = a.neutralTexture;
        ci.userLutTexture = a.userTexture;
        ci.lutUploadQueue = c.q;
        ci.lutUploadQueueFamily = c.qf;
        ci.videoMonitorOutput = a.videoMonitor;
        if (!a.lutFiles.empty()) ci.lutChain = &userChain;
        TonemapEngine engine(ci);
        std::vector<uint32_t> referenceWords;
        std::unique_ptr<TonemapEngine> referenceEngine;
        if (!a.referenceShader.empty()) {
            referenceWords = readSpv(a.referenceShader);
            auto referenceInfo = ci;
            referenceInfo.shaderSpirv = referenceWords.data();
            referenceInfo.shaderSpirvBytes = referenceWords.size() * sizeof(uint32_t);
            referenceInfo.neutralLutTexture = false;
            referenceInfo.userLutTexture = false;
            referenceEngine = std::make_unique<TonemapEngine>(referenceInfo);
        }
        TonemapParams p = tonemap::TonemapPresets::NeutralBaseline().params;
        const TonemapConfig cfg = ci.config;
        p.aePostGain = a.aePostGain;
        p.exposureEV = a.exposureEV;
        p.renderTransform = static_cast<tonemap::RenderTransform>(a.renderTransform);
        p.outputSpace = {static_cast<tonemap::color::Gamut>(a.outputGamut), static_cast<tonemap::color::TransferFunction>(a.outputTransfer)};
        if (a.colorStress) {
            p.vibrance = 100.0f;
            p.saturation = 35.0f;
        }
        if (a.toneSet) {
            p.blackPointEV = a.tone[0];
            p.shadowLiftEV = a.tone[1];
            p.contrast = a.tone[2];
            p.midtoneLiftEV = a.tone[3];
            p.highlightBiasEV = a.tone[4];
            p.whitePointEV = a.tone[5];
            p.saturation = a.tone[6];
            p.vibrance = a.tone[7];
        }
        if (a.controlsStress) {
            p.blackPointEV = -35.0f;
            p.shadowLiftEV = 30.0f;
            p.midtoneLiftEV = 20.0f;
            p.contrast = 25.0f;
            p.whitePointEV = 25.0f;
            p.highlightBiasEV = 30.0f;
            p.saturation = 25.0f;
            p.vibrance = 35.0f;
        }
        TonemapRecordInfo ri{};
        ri.input.view = input.v;
        ri.input.width = a.w;
        ri.input.height = a.h;
        ri.output.view = output.v;
        ri.output.width = a.w;
        ri.output.height = a.h;
        ri.frameSlot = 0;
        ri.output.format = outputFormat;
        if (a.videoMonitor) {
            ri.monitor = {monitor.v, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_GENERAL, a.w, a.h};
            ri.monitorEnabled = true;
        }
        ri.highlightCompression = a.highlightCompression;
        ri.highlightExposureGain = a.highlightGain;
        ri.cameraToWorkingColumnMajor3x3 = a.cameraMatrix.data();
        ri.params = p;
        auto renderReadback = [&](TonemapEngine& renderer) {
            auto b = cmd(c);
            begin(b);
            ri.commandBuffer = b;
            renderer.record(ri);
            imageBarrier(b, output.i, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy cp{};
            cp.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            cp.imageSubresource.layerCount = 1;
            cp.imageExtent = {a.w, a.h, 1};
            vkCmdCopyImageToBuffer(b, output.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, down.b, 1, &cp);
            imageBarrier(b, output.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                         VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            submit(c, b);
        };
        auto validateFrame = [&](const char* passLabel) {
            std::vector<uint8_t> reference;
            if (referenceEngine) {
                renderReadback(*referenceEngine);
                reference.resize(n * outputStride);
                std::memcpy(reference.data(), down.p, reference.size());
            }
            renderReadback(engine);
            if (referenceEngine) {
                double maximum = 0, sum = 0;
                size_t bad = 0, changed = 0;
                for (size_t i = 0; i < n * 4; ++i) {
                    double ref, actual;
                    if (a.outputBits == 32) {
                        float x, y;
                        std::memcpy(&x, reference.data() + i * 4, 4);
                        std::memcpy(&y, static_cast<const char*>(down.p) + i * 4, 4);
                        ref = x; actual = y;
                    } else if (a.outputBits == 16) {
                        uint16_t x, y;
                        std::memcpy(&x, reference.data() + i * 2, 2);
                        std::memcpy(&y, static_cast<const char*>(down.p) + i * 2, 2);
                        ref = h2f(x); actual = h2f(y);
                    } else if (a.outputBits == 10) {
                        uint32_t x, y;
                        std::memcpy(&x, reference.data() + (i / 4) * 4, 4);
                        std::memcpy(&y, static_cast<const char*>(down.p) + (i / 4) * 4, 4);
                        const uint32_t shift = uint32_t(i % 4) * 10, mask = i % 4 == 3 ? 3 : 1023;
                        ref = (x >> shift) & mask; actual = (y >> shift) & mask;
                    } else {
                        ref = reference[i]; actual = static_cast<const uint8_t*>(down.p)[i];
                    }
                    const double delta = std::abs(actual - ref);
                    maximum = std::max(maximum, delta); sum += delta;
                    if (delta != 0) ++changed;
                    const double tolerance = a.outputBits == 32 ? 1e-5 + 1e-5 * std::abs(ref) :
                        a.outputBits == 16 ? 1.0 / 1023.0 : 1.0;
                    if (!std::isfinite(ref) || !std::isfinite(actual) || delta > tolerance) ++bad;
                }
                std::cout << "REFERENCE_PARITY outputBits=" << a.outputBits << " max=" << maximum
                          << " mean=" << sum / (n * 4) << " changed=" << changed << " bad=" << bad << "\n";
                if (bad) throw std::runtime_error("original shader parity failed");
                std::cout << "REFERENCE_PARITY_PASS\n";
            }
            if (!a.rawOutput.empty()) {
                std::ofstream file(a.rawOutput, std::ios::binary);
                if (!file || !file.write(static_cast<const char*>(down.p), n * outputStride)) throw std::runtime_error("output dump failed");
            }
            auto* got = (uint8_t*)down.p;
            if (!a.validate && !a.postGainSequence) {
                if (!a.out.empty() && a.outputBits == 8) writePPM(a.out, got, a.w, a.h);
                return;
            }
            int maxe = 0;
            double mae = 0;
            size_t bad = 0;
            int edgeMax = 0;
            size_t edgeBad = 0;
            for (size_t i = 0; i < n; i++) {
                uint32_t px = uint32_t(i % a.w), py = uint32_t(i / a.w);
                auto ref = cpu(quant[i], ri.params, cfg, px, py, a.outputBits);
                const bool edge = (px + 1 == a.w) || (py + 1 == a.h);
                for (int q = 0; q < 3; q++) {
                    const int maxCode = a.outputBits == 10 ? 1023 : 255;
                    int ex = int(std::lround(std::clamp(ref[q], 0.f, 1.f) * maxCode));
                    uint32_t packed = 0;
                    if (a.outputBits == 10) std::memcpy(&packed, got + 4 * i, 4);
                    int actual = a.outputBits == 10 ? int((packed >> (q * 10)) & 1023u) : int(got[4 * i + q]);
                    int e = std::abs(actual - ex);
                    maxe = std::max(maxe, e);
                    mae += e;
                    if (e > 2) bad++;
                    if (edge) {
                        edgeMax = std::max(edgeMax, e);
                        if (e > 2) edgeBad++;
                    }
                }
                if (a.outputBits == 8 && got[4 * i + 3] != 255) {
                    int e = std::abs(int(got[4 * i + 3]) - 255);
                    maxe = std::max(maxe, e);
                    bad++;
                    if (edge) {
                        edgeMax = std::max(edgeMax, e);
                        edgeBad++;
                    }
                }
            }
            mae /= double(n * 3);
            std::cout << "PARITY aePostGain=" << ri.params.aePostGain << " max_lsb=" << maxe << " mean_lsb=" << mae
                      << " bad_gt2=" << bad << "\n";
            std::cout << "EDGE_PARITY right_and_bottom max_lsb=" << edgeMax << " bad_gt2=" << edgeBad << "\n";
            if (edgeMax > 3 || edgeBad > 0) throw std::runtime_error("edge parity gate failed");
            std::cout << "EDGE_PARITY_PASS\n";
            if (!a.out.empty()) writePPM(a.out, got, a.w, a.h);
            if (maxe > 3 || bad > std::max<size_t>(4, n / 100000)) throw std::runtime_error("GPU parity gate failed");
            std::cout << passLabel << "\n";
        };
        if (a.validate || referenceEngine || !a.rawOutput.empty() || !a.out.empty()) validateFrame("GPU_PARITY_PASS");
        if (a.postGainSequence) {
            for (float gain : {0.5f, 1.0f, 2.0f, 4.0f}) {
                p.aePostGain = gain;
                ri.params = p;
                validateFrame("AE_POST_GAIN_FRAME_PASS");
            }
            std::cout << "AE_POST_GAIN_RUNTIME_UPDATE_PASS\n";
        }
        if (a.handoffBench) {
            std::cout << "HANDOFF_CONFIG lutStages=" << a.lutFiles.size()
                      << " controlsStress=" << (a.controlsStress ? 1 : 0) << " warmup=" << a.warm
                      << " measured=" << a.it << "\n";
            VkQueueFamilyProperties qp{};
            uint32_t qc = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(c.pd, &qc, nullptr);
            std::vector<VkQueueFamilyProperties> qps(qc);
            vkGetPhysicalDeviceQueueFamilyProperties(c.pd, &qc, qps.data());
            qp = qps[c.qf];
            if (qp.timestampValidBits == 0) throw std::runtime_error("queue has no timestamps");
            auto producerSpv = readSpv(a.producerSpv);
            auto producer = makeProducer(c, producerSpv, input.v);
            auto handoff = [&](VkCommandBuffer b) {
                imageBarrier(b, input.i, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT,
                             VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            };
            auto outputReuse = [&](VkCommandBuffer b) {
                VkMemoryBarrier mb{};
                mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                mb.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                vkCmdPipelineBarrier(b, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                     1, &mb, 0, nullptr, 0, nullptr);
            };
            if (a.warm) {
                auto b = cmd(c);
                begin(b);
                ri.commandBuffer = b;
                for (uint32_t i = 0; i < a.warm; i++) {
                    recordProducer(b, producer, a.w, a.h, a.lx, a.ly);
                    handoff(b);
                    engine.record(ri);
                    outputReuse(b);
                }
                submit(c, b);
            }
            VkQueryPoolCreateInfo qci{};
            qci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qci.queryCount = 4 * a.it;
            VkQueryPool qpool{};
            ck(vkCreateQueryPool(c.dev, &qci, nullptr, &qpool), "handoff query pool");
            auto b = cmd(c);
            begin(b);
            ri.commandBuffer = b;
            for (uint32_t i = 0; i < a.it; i++) {
                vkCmdWriteTimestamp(b, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, qpool, 4 * i);
                recordProducer(b, producer, a.w, a.h, a.lx, a.ly);
                vkCmdWriteTimestamp(b, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, qpool, 4 * i + 1);
                handoff(b);
                vkCmdWriteTimestamp(b, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, qpool, 4 * i + 2);
                engine.record(ri);
                vkCmdWriteTimestamp(b, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, qpool, 4 * i + 3);
                outputReuse(b);
            }
            submit(c, b);
            std::vector<uint64_t> ts(4 * a.it);
            ck(vkGetQueryPoolResults(c.dev, qpool, 0, 4 * a.it, ts.size() * 8, ts.data(), 8,
                                     VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
               "handoff query results");
            vkDestroyQueryPool(c.dev, qpool, nullptr);
            uint64_t mask = qp.timestampValidBits >= 64 ? ~uint64_t(0) : ((uint64_t(1) << qp.timestampValidBits) - 1);
            auto dt = [&](uint64_t x, uint64_t y) {
                return double((y - x) & mask) * c.prop.limits.timestampPeriod / 1e6;
            };
            std::vector<double> prod(a.it), bar(a.it), tone(a.it), total(a.it);
            for (uint32_t i = 0; i < a.it; i++) {
                prod[i] = dt(ts[4 * i], ts[4 * i + 1]);
                bar[i] = dt(ts[4 * i + 1], ts[4 * i + 2]);
                tone[i] = dt(ts[4 * i + 2], ts[4 * i + 3]);
                total[i] = dt(ts[4 * i], ts[4 * i + 3]);
            }
            printStats("HANDOFF_PRODUCER_MS", statsOf(prod));
            printStats("HANDOFF_BARRIER_MS", statsOf(bar));
            printStats("HANDOFF_TONEMAP_MS", statsOf(tone));
            printStats("HANDOFF_TOTAL_MS", statsOf(total));
        }
        if (a.bench) {
            std::cout << "BENCH_CONFIG lutStages=" << a.lutFiles.size()
                      << " controlsStress=" << (a.controlsStress ? 1 : 0) << "\n";
            VkQueueFamilyProperties qp{};
            uint32_t qc = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(c.pd, &qc, nullptr);
            std::vector<VkQueueFamilyProperties> qps(qc);
            vkGetPhysicalDeviceQueueFamilyProperties(c.pd, &qc, qps.data());
            qp = qps[c.qf];
            if (qp.timestampValidBits == 0) throw std::runtime_error("queue has no timestamps");
            if (a.warm) {
                auto b = cmd(c);
                begin(b);
                ri.commandBuffer = b;
                for (uint32_t i = 0; i < a.warm; i++) {
                    engine.record(ri);
                    VkMemoryBarrier mb{};
                    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                    mb.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                    vkCmdPipelineBarrier(b, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                         0, 1, &mb, 0, nullptr, 0, nullptr);
                }
                submit(c, b);
            }
            VkQueryPoolCreateInfo qci{};
            qci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qci.queryCount = 2 * a.it;
            VkQueryPool qpool{};
            ck(vkCreateQueryPool(c.dev, &qci, nullptr, &qpool), "query pool");
            auto b = cmd(c);
            begin(b);
            ri.commandBuffer = b;
            for (uint32_t i = 0; i < a.it; i++) {
                vkCmdWriteTimestamp(b, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, qpool, 2 * i);
                engine.record(ri);
                vkCmdWriteTimestamp(b, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, qpool, 2 * i + 1);
                VkMemoryBarrier mb{};
                mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                mb.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                vkCmdPipelineBarrier(b, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                     1, &mb, 0, nullptr, 0, nullptr);
            }
            submit(c, b);
            std::vector<uint64_t> ts(2 * a.it);
            ck(vkGetQueryPoolResults(c.dev, qpool, 0, 2 * a.it, ts.size() * 8, ts.data(), 8,
                                     VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
               "query results");
            vkDestroyQueryPool(c.dev, qpool, nullptr);
            std::vector<double> ms(a.it);
            uint64_t mask = qp.timestampValidBits >= 64 ? ~uint64_t(0) : ((uint64_t(1) << qp.timestampValidBits) - 1);
            for (uint32_t i = 0; i < a.it; i++) {
                uint64_t d = (ts[2 * i + 1] - ts[2 * i]) & mask;
                ms[i] = double(d) * c.prop.limits.timestampPeriod / 1e6;
            }
            std::sort(ms.begin(), ms.end());
            double mean = std::accumulate(ms.begin(), ms.end(), 0.0) / ms.size();
            auto pct = [&](double q) {
                size_t i = size_t(std::floor(q * (ms.size() - 1)));
                return ms[i];
            };
            std::cout << "BENCH_MS best=" << ms.front() << " median=" << pct(.5) << " mean=" << mean
                      << " p95=" << pct(.95) << " p99=" << pct(.99) << " worst=" << ms.back() << "\n";
        }
        std::cout << "VULKAN_RUN_PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << "\n";
        return 2;
    }
}
