#include <rawr/raw_gpu_pipeline/HdrPlusRecorder.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>

namespace rawr::raw_gpu_pipeline {
namespace {
namespace hp = rawr::raw_merge_hdrplus_gpu;

constexpr std::uint32_t divUp(std::uint32_t x, std::uint32_t y) { return (x + y - 1u) / y; }
ImageBinding ib(std::uint32_t b, const ArenaImage& i) { return {b, i.view, VK_IMAGE_LAYOUT_GENERAL}; }
BufferBinding bb(std::uint32_t b, const ArenaBuffer& buf) { return {b, buf.buffer, 0, buf.bytes}; }
std::string levelName(bool reference, std::size_t level) {
    return std::string(reference ? "hdrp_ref_l" : "hdrp_comp_l") + std::to_string(level);
}

struct PreparePc {
    float blackDelta[4];
    float refBlack[4];
    std::int32_t padX, padY, width, height, paddedWidth, paddedHeight;
    float gain;
};
// Push-constant structs mirror the GLSL blocks (ivec2 members are 8-byte aligned).
struct HotPixelPc {
    std::int32_t count, alignPad, padX, padY, width, height;
};
struct SizePc {
    std::int32_t width, height;
};
struct BlurPc {
    std::int32_t width, height, srcX, srcY, dstX, dstY, kernelSize, stride, direction, quantizeHalf;
};
struct UpsamplePc {
    std::int32_t srcX, srcY, dstX, dstY;
    float scaleX, scaleY;
};
struct TileCostPc {
    std::int32_t levelWidth, levelHeight, tilesX, tilesY, downscale, tileSize, useSsd;
};
struct BestTilePc {
    std::int32_t tilesX, tilesY, downscale;
};
struct WarpPc {
    std::int32_t width, height, padX, padY, paddedWidth, paddedHeight, tilesX, tilesY, halfTileSize;
};
struct ColorDiffPc {
    std::int32_t cellsX, cellsY, aX, aY, bX, bY;
};
struct MeanPc {
    std::int32_t count;
    float pixels;
};
struct WeightPc {
    std::int32_t cellsX, cellsY;
    float robustness;
};
struct AccumulatePc {
    std::int32_t width, height, padX, padY, cellsX, cellsY;
    float invCount;
    std::int32_t mode;
};
struct FinalizePc {
    float black[4];
    float white;
    std::int32_t alignPad, width, height;
};
}  // namespace

ScratchLayout makeHdrPlusScratchLayout(const hp::Geometry& g) {
    ScratchLayout l{};
    const Extent2D raw{g.width, g.height}, padded{g.paddedWidth, g.paddedHeight}, cells{g.width / 2u, g.height / 2u};
    const auto add = [&](const char* name, Extent2D e, PixelStorage s, Lifetime life) {
        l.images.push_back({name, e, s, life});
        l.logicalImageBytes += std::uint64_t(e.width) * e.height * bytesPerPixel(s);
    };
    add("hdrp_ref_padded", padded, PixelStorage::R32Float, Lifetime::Burst);
    add("hdrp_comp_padded", padded, PixelStorage::R32Float, Lifetime::Companion);
    add("hdrp_aligned", raw, PixelStorage::R32Float, Lifetime::Companion);
    add("hdrp_ref_blur", raw, PixelStorage::R32Float, Lifetime::Burst);
    add("hdrp_comp_blur", raw, PixelStorage::R32Float, Lifetime::Companion);
    add("hdrp_blur_tmp", raw, PixelStorage::R32Float, Lifetime::Companion);
    add("hdrp_diff", cells, PixelStorage::R32Float, Lifetime::Companion);
    add("hdrp_weight", cells, PixelStorage::R32Float, Lifetime::Companion);
    add("hdrp_accum", raw, PixelStorage::R32Float, Lifetime::Output);
    add("hdrp_output", raw, PixelStorage::RGBA16Float, Lifetime::Output);
    add("hdrp_cfa", raw, PixelStorage::R32Float, Lifetime::Output);
    const Extent2D level0{g.levels.front().width, g.levels.front().height};
    add("hdrp_pyr_tmp_a", level0, PixelStorage::R32Float, Lifetime::PyramidTemp);
    add("hdrp_pyr_tmp_b", level0, PixelStorage::R32Float, Lifetime::PyramidTemp);
    std::uint64_t maxTiles = 1u;
    for (std::size_t i = 0; i < g.levels.size(); ++i) {
        const Extent2D e{g.levels[i].width, g.levels[i].height};
        l.images.push_back({levelName(true, i), e, PixelStorage::R32Float, Lifetime::Burst});
        l.images.push_back({levelName(false, i), e, PixelStorage::R32Float, Lifetime::Companion});
        l.logicalImageBytes += 2ull * e.width * e.height * 4ull;
        maxTiles = std::max<std::uint64_t>(maxTiles, std::uint64_t(g.levels[i].tilesX) * g.levels[i].tilesY);
    }
    const auto addBuffer = [&](const char* name, std::uint64_t bytes) {
        l.buffers.push_back({name, bytes, Lifetime::Companion});
        l.logicalBufferBytes += bytes;
    };
    addBuffer("hdrp_align_a", maxTiles * 8u);
    addBuffer("hdrp_align_b", maxTiles * 8u);
    addBuffer("hdrp_align_c", maxTiles * 8u);
    addBuffer("hdrp_tile_cost", maxTiles * 25u * 4u);
    addBuffer("hdrp_columns", std::uint64_t(cells.width) * 4u);
    addBuffer("hdrp_noise", 16u);
    return l;
}

HdrPlusRecorder::HdrPlusRecorder(ResourceArena& arena, VulkanExecutor& executor, hp::Config config,
                                 hp::Geometry geometry)
    : arena_(arena), executor_(executor), config_(config), geometry_(std::move(geometry)) {
    if (geometry_.levels.empty()) throw std::invalid_argument("hdrplus recorder: empty geometry");
}

void HdrPlusRecorder::recordPrepare(VkCommandBuffer c, bool reference, VkImageView rawU16,
                                    const RawNormalization& frame) {
    const auto& dst = arena_.image(reference ? "hdrp_ref_padded" : "hdrp_comp_padded");
    PreparePc pc{};
    for (int i = 0; i < 4; ++i) {
        pc.blackDelta[i] = reference_.blackByPhase[i] - frame.blackByPhase[i];
        pc.refBlack[i] = reference_.blackByPhase[i];
    }
    pc.gain = reference ? 1.0f : companionGain_;
    pc.padX = std::int32_t(geometry_.padLeft);
    pc.padY = std::int32_t(geometry_.padTop);
    pc.width = std::int32_t(geometry_.width);
    pc.height = std::int32_t(geometry_.height);
    pc.paddedWidth = std::int32_t(geometry_.paddedWidth);
    pc.paddedHeight = std::int32_t(geometry_.paddedHeight);
    executor_.record(c, ShaderId::HdrpPrepare, {{{0u, rawU16, VK_IMAGE_LAYOUT_GENERAL}, ib(1, dst)}, {}}, &pc,
                     sizeof(pc), divUp(geometry_.paddedWidth, 16), divUp(geometry_.paddedHeight, 16));
    computeWriteBarrier(c);
    if (hotPixelBuffer_ && hotPixelCount_) {
        HotPixelPc hpc{std::int32_t(hotPixelCount_), 0, pc.padX, pc.padY, pc.width, pc.height};
        executor_.record(c, ShaderId::HdrpHotPixel, {{ib(0, dst)}, {BufferBinding{1u, hotPixelBuffer_}}}, &hpc,
                         sizeof(hpc), divUp(hotPixelCount_, 64), 1);
        computeWriteBarrier(c);
    }
}

void HdrPlusRecorder::recordBlur(VkCommandBuffer c, const ArenaImage& src, std::array<std::int32_t, 2> srcOffset,
                                 const ArenaImage& tmp, const ArenaImage& dst, std::uint32_t w, std::uint32_t h,
                                 std::int32_t kernelSize, std::int32_t stride, bool quantizeHalf) {
    BlurPc x{std::int32_t(w), std::int32_t(h), srcOffset[0], srcOffset[1], 0, 0, kernelSize, stride, 0,
             quantizeHalf ? 1 : 0};
    executor_.record(c, ShaderId::HdrpBlur, {{ib(0, src), ib(1, tmp)}, {}}, &x, sizeof(x), divUp(w, 16),
                     divUp(h, 16));
    computeWriteBarrier(c);
    BlurPc y = x;
    y.srcX = y.srcY = 0;
    y.direction = 1;
    executor_.record(c, ShaderId::HdrpBlur, {{ib(0, tmp), ib(1, dst)}, {}}, &y, sizeof(y), divUp(w, 16),
                     divUp(h, 16));
    computeWriteBarrier(c);
}

void HdrPlusRecorder::recordPyramid(VkCommandBuffer c, bool reference) {
    const auto& padded = arena_.image(reference ? "hdrp_ref_padded" : "hdrp_comp_padded");
    const auto& tmpA = arena_.image("hdrp_pyr_tmp_a");
    const auto& tmpB = arena_.image("hdrp_pyr_tmp_b");
    for (std::size_t i = 0; i < geometry_.levels.size(); ++i) {
        const auto& level = geometry_.levels[i];
        const auto& dst = arena_.image(levelName(reference, i));
        const ArenaImage* src = &padded;
        if (i > 0) {
            // Upstream build_pyramid: blur(previous level, pattern 1, kernel 2), then pool.
            const auto& prev = geometry_.levels[i - 1];
            recordBlur(c, arena_.image(levelName(reference, i - 1)), {0, 0}, tmpA, tmpB, prev.width, prev.height, 2,
                       1, true);
            src = &tmpB;
        }
        SizePc pc{std::int32_t(level.width), std::int32_t(level.height)};
        executor_.record(c, ShaderId::HdrpAvgPool, {{ib(0, *src), ib(1, dst)}, {}}, &pc, sizeof(pc),
                         divUp(level.width, 16), divUp(level.height, 16));
        computeWriteBarrier(c);
    }
}

void HdrPlusRecorder::recordReference(VkCommandBuffer c, VkImageView rawU16, const RawNormalization& frame,
                                      std::uint32_t frameCount) {
    reference_ = frame;
    recordPrepare(c, true, rawU16, frame);
    recordPyramid(c, true);
    const auto& padded = arena_.image("hdrp_ref_padded");
    const auto& blur = arena_.image("hdrp_ref_blur");
    const std::array<std::int32_t, 2> pad{std::int32_t(geometry_.padLeft), std::int32_t(geometry_.padTop)};
    recordBlur(c, padded, pad, arena_.image("hdrp_blur_tmp"), blur, geometry_.width, geometry_.height, 16, 2, false);
    // Noise level: mean per-cell |ref - blur(ref)| (upstream estimate_color_noise).
    const std::uint32_t cellsX = geometry_.width / 2u, cellsY = geometry_.height / 2u;
    const auto& diff = arena_.image("hdrp_diff");
    ColorDiffPc cd{std::int32_t(cellsX), std::int32_t(cellsY), pad[0], pad[1], 0, 0};
    executor_.record(c, ShaderId::HdrpColorDiff, {{ib(0, padded), ib(1, blur), ib(2, diff)}, {}}, &cd, sizeof(cd),
                     divUp(cellsX, 16), divUp(cellsY, 16));
    computeWriteBarrier(c);
    SizePc cs{std::int32_t(cellsX), std::int32_t(cellsY)};
    executor_.record(c, ShaderId::HdrpColumnSum, {{ib(0, diff)}, {bb(1, arena_.buffer("hdrp_columns"))}}, &cs,
                     sizeof(cs), divUp(cellsX, 64), 1);
    computeWriteBarrier(c);
    MeanPc mp{std::int32_t(cellsX), float(cellsX) * float(cellsY)};
    executor_.record(c, ShaderId::HdrpMean, {{}, {bb(0, arena_.buffer("hdrp_columns")), bb(1, arena_.buffer("hdrp_noise"))}},
                     &mp, sizeof(mp), 1, 1);
    computeWriteBarrier(c);
    // Accumulator starts as ref / N; companions add lerp(ref, aligned, w) / N.
    AccumulatePc ap{std::int32_t(geometry_.width), std::int32_t(geometry_.height), pad[0], pad[1],
                    std::int32_t(cellsX), std::int32_t(cellsY), 1.0f / float(frameCount), 0};
    executor_.record(c, ShaderId::HdrpAccumulate,
                     {{ib(0, padded), ib(1, arena_.image("hdrp_aligned")), ib(2, arena_.image("hdrp_weight")),
                       ib(3, arena_.image("hdrp_accum"))},
                      {}},
                     &ap, sizeof(ap), divUp(geometry_.width, 16), divUp(geometry_.height, 16));
    computeWriteBarrier(c);
}

void HdrPlusRecorder::recordCompanionPadded(VkCommandBuffer c, VkImageView rawU16, const RawNormalization& frame) {
    recordPrepare(c, false, rawU16, frame);
}

void HdrPlusRecorder::recordReferencePrepare(VkCommandBuffer c, VkImageView rawU16, const RawNormalization& frame) {
    reference_ = frame;
    recordPrepare(c, true, rawU16, frame);
    recordPyramid(c, true);
}

void HdrPlusRecorder::recordCompanionPrepare(VkCommandBuffer c, VkImageView rawU16, const RawNormalization& frame) {
    recordPrepare(c, false, rawU16, frame);
    recordPyramid(c, false);
}

void HdrPlusRecorder::recordCompanionAlignLevel(VkCommandBuffer c, std::uint32_t n,
                                                const std::function<void(std::uint32_t)>& mark) {
    if (n >= geometry_.levels.size()) throw std::invalid_argument("hdrplus recorder: invalid alignment level");
    const auto& cur = arena_.buffer("hdrp_align_a");
    const auto& prev = arena_.buffer("hdrp_align_b");
    const auto& corrected = arena_.buffer("hdrp_align_c");
    const auto& cost = arena_.buffer("hdrp_tile_cost");
    const auto& level = geometry_.levels[n];
    const bool coarsest = n + 1u == geometry_.levels.size();
    // Alignment from the next-coarser level, in its (2x smaller) pixel units.
    const std::int32_t srcX = coarsest ? 1 : std::int32_t(geometry_.levels[n + 1u].tilesX);
    const std::int32_t srcY = coarsest ? 1 : std::int32_t(geometry_.levels[n + 1u].tilesY);
    const std::int32_t downscale = coarsest ? 0 : 2;
    UpsamplePc up{srcX, srcY, std::int32_t(level.tilesX), std::int32_t(level.tilesY),
                  float(double(level.tilesX) / double(srcX)), float(double(level.tilesY) / double(srcY))};
    executor_.record(c, ShaderId::HdrpUpsampleAlign, {{}, {bb(0, cur), bb(1, prev)}}, &up, sizeof(up),
                     divUp(level.tilesX, 16), divUp(level.tilesY, 16));
    computeWriteBarrier(c);
    const auto& refLevel = arena_.image(levelName(true, n));
    const auto& compLevel = arena_.image(levelName(false, n));
    TileCostPc tc{std::int32_t(level.width), std::int32_t(level.height), std::int32_t(level.tilesX),
                  std::int32_t(level.tilesY), downscale, std::int32_t(level.tileSize), n != 0u ? 1 : 0};
    executor_.record(c, ShaderId::HdrpCorrectUpsampling,
                     {{ib(0, refLevel), ib(1, compLevel)}, {bb(2, prev), bb(3, corrected)}}, &tc, sizeof(tc),
                     level.tilesX, level.tilesY);
    computeWriteBarrier(c);
    if (mark) mark(1u);
    executor_.record(c, ShaderId::HdrpTileDiff, {{ib(0, refLevel), ib(1, compLevel)}, {bb(2, corrected), bb(3, cost)}},
                     &tc, sizeof(tc), level.tilesX, level.tilesY);
    computeWriteBarrier(c);
    if (mark) mark(2u);
    BestTilePc bt{std::int32_t(level.tilesX), std::int32_t(level.tilesY), downscale};
    executor_.record(c, ShaderId::HdrpBestTile, {{}, {bb(0, cost), bb(1, corrected), bb(2, cur)}}, &bt, sizeof(bt),
                     divUp(level.tilesX, 16), divUp(level.tilesY, 16));
    computeWriteBarrier(c);
}

void HdrPlusRecorder::recordCompanionMerge(VkCommandBuffer c, std::uint32_t frameCount,
                                           const std::function<void(std::uint32_t)>& mark) {
    const auto& level0 = geometry_.levels.front();
    const auto& aligned = arena_.image("hdrp_aligned");
    WarpPc wp{std::int32_t(geometry_.width), std::int32_t(geometry_.height), std::int32_t(geometry_.padLeft),
              std::int32_t(geometry_.padTop), std::int32_t(geometry_.paddedWidth),
              std::int32_t(geometry_.paddedHeight), std::int32_t(level0.tilesX), std::int32_t(level0.tilesY),
              std::int32_t(level0.tileSize)};
    executor_.record(c, ShaderId::HdrpWarp,
                     {{ib(0, arena_.image("hdrp_comp_padded")), ib(1, aligned)}, {bb(2, arena_.buffer("hdrp_align_a"))}},
                     &wp, sizeof(wp), divUp(geometry_.width, 16), divUp(geometry_.height, 16));
    computeWriteBarrier(c);
    if (mark) mark(1u);
    const auto& compBlur = arena_.image("hdrp_comp_blur");
    recordBlur(c, aligned, {0, 0}, arena_.image("hdrp_blur_tmp"), compBlur, geometry_.width, geometry_.height, 16, 2,
               false);
    if (mark) mark(2u);
    const std::uint32_t cellsX = geometry_.width / 2u, cellsY = geometry_.height / 2u;
    const auto& diff = arena_.image("hdrp_diff");
    const auto& weight = arena_.image("hdrp_weight");
    ColorDiffPc cd{std::int32_t(cellsX), std::int32_t(cellsY), 0, 0, 0, 0};
    executor_.record(c, ShaderId::HdrpColorDiff, {{ib(0, arena_.image("hdrp_ref_blur")), ib(1, compBlur), ib(2, diff)}, {}},
                     &cd, sizeof(cd), divUp(cellsX, 16), divUp(cellsY, 16));
    computeWriteBarrier(c);
    WeightPc wt{std::int32_t(cellsX), std::int32_t(cellsY), hp::robustness(config_.strength)};
    executor_.record(c, ShaderId::HdrpMergeWeight, {{ib(0, diff), ib(1, weight)}, {bb(2, arena_.buffer("hdrp_noise"))}},
                     &wt, sizeof(wt), divUp(cellsX, 16), divUp(cellsY, 16));
    computeWriteBarrier(c);
    if (mark) mark(3u);
    AccumulatePc ap{std::int32_t(geometry_.width), std::int32_t(geometry_.height), std::int32_t(geometry_.padLeft),
                    std::int32_t(geometry_.padTop), std::int32_t(cellsX), std::int32_t(cellsY),
                    1.0f / float(frameCount), 1};
    executor_.record(c, ShaderId::HdrpAccumulate,
                     {{ib(0, arena_.image("hdrp_ref_padded")), ib(1, aligned), ib(2, weight),
                       ib(3, arena_.image("hdrp_accum"))},
                      {}},
                     &ap, sizeof(ap), divUp(geometry_.width, 16), divUp(geometry_.height, 16));
    computeWriteBarrier(c);
}

void HdrPlusRecorder::recordFinalize(VkCommandBuffer c) {
    FinalizePc fp{};
    for (int i = 0; i < 4; ++i) fp.black[i] = reference_.blackByPhase[i];
    fp.white = reference_.whiteLevel;
    fp.width = std::int32_t(geometry_.width);
    fp.height = std::int32_t(geometry_.height);
    executor_.record(c, ShaderId::HdrpFinalize,
                     {{ib(0, arena_.image("hdrp_accum")), ib(1, arena_.image("hdrp_output")),
                       ib(2, arena_.image("hdrp_cfa"))},
                      {}},
                     &fp, sizeof(fp),
                     divUp(geometry_.width, 16), divUp(geometry_.height, 16));
    computeWriteBarrier(c);
}

namespace {
struct TilesPc {
    std::int32_t tilesX, tilesY;
};
struct ToRgbaPc {
    std::int32_t width, height, cropX, cropY, offsetX, offsetY, paddedWidth, paddedHeight;
};
struct WarpRgbaPc {
    std::int32_t width, height, cropX, cropY, paddedWidth, paddedHeight, tilesX, tilesY, halfTileSize;
    std::int32_t alignPad;  // ivec2 offset is 8-byte aligned in the GLSL block
    std::int32_t offsetX, offsetY;
    std::int32_t continuous;
};
VkDeviceSize alignUp256(VkDeviceSize v) { return (v + 255u) / 256u * 256u; }
struct MismatchPc {
    std::int32_t tilesX, tilesY, rgbaWidth, rgbaHeight;
    float exposureFactor;
};
struct HighlightsPc {
    std::int32_t tilesX, tilesY;
    float exposureFactor, whiteLevel, blackLevelMean;
};
struct RegionPc {
    std::int32_t originX, originY, width, height;
};
struct MismatchNormPc {
    std::int32_t tilesX, tilesY;
    float invFrames;
};
struct FreqMergePc {
    std::int32_t tilesX, tilesY;
    float robustnessNorm, readNoise, maxMotionNorm;
    std::int32_t uniformExposure;
};
struct BackwardPc {
    std::int32_t tilesX, tilesY;
    float frames;
};
struct BorderPc {
    std::int32_t width, height;
    float minValue;
};
struct FreqAccumulatePc {
    std::int32_t width, height, offsetX, offsetY, mode;
};
}  // namespace

ScratchLayout makeHdrPlusFrequencyScratchLayout(const hp::FrequencyGeometry& f, std::uint32_t width,
                                                std::uint32_t height) {
    ScratchLayout l{};
    const auto& g = f.align;
    const Extent2D raw{width, height}, padded{g.paddedWidth, g.paddedHeight};
    const Extent2D rgba{f.rgbaWidth, f.rgbaHeight}, spectrum{2u * f.rgbaWidth, f.rgbaHeight}, tiles{f.tilesX, f.tilesY};
    const auto add = [&](const std::string& name, Extent2D e, PixelStorage s, Lifetime life) {
        l.images.push_back({name, e, s, life});
        l.logicalImageBytes += std::uint64_t(e.width) * e.height * bytesPerPixel(s);
    };
    add("hdrp_ref_padded", padded, PixelStorage::R32Float, Lifetime::Burst);
    add("hdrp_comp_padded", padded, PixelStorage::R32Float, Lifetime::Companion);
    add("hdrp_pyr_tmp_a", {g.levels.front().width, g.levels.front().height}, PixelStorage::R32Float, Lifetime::PyramidTemp);
    add("hdrp_pyr_tmp_b", {g.levels.front().width, g.levels.front().height}, PixelStorage::R32Float, Lifetime::PyramidTemp);
    std::uint64_t maxTiles = 1u;
    for (std::size_t i = 0; i < g.levels.size(); ++i) {
        const Extent2D e{g.levels[i].width, g.levels[i].height};
        add(levelName(true, i), e, PixelStorage::R32Float, Lifetime::Burst);
        add(levelName(false, i), e, PixelStorage::R32Float, Lifetime::Companion);
        maxTiles = std::max<std::uint64_t>(maxTiles, std::uint64_t(g.levels[i].tilesX) * g.levels[i].tilesY);
    }
    add("hdrq_ref_rgba", rgba, PixelStorage::RGBA32Float, Lifetime::Burst);
    add("hdrq_aligned_rgba", rgba, PixelStorage::RGBA32Float, Lifetime::Companion);
    add("hdrq_out_rgba", rgba, PixelStorage::RGBA32Float, Lifetime::Output);
    add("hdrq_ref_ft", spectrum, PixelStorage::RGBA32Float, Lifetime::Burst);
    add("hdrq_aligned_ft", spectrum, PixelStorage::RGBA32Float, Lifetime::Companion);
    add("hdrq_final_ft", spectrum, PixelStorage::RGBA32Float, Lifetime::Output);
    add("hdrq_rms", tiles, PixelStorage::RGBA32Float, Lifetime::Burst);
    add("hdrq_mismatch", tiles, PixelStorage::R32Float, Lifetime::Companion);
    add("hdrq_total_mismatch", tiles, PixelStorage::R32Float, Lifetime::Output);
    add("hdrq_highlights", tiles, PixelStorage::R32Float, Lifetime::Companion);
    add("hdrp_accum", raw, PixelStorage::R32Float, Lifetime::Output);
    add("hdrp_output", raw, PixelStorage::RGBA16Float, Lifetime::Output);
    add("hdrp_cfa", raw, PixelStorage::R32Float, Lifetime::Output);
    const auto addBuffer = [&](const char* name, std::uint64_t bytes) {
        l.buffers.push_back({name, bytes, Lifetime::Companion});
        l.logicalBufferBytes += bytes;
    };
    addBuffer("hdrp_align_a", maxTiles * 8u);
    addBuffer("hdrp_align_b", maxTiles * 8u);
    addBuffer("hdrp_align_c", maxTiles * 8u);
    addBuffer("hdrp_tile_cost", maxTiles * 25u * 4u);
    addBuffer("hdrq_mean", 16u);
    addBuffer("hdrq_shift_table", 49u * 64u * 8u);
    const auto& level0 = g.levels.front();
    addBuffer("hdrq_align_store",
              hp::kMaxFrequencyFrames * alignUp256(std::uint64_t(level0.tilesX) * level0.tilesY * 8u));
    return l;
}

HdrPlusFrequencyRecorder::HdrPlusFrequencyRecorder(ResourceArena& arena, VulkanExecutor& executor, hp::Config config,
                                                   hp::FrequencyGeometry geometry)
    : arena_(arena), executor_(executor), config_(config), geometry_(std::move(geometry)),
      align_(arena, executor, config, geometry_.align) {}

void HdrPlusFrequencyRecorder::setExposure(std::vector<float> exposureFactors, std::vector<float> whiteLevels) {
    if (!exposureFactors.empty() && whiteLevels.size() != exposureFactors.size())
        throw std::invalid_argument("hdrplus frequency: exposure/white level count mismatch");
    for (const float f : exposureFactors)
        if (!(std::isfinite(f) && f > 0.0f)) throw std::invalid_argument("hdrplus frequency: invalid exposure factor");
    uniformExposure_ = hp::uniformExposure(exposureFactors);
    exposureFactors_ = uniformExposure_ ? std::vector<float>{} : std::move(exposureFactors);
    whiteLevels_ = uniformExposure_ ? std::vector<float>{} : std::move(whiteLevels);
}

void HdrPlusFrequencyRecorder::beginPass(std::uint32_t pass) noexcept {
    pass_ = pass;
    // Align-once keeps every frame in the pass-0 padding; passOffset() maps.
    const std::uint32_t prepared = config_.frequencyAlignOnce ? 0u : pass;
    align_.setPads(geometry_.padLeft(prepared), geometry_.padTop(prepared));
}

std::array<std::int32_t, 2> HdrPlusFrequencyRecorder::passOffset() const noexcept {
    if (!config_.frequencyAlignOnce) return {0, 0};
    return {std::int32_t(geometry_.padLeft(0)) - std::int32_t(geometry_.padLeft(pass_)),
            std::int32_t(geometry_.padTop(0)) - std::int32_t(geometry_.padTop(pass_))};
}

VkDeviceSize HdrPlusFrequencyRecorder::alignSlotBytes() const noexcept {
    const auto& level0 = geometry_.align.levels.front();
    return alignUp256(VkDeviceSize(level0.tilesX) * level0.tilesY * 8u);
}

void HdrPlusFrequencyRecorder::recordCompanionAlign(VkCommandBuffer c, VkImageView rawU16, const RawNormalization& frame,
                                                    std::uint32_t slot) {
    align_.setCompanionGain(1.0f / frameExposure(slot));
    if (!alignsThisPass()) {
        align_.recordCompanionPadded(c, rawU16, frame);
        return;
    }
    align_.recordCompanionPrepare(c, rawU16, frame);
    for (std::uint32_t level = align_.levelCount(); level-- > 0;) align_.recordCompanionAlignLevel(c, level);
    if (!config_.frequencyAlignOnce) return;
    if (slot >= hp::kMaxFrequencyFrames) throw std::invalid_argument("hdrplus frequency: too many frames");
    const auto& level0 = geometry_.align.levels.front();
    VkMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT,
                               VK_ACCESS_TRANSFER_READ_BIT};
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &toTransfer, 0,
                         nullptr, 0, nullptr);
    const VkBufferCopy copy{0, slot * alignSlotBytes(), VkDeviceSize(level0.tilesX) * level0.tilesY * 8u};
    vkCmdCopyBuffer(c, arena_.buffer("hdrp_align_a").buffer, arena_.buffer("hdrq_align_store").buffer, 1, &copy);
    VkMemoryBarrier toCompute{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT,
                              VK_ACCESS_SHADER_READ_BIT};
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &toCompute, 0,
                         nullptr, 0, nullptr);
}

void HdrPlusFrequencyRecorder::recordReference(VkCommandBuffer c, VkImageView rawU16, const RawNormalization& frame) {
    if (pass_ == 0u) {
        executor_.record(c, ShaderId::HdrqShiftTable, {{}, {bb(0, arena_.buffer("hdrq_shift_table"))}}, nullptr, 0u,
                         divUp(49u * 64u, 64u), 1);
    }
    if (alignsThisPass()) align_.recordReferencePrepare(c, rawU16, frame);
    const auto& rgba = arena_.image("hdrq_ref_rgba");
    const std::uint32_t tx = geometry_.tilesX, ty = geometry_.tilesY;
    const auto offset = passOffset();
    ToRgbaPc tr{std::int32_t(geometry_.rgbaWidth),         std::int32_t(geometry_.rgbaHeight),
                std::int32_t(geometry_.cropX),             std::int32_t(geometry_.cropY),
                offset[0],                                 offset[1],
                std::int32_t(geometry_.align.paddedWidth), std::int32_t(geometry_.align.paddedHeight)};
    executor_.record(c, ShaderId::HdrqToRgba, {{ib(0, arena_.image("hdrp_ref_padded")), ib(1, rgba)}, {}}, &tr,
                     sizeof(tr), divUp(geometry_.rgbaWidth, 16), divUp(geometry_.rgbaHeight, 16));
    computeWriteBarrier(c);
    TilesPc tp{std::int32_t(tx), std::int32_t(ty)};
    executor_.record(c, ShaderId::HdrqRms, {{ib(0, rgba), ib(1, arena_.image("hdrq_rms"))}, {}}, &tp, sizeof(tp),
                     divUp(tx, 16), divUp(ty, 16));
    // The reference spectrum seeds both the reference and the running merge.
    executor_.record(c, ShaderId::HdrqForwardDft, {{ib(0, rgba), ib(1, arena_.image("hdrq_ref_ft"))}, {}}, &tp,
                     sizeof(tp), tx, ty);
    executor_.record(c, ShaderId::HdrqForwardDft, {{ib(0, rgba), ib(1, arena_.image("hdrq_final_ft"))}, {}}, &tp,
                     sizeof(tp), tx, ty);
    // Per-pass total mismatch starts at zero.
    VkMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                               VK_ACCESS_TRANSFER_WRITE_BIT};
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &toTransfer, 0,
                         nullptr, 0, nullptr);
    const VkClearColorValue zero{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(c, arena_.image("hdrq_total_mismatch").image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    VkMemoryBarrier toCompute{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT,
                              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &toCompute, 0, nullptr, 0, nullptr);
}

void HdrPlusFrequencyRecorder::recordCompanionMerge(VkCommandBuffer c, std::uint32_t frameCount, std::uint32_t slot,
                                                    const std::function<void(std::uint32_t)>& mark) {
    const auto& a = geometry_.align;
    const auto& level0 = a.levels.front();
    const std::uint32_t tx = geometry_.tilesX, ty = geometry_.tilesY;
    const auto& refRgba = arena_.image("hdrq_ref_rgba");
    const auto& alignedRgba = arena_.image("hdrq_aligned_rgba");
    const auto& mismatch = arena_.image("hdrq_mismatch");
    const auto offset = passOffset();
    WarpRgbaPc wp{std::int32_t(geometry_.rgbaWidth), std::int32_t(geometry_.rgbaHeight), std::int32_t(geometry_.cropX),
                  std::int32_t(geometry_.cropY), std::int32_t(a.paddedWidth), std::int32_t(a.paddedHeight),
                  std::int32_t(level0.tilesX), std::int32_t(level0.tilesY), std::int32_t(level0.tileSize), 0,
                  offset[0], offset[1], config_.frequencyAlignOnce ? 1 : 0};
    // Align-once: this companion's pass-0 shifts from its store slot.
    const BufferBinding shifts =
        config_.frequencyAlignOnce
            ? BufferBinding{2u, arena_.buffer("hdrq_align_store").buffer, slot * alignSlotBytes(),
                            VkDeviceSize(level0.tilesX) * level0.tilesY * 8u}
            : bb(2, arena_.buffer("hdrp_align_a"));
    executor_.record(c, ShaderId::HdrqWarpRgba,
                     {{ib(0, arena_.image("hdrp_comp_padded")), ib(1, alignedRgba)}, {shifts}},
                     &wp, sizeof(wp), divUp(geometry_.rgbaWidth, 16), divUp(geometry_.rgbaHeight, 16));
    computeWriteBarrier(c);
    if (mark) mark(1u);
    const float exposureFactor = frameExposure(slot);
    MismatchPc mp{std::int32_t(tx), std::int32_t(ty), std::int32_t(geometry_.rgbaWidth), std::int32_t(geometry_.rgbaHeight),
                  exposureFactor};
    executor_.record(c, ShaderId::HdrqMismatch,
                     {{ib(0, refRgba), ib(1, alignedRgba), ib(2, arena_.image("hdrq_rms")), ib(3, mismatch)}, {}}, &mp,
                     sizeof(mp), tx, ty);
    if (mark) {
        computeWriteBarrier(c);
        mark(5u);
    }
    TilesPc tp{std::int32_t(tx), std::int32_t(ty)};
    executor_.record(c, ShaderId::HdrqForwardDft, {{ib(0, alignedRgba), ib(1, arena_.image("hdrq_aligned_ft"))}, {}}, &tp,
                     sizeof(tp), tx, ty);
    computeWriteBarrier(c);
    if (mark) mark(2u);
    // Mean mismatch excluding the tile row/column introduced by this pass's shift.
    const std::uint32_t left = hp::FrequencyGeometry::shiftLeft(pass_) ? 1u : 0u;
    const std::uint32_t right = hp::FrequencyGeometry::shiftLeft(pass_) ? 0u : 1u;
    const std::uint32_t top = hp::FrequencyGeometry::shiftTop(pass_) ? 1u : 0u;
    const std::uint32_t bottom = hp::FrequencyGeometry::shiftTop(pass_) ? 0u : 1u;
    RegionPc rp{std::int32_t(left), std::int32_t(top), std::int32_t(tx - left - right), std::int32_t(ty - top - bottom)};
    executor_.record(c, ShaderId::HdrqRegionMean, {{ib(0, mismatch)}, {bb(1, arena_.buffer("hdrq_mean"))}}, &rp,
                     sizeof(rp), 1, 1);
    computeWriteBarrier(c);
    MismatchNormPc np{std::int32_t(tx), std::int32_t(ty), 1.0f / float(frameCount)};
    executor_.record(c, ShaderId::HdrqMismatchNorm,
                     {{ib(0, mismatch), ib(1, arena_.image("hdrq_total_mismatch"))}, {bb(2, arena_.buffer("hdrq_mean"))}},
                     &np, sizeof(np), divUp(tx, 16), divUp(ty, 16));
    computeWriteBarrier(c);
    if (mark) mark(3u);
    // Clipped-highlights factor per tile (all 1 for frames no brighter than the reference).
    const auto& highlights = arena_.image("hdrq_highlights");
    float blackMean = 0.0f;
    for (const float b : align_.referenceBlack()) blackMean += 0.25f * b;
    HighlightsPc hl{std::int32_t(tx), std::int32_t(ty), exposureFactor,
                    slot < whiteLevels_.size() ? whiteLevels_[slot] : 65535.0f, blackMean};
    executor_.record(c, ShaderId::HdrqHighlightsNorm, {{ib(0, alignedRgba), ib(1, highlights)}, {}}, &hl, sizeof(hl),
                     divUp(tx, 16), divUp(ty, 16));
    computeWriteBarrier(c);
    const auto norms = uniformExposure_ ? hp::frequencyNorms(config_.strength)
                                        : hp::bracketedFrequencyNorms(config_.strength, exposureFactors_);
    const float maxMotionNorm =
        uniformExposure_ ? norms.maxMotionNorm : hp::bracketedMaxMotionNorm(norms, exposureFactor);
    FreqMergePc fm{std::int32_t(tx), std::int32_t(ty), norms.robustnessNorm, norms.readNoise, maxMotionNorm,
                   uniformExposure_ ? 1 : 0};
    executor_.record(c, ShaderId::HdrqMerge,
                     {{ib(0, arena_.image("hdrq_ref_ft")), ib(1, arena_.image("hdrq_aligned_ft")),
                       ib(2, arena_.image("hdrq_final_ft")), ib(3, arena_.image("hdrq_rms")), ib(4, mismatch),
                       ib(6, highlights)},
                      {bb(5, arena_.buffer("hdrq_shift_table"))}},
                     &fm, sizeof(fm), tx, ty);
    computeWriteBarrier(c);
}

void HdrPlusFrequencyRecorder::recordPassFinish(VkCommandBuffer c, std::uint32_t frameCount) {
    const std::uint32_t tx = geometry_.tilesX, ty = geometry_.tilesY;
    const auto& finalFt = arena_.image("hdrq_final_ft");
    const auto& out = arena_.image("hdrq_out_rgba");
    TilesPc tp{std::int32_t(tx), std::int32_t(ty)};
    executor_.record(c, ShaderId::HdrqDeconvolute, {{ib(0, finalFt), ib(1, arena_.image("hdrq_total_mismatch"))}, {}}, &tp,
                     sizeof(tp), tx, ty);
    computeWriteBarrier(c);
    BackwardPc bp{std::int32_t(tx), std::int32_t(ty), float(frameCount)};
    executor_.record(c, ShaderId::HdrqBackwardDft, {{ib(0, finalFt), ib(1, out)}, {}}, &bp, sizeof(bp), tx, ty);
    computeWriteBarrier(c);
    // Exposure control off: upstream passes black level -1, so the clamp floor is -2 * window.
    BorderPc bd{std::int32_t(geometry_.rgbaWidth), std::int32_t(geometry_.rgbaHeight), -2.0f};
    executor_.record(c, ShaderId::HdrqBorder, {{ib(0, out), ib(1, arena_.image("hdrq_ref_rgba"))}, {}}, &bd, sizeof(bd),
                     divUp(geometry_.rgbaWidth, 16), divUp(geometry_.rgbaHeight, 16));
    computeWriteBarrier(c);
    const auto& accum = arena_.image("hdrp_accum");
    FreqAccumulatePc ap{std::int32_t(accum.extent.width), std::int32_t(accum.extent.height),
                        std::int32_t(geometry_.padLeft(pass_) - geometry_.cropX),
                        std::int32_t(geometry_.padTop(pass_) - geometry_.cropY), pass_ == 0u ? 0 : 1};
    executor_.record(c, ShaderId::HdrqAccumulate, {{ib(0, out), ib(1, accum)}, {}}, &ap, sizeof(ap),
                     divUp(accum.extent.width, 16), divUp(accum.extent.height, 16));
    computeWriteBarrier(c);
}

}  // namespace rawr::raw_gpu_pipeline
