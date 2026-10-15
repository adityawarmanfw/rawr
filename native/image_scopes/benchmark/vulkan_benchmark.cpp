#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

#include "image_scopes/image_scopes.h"
#include "layouts.hpp"
#include "vulkan_test_context.hpp"

using namespace image_scopes;
namespace t = image_scopes::tests;

namespace {
struct Stats {
    double best = 0, median = 0, mean = 0, p95 = 0, p99 = 0, worst = 0, cpuRecordMean = 0;
};

Stats summarize(std::vector<double> gpu, const std::vector<double>& cpu) {
    if (gpu.empty() || cpu.empty()) throw std::runtime_error("empty benchmark samples");
    std::sort(gpu.begin(), gpu.end());
    auto pct = [&](double q) {
        const double pos = q * static_cast<double>(gpu.size() - 1u);
        const auto lo = static_cast<std::size_t>(pos);
        const auto hi = std::min(lo + 1u, gpu.size() - 1u);
        const double f = pos - static_cast<double>(lo);
        return gpu[lo] * (1.0 - f) + gpu[hi] * f;
    };
    Stats s{};
    s.best = gpu.front();
    s.median = pct(.5);
    s.mean = std::accumulate(gpu.begin(), gpu.end(), 0.0) / gpu.size();
    s.p95 = pct(.95);
    s.p99 = pct(.99);
    s.worst = gpu.back();
    s.cpuRecordMean = std::accumulate(cpu.begin(), cpu.end(), 0.0) / cpu.size();
    return s;
}
void print(const std::string& name, const Stats& s) {
    std::cout << name << " best=" << s.best << " median=" << s.median << " mean=" << s.mean << " p95=" << s.p95
              << " p99=" << s.p99 << " worst=" << s.worst << " cpu_record_mean=" << s.cpuRecordMean << " ms\n";
}
std::vector<std::uint8_t> pattern(std::uint32_t w, std::uint32_t h) {
    std::vector<std::uint8_t> a(static_cast<std::size_t>(w) * h * 4u);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            auto* p = &a[(static_cast<std::size_t>(y) * w + x) * 4u];
            p[0] = static_cast<std::uint8_t>((x * 17u + y * 3u) & 255u);
            p[1] = static_cast<std::uint8_t>((x * 5u + y * 29u) & 255u);
            p[2] = static_cast<std::uint8_t>(((x ^ y) * 11u) & 255u);
            p[3] = 255u;
        }
    return a;
}

template <class Record>
Stats bench(t::VulkanTestContext& vk, VkQueryPool qp, ImageScopes& scopes, Record&& record, std::uint32_t warmup = 30,
            std::uint32_t measured = 300) {
    std::vector<double> gpu;
    gpu.reserve(measured);
    std::vector<double> cpu;
    cpu.reserve(measured);
    for (std::uint32_t i = 0; i < warmup + measured; ++i) {
        vk.beginCommands();
        vkCmdResetQueryPool(vk.commandBuffer(), qp, 0, 2);
        vkCmdWriteTimestamp(vk.commandBuffer(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 0);
        const auto c0 = std::chrono::steady_clock::now();
        record(vk.commandBuffer(), i % 3u);
        const auto c1 = std::chrono::steady_clock::now();
        vkCmdWriteTimestamp(vk.commandBuffer(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 1);
        vk.submitAndWait();
        scopes.retireFrameSlot(i % 3u);
        if (i >= warmup) {
            gpu.push_back(vk.timestampDeltaMs(qp, 0, 1));
            cpu.push_back(std::chrono::duration<double, std::milli>(c1 - c0).count());
        }
    }
    return summarize(std::move(gpu), cpu);
}

// Opt-in representative workload matrix; the legacy benchmark/gate format stays intact.
std::vector<std::uint8_t> livePixels(const std::string& kind, const std::string& fixtures, std::uint32_t w,
                                     std::uint32_t h) {
    auto pixels = pattern(w, h);
    if (kind == "portrait_a" || kind == "portrait_b") {
        std::ifstream f(fixtures + "/" + kind + ".ppm", std::ios::binary);
        std::string magic;
        unsigned pw = 0, ph = 0, maxval = 0;
        f >> magic >> pw >> ph >> maxval;
        if (!f || magic != "P6" || !pw || !ph || maxval != 255)
            throw std::runtime_error("invalid benchmark PPM fixture");
        f.get();
        std::vector<std::uint8_t> rgb(std::size_t(pw) * ph * 3u);
        f.read(reinterpret_cast<char*>(rgb.data()), std::streamsize(rgb.size()));
        if (std::size_t(f.gcount()) != rgb.size()) throw std::runtime_error("short benchmark PPM");
        for (unsigned y = 0; y < h; ++y)
            for (unsigned x = 0; x < w; ++x)
                for (unsigned c = 0; c < 3; ++c)
                    pixels[(std::size_t(y) * w + x) * 4u + c] =
                        rgb[(std::size_t(y * ph / h) * pw + x * pw / w) * 3u + c];
    } else {
        std::uint32_t random = 0x12345678u;
        for (std::size_t i = 0; i < pixels.size(); i += 4u)
            for (unsigned c = 0; c < 3; ++c) {
                random ^= random << 13u;
                random ^= random >> 17u;
                random ^= random << 5u;
                pixels[i + c] = kind == "gray" ? 128u : kind == "red" ? (c == 0u ? 255u : 0u) : random & 255u;
            }
    }
    return pixels;
}

void liveBench(t::VulkanTestContext& vk, VkQueryPool qp, const std::string& fixtures, unsigned measured,
               bool vectorsOnly) {
    std::cout << "LIVE_MEMORY vector_scratch_per_slot_bytes=" << detail::kVectorScratchBytes
              << " waveform_per_slot_bytes=" << sizeof(detail::DisplayWaveformStd430) << '\n';
    for (const auto& [w, h] : {std::pair<unsigned, unsigned>{2048, 1536}, {2040, 1532}, {2040, 1536}}) {
        for (const std::string kind : {"portrait_a", "portrait_b", "gray", "red", "noise"}) {
            auto input = vk.createRgbaImage(w, h);
            vk.uploadRgba(input, livePixels(kind, fixtures, w, h));
            auto wave = vk.createRgbaImage(768, 512), vec = vk.createRgbaImage(512, 512);
            vk.uploadRgba(wave, std::vector<std::uint8_t>(768u * 512u * 4u));
            vk.uploadRgba(vec, std::vector<std::uint8_t>(512u * 512u * 4u));
            ImageScopes scopes({{vk.physicalDevice(), vk.device(), nullptr}, 3});
            DisplayImageView iv{input.view, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_GENERAL, w, h};
            auto report = [&](const std::string& name, auto&& record, t::RgbaImage& target) {
                const auto label = "live_" + std::to_string(w) + "x" + std::to_string(h) + "_" + kind + "_" + name;
                print(label, bench(vk, qp, scopes, record, 10u, measured));
                // Outside the timed region: deterministic pixel fingerprint for same-device A/B checks.
                std::uint64_t hash = 14695981039346656037ull;
                for (auto byte : vk.downloadRgba(target)) hash = (hash ^ byte) * 1099511628211ull;
                std::cout << "PIXELS " << label << " " << hash << std::endl;
            };
            for (auto sampling :
                 {SamplingMode::Production25, SamplingMode::Production50, SamplingMode::FullReference}) {
                const auto suffix = sampling == SamplingMode::Production25   ? "25"
                                    : sampling == SamplingMode::Production50 ? "50"
                                                                             : "full";
                if (!vectorsOnly)
                    for (auto mode : {WaveformMode::Luma, WaveformMode::RgbOverlay}) {
                        for (unsigned q = 0; q < 4; ++q) {
                            report(
                                std::string(mode == WaveformMode::Luma ? "luma_" : "rgb_") + suffix + "_q" +
                                    std::to_string(q),
                                [&](VkCommandBuffer cmd, unsigned slot) {
                                    DisplayWaveformRecordInfo r{};
                                    r.commandBuffer = cmd;
                                    r.frameSlot = slot;
                                    r.input = iv;
                                    r.renderTarget = {wave.view, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_GENERAL, 768,
                                                      512};
                                    r.render.mode = mode;
                                    r.samplingMode = sampling;
                                    r.sourceQuarterTurns = q;
                                    scopes.recordDisplayWaveform(r);
                                },
                                wave);
                        }
                    }
                for (auto grid : {VectorscopeGrid::Compact128, VectorscopeGrid::Reference256}) {
                    report(
                        "vec" + std::to_string(static_cast<unsigned>(grid)) + "_" + suffix,
                        [&](VkCommandBuffer cmd, unsigned slot) {
                            VectorscopeRecordInfo r{};
                            r.commandBuffer = cmd;
                            r.frameSlot = slot;
                            r.input = iv;
                            r.renderTarget = {vec.view, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_GENERAL, 512, 512};
                            r.grid = grid;
                            r.samplingMode = sampling;
                            scopes.recordVectorscope(r);
                        },
                        vec);
                }
            }
            vk.destroyRgbaImage(vec);
            vk.destroyRgbaImage(wave);
            vk.destroyRgbaImage(input);
        }
    }
}
}  // namespace

int main(int argc, char** argv) {
    try {
        const bool vectorscopeOnly = (argc > 1 && std::string(argv[1]) == "--vectorscope-only");
        t::VulkanTestContext vk;
        std::cout << "Using GPU: " << vk.deviceName() << "\n";
        if (!vk.hasTimestamps()) {
            std::cout << "IMAGE_SCOPES_GPU_BENCHMARK_SKIP no timestamp support\n";
            return 0;
        }
        const auto& props = vk.properties();
        std::cout << "DEVICE vendor=" << props.vendorID << " device=" << props.deviceID
                  << " driver=" << props.driverVersion << " api=" << props.apiVersion << "\n";
        VkQueryPool qp = vk.createTimestampQueryPool(2);
        if (argc > 1 && (std::string(argv[1]) == "--live" || std::string(argv[1]) == "--live-vectors")) {
            if (argc < 3 || argc > 4)
                throw std::runtime_error("usage: --live|--live-vectors FIXTURE_DIRECTORY [ITERATIONS]");
            const unsigned iterations = argc == 4 ? std::stoul(argv[3]) : 100u;
            if (!iterations || iterations > 10000u) throw std::runtime_error("iterations must be 1..10000");
            liveBench(vk, qp, argv[2], iterations, std::string(argv[1]) == "--live-vectors");
            vk.destroyQueryPool(qp);
            std::cout << "IMAGE_SCOPES_LIVE_BENCHMARK_PASS\n";
            return 0;
        }
        {
            constexpr VkDeviceSize logical = sizeof(detail::DisplayWaveformStd430);
            auto probe = vk.createBuffer(logical, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            const auto actual = probe.allocationSize;
            vk.destroyBuffer(probe);
            std::cout << "WAVEFORM_MEMORY logical_per_slot_bytes=" << logical
                      << " actual_device_allocation_per_slot_bytes=" << actual
                      << " one_slot_MiB=" << (double(actual) / (1024.0 * 1024.0))
                      << " three_slots_MiB=" << (double(actual * 3ull) / (1024.0 * 1024.0))
                      << " luma_same_allocation=yes scratch_bytes=0\n";
        }
        try {
            for (const auto& [W, H] :
                 {std::pair<std::uint32_t, std::uint32_t>{2048, 1536}, {2040, 1532}, {2040, 1536}}) {
                auto px = pattern(W, H);
                auto input = vk.createRgbaImage(W, H);
                vk.uploadRgba(input, px);
                auto waveOut = vk.createRgbaImage(768, 512);
                auto vecOut = vk.createRgbaImage(512, 512);
                std::vector<std::uint8_t> waveZero(768u * 512u * 4u, 0u), vecZero(512u * 512u * 4u, 0u);
                vk.uploadRgba(waveOut, waveZero);
                vk.uploadRgba(vecOut, vecZero);
                ImageScopes scopes({{vk.physicalDevice(), vk.device(), nullptr}, 3});
                DisplayImageView iv{input.view, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_GENERAL, W, H};
                ScopeRenderTarget wt{waveOut.view, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_GENERAL, 768, 512};
                ScopeRenderTarget vt{vecOut.view, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_GENERAL, 512, 512};
                std::cout << "RESOLUTION " << W << "x" << H << "\n";
                if (!vectorscopeOnly) {
                    print("display_exposure_stats_6p25",
                          bench(vk, qp, scopes, [&](VkCommandBuffer cmd, std::uint32_t slot) {
                              DisplayExposureStatsRecordInfo r{};
                              r.commandBuffer = cmd;
                              r.input = iv;
                              r.frameSlot = slot;
                              r.sampleBlockSize = 4u;
                              scopes.recordDisplayExposureStats(r);
                          }));
                }
                {
                    for (auto smode :
                         {SamplingMode::Production25, SamplingMode::Production50, SamplingMode::FullReference}) {
                        const char* sn = smode == SamplingMode::Production25   ? "25"
                                         : smode == SamplingMode::Production50 ? "50"
                                                                               : "full";
                        if (!vectorscopeOnly) {
                            print(std::string("wave_") + sn + "_measure",
                                  bench(vk, qp, scopes, [&](VkCommandBuffer cmd, std::uint32_t slot) {
                                      DisplayWaveformRecordInfo r{};
                                      r.commandBuffer = cmd;
                                      r.input = iv;
                                      r.render.mode = WaveformMode::RgbOverlay;
                                      r.samplingMode = smode;
                                      r.frameSlot = slot;
                                      r.renderEnabled = false;
                                      scopes.recordDisplayWaveform(r);
                                  }));
                            print(std::string("wave_luma_") + sn + "_measure",
                                  bench(vk, qp, scopes, [&](VkCommandBuffer cmd, std::uint32_t slot) {
                                      DisplayWaveformRecordInfo r{};
                                      r.commandBuffer = cmd;
                                      r.input = iv;
                                      r.render.mode = WaveformMode::Luma;
                                      r.samplingMode = smode;
                                      r.frameSlot = slot;
                                      r.renderEnabled = false;
                                      scopes.recordDisplayWaveform(r);
                                  }));
                        }
                        print(std::string("vec256_") + sn + "_measure_present512",
                              bench(vk, qp, scopes, [&](VkCommandBuffer cmd, std::uint32_t slot) {
                                  VectorscopeRecordInfo r{};
                                  r.commandBuffer = cmd;
                                  r.input = iv;
                                  r.samplingMode = smode;
                                  r.frameSlot = slot;
                                  r.renderEnabled = false;
                                  scopes.recordVectorscope(r);
                              }));
                    }
                    for (auto smode :
                         {SamplingMode::Production25, SamplingMode::Production50, SamplingMode::FullReference}) {
                        const char* sn = smode == SamplingMode::Production25   ? "25"
                                         : smode == SamplingMode::Production50 ? "50"
                                                                               : "full";
                        print(std::string("vec128_") + sn + "_measure_present512",
                              bench(vk, qp, scopes, [&](VkCommandBuffer cmd, std::uint32_t slot) {
                                  VectorscopeRecordInfo r{};
                                  r.commandBuffer = cmd;
                                  r.input = iv;
                                  r.samplingMode = smode;
                                  r.grid = VectorscopeGrid::Compact128;
                                  r.frameSlot = slot;
                                  r.renderEnabled = false;
                                  scopes.recordVectorscope(r);
                              }));
                    }
                    // Seed all slots once for true presentation-only accepted photographic timing.
                    if (!vectorscopeOnly) {
                        for (std::uint32_t slot = 0; slot < 3u; ++slot) {
                            vk.beginCommands();
                            DisplayWaveformRecordInfo seed{};
                            seed.commandBuffer = vk.commandBuffer();
                            seed.input = iv;
                            seed.render.mode = WaveformMode::RgbOverlay;
                            seed.samplingMode = SamplingMode::FullReference;
                            seed.frameSlot = slot;
                            seed.renderEnabled = false;
                            scopes.recordDisplayWaveform(seed);
                            vk.submitAndWait();
                            scopes.retireFrameSlot(slot);
                        }
                        print("wave_rgb_render_only",
                              bench(vk, qp, scopes, [&](VkCommandBuffer cmd, std::uint32_t slot) {
                                  DisplayWaveformRenderInfo r{};
                                  r.commandBuffer = cmd;
                                  r.renderTarget = wt;
                                  r.render.mode = WaveformMode::RgbOverlay;
                                  r.frameSlot = slot;
                                  scopes.recordDisplayWaveformRender(r);
                              }));
                        for (std::uint32_t slot = 0; slot < 3u; ++slot) {
                            vk.beginCommands();
                            DisplayWaveformRecordInfo seed{};
                            seed.commandBuffer = vk.commandBuffer();
                            seed.input = iv;
                            seed.render.mode = WaveformMode::Luma;
                            seed.samplingMode = SamplingMode::FullReference;
                            seed.frameSlot = slot;
                            seed.renderEnabled = false;
                            scopes.recordDisplayWaveform(seed);
                            vk.submitAndWait();
                            scopes.retireFrameSlot(slot);
                        }
                        print("wave_luma_render_only",
                              bench(vk, qp, scopes, [&](VkCommandBuffer cmd, std::uint32_t slot) {
                                  DisplayWaveformRenderInfo r{};
                                  r.commandBuffer = cmd;
                                  r.renderTarget = wt;
                                  r.render.mode = WaveformMode::Luma;
                                  r.frameSlot = slot;
                                  scopes.recordDisplayWaveformRender(r);
                              }));
                    }
                    for (std::uint32_t slot = 0; slot < 3u; ++slot) {
                        vk.beginCommands();
                        VectorscopeRecordInfo seed{};
                        seed.commandBuffer = vk.commandBuffer();
                        seed.input = iv;
                        seed.samplingMode = SamplingMode::FullReference;
                        seed.grid = VectorscopeGrid::Reference256;
                        seed.frameSlot = slot;
                        seed.renderEnabled = false;
                        scopes.recordVectorscope(seed);
                        vk.submitAndWait();
                        scopes.retireFrameSlot(slot);
                    }
                    print("vec256_render_only", bench(vk, qp, scopes, [&](VkCommandBuffer cmd, std::uint32_t slot) {
                              VectorscopeRenderInfo r{};
                              r.commandBuffer = cmd;
                              r.renderTarget = vt;
                              r.frameSlot = slot;
                              scopes.recordVectorscopeRender(r);
                          }));
                    print("vec128_50_measure_render",
                          bench(vk, qp, scopes, [&](VkCommandBuffer cmd, std::uint32_t slot) {
                              VectorscopeRecordInfo r{};
                              r.commandBuffer = cmd;
                              r.input = iv;
                              r.renderTarget = vt;
                              r.samplingMode = SamplingMode::Production50;
                              r.grid = VectorscopeGrid::Compact128;
                              r.frameSlot = slot;
                              r.renderEnabled = true;
                              scopes.recordVectorscope(r);
                          }));
                    print("vec128_full_measure_render",
                          bench(vk, qp, scopes, [&](VkCommandBuffer cmd, std::uint32_t slot) {
                              VectorscopeRecordInfo r{};
                              r.commandBuffer = cmd;
                              r.input = iv;
                              r.renderTarget = vt;
                              r.samplingMode = SamplingMode::FullReference;
                              r.grid = VectorscopeGrid::Compact128;
                              r.frameSlot = slot;
                              r.renderEnabled = true;
                              scopes.recordVectorscope(r);
                          }));
                    if (!vectorscopeOnly)
                        print("wave_50_measure_render",
                              bench(vk, qp, scopes, [&](VkCommandBuffer cmd, std::uint32_t slot) {
                                  DisplayWaveformRecordInfo r{};
                                  r.commandBuffer = cmd;
                                  r.input = iv;
                                  r.renderTarget = wt;
                                  r.render.mode = WaveformMode::RgbOverlay;
                                  r.samplingMode = SamplingMode::Production50;
                                  r.frameSlot = slot;
                                  r.renderEnabled = true;
                                  scopes.recordDisplayWaveform(r);
                              }));
                    if (!vectorscopeOnly)
                        print("wave_luma_50_measure_render",
                              bench(vk, qp, scopes, [&](VkCommandBuffer cmd, std::uint32_t slot) {
                                  DisplayWaveformRecordInfo r{};
                                  r.commandBuffer = cmd;
                                  r.input = iv;
                                  r.renderTarget = wt;
                                  r.render.mode = WaveformMode::Luma;
                                  r.samplingMode = SamplingMode::Production50;
                                  r.frameSlot = slot;
                                  r.renderEnabled = true;
                                  scopes.recordDisplayWaveform(r);
                              }));
                    print("vec256_50_measure_render",
                          bench(vk, qp, scopes, [&](VkCommandBuffer cmd, std::uint32_t slot) {
                              VectorscopeRecordInfo r{};
                              r.commandBuffer = cmd;
                              r.input = iv;
                              r.renderTarget = vt;
                              r.samplingMode = SamplingMode::Production50;
                              r.frameSlot = slot;
                              r.renderEnabled = true;
                              scopes.recordVectorscope(r);
                          }));
                    if (!vectorscopeOnly)
                        print("wave_full_measure_render",
                              bench(vk, qp, scopes, [&](VkCommandBuffer cmd, std::uint32_t slot) {
                                  DisplayWaveformRecordInfo r{};
                                  r.commandBuffer = cmd;
                                  r.input = iv;
                                  r.renderTarget = wt;
                                  r.render.mode = WaveformMode::RgbOverlay;
                                  r.samplingMode = SamplingMode::FullReference;
                                  r.frameSlot = slot;
                                  r.renderEnabled = true;
                                  scopes.recordDisplayWaveform(r);
                              }));
                    if (!vectorscopeOnly)
                        print("wave_luma_full_measure_render",
                              bench(vk, qp, scopes, [&](VkCommandBuffer cmd, std::uint32_t slot) {
                                  DisplayWaveformRecordInfo r{};
                                  r.commandBuffer = cmd;
                                  r.input = iv;
                                  r.renderTarget = wt;
                                  r.render.mode = WaveformMode::Luma;
                                  r.samplingMode = SamplingMode::FullReference;
                                  r.frameSlot = slot;
                                  r.renderEnabled = true;
                                  scopes.recordDisplayWaveform(r);
                              }));
                    print("vec256_full_measure_render",
                          bench(vk, qp, scopes, [&](VkCommandBuffer cmd, std::uint32_t slot) {
                              VectorscopeRecordInfo r{};
                              r.commandBuffer = cmd;
                              r.input = iv;
                              r.renderTarget = vt;
                              r.samplingMode = SamplingMode::FullReference;
                              r.frameSlot = slot;
                              r.renderEnabled = true;
                              scopes.recordVectorscope(r);
                          }));
                }
                vk.destroyRgbaImage(vecOut);
                vk.destroyRgbaImage(waveOut);
                vk.destroyRgbaImage(input);
            }
            if (!vectorscopeOnly) {
                // RAW render-only timing. This consumes a frozen raw_stats-format buffer and does no RAW measurement.
                auto rawWave =
                    vk.createBuffer(262144, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                auto* rw1 = static_cast<std::uint32_t*>(vk.map(rawWave));
                for (unsigned i = 0; i < 262144 / 4u; ++i) rw1[i] = (i % 17u);
                vk.flush(rawWave);
                vk.unmap(rawWave);
                auto rawWaveOut = vk.createRgbaImage(640, 360);
                std::vector<std::uint8_t> rwz(640u * 360u * 4u, 0u);
                vk.uploadRgba(rawWaveOut, rwz);
                ImageScopes rawScopes({{vk.physicalDevice(), vk.device(), nullptr}, 3});
                ScopeRenderTarget rwt{rawWaveOut.view, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_GENERAL, 640, 360};
                print("raw_wave_render", bench(vk, qp, rawScopes, [&](VkCommandBuffer cmd, std::uint32_t slot) {
                          RawWaveformRecordInfo r{};
                          r.commandBuffer = cmd;
                          r.rawWaveform.waveform = {rawWave.buffer, 0, 262144};
                          r.renderTarget = rwt;
                          r.frameSlot = slot;
                          rawScopes.recordRawWaveformRender(r);
                      }));
                vk.destroyRgbaImage(rawWaveOut);
                vk.destroyBuffer(rawWave);
            }
        } catch (...) {
            vk.destroyQueryPool(qp);
            throw;
        }
        vk.destroyQueryPool(qp);
        std::cout << "IMAGE_SCOPES_GPU_BENCHMARK_PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
