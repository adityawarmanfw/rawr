#include <rawr/raw_gpu_pipeline/HdrPlusRecorder.h>

#include <algorithm>
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
    std::int32_t padX, padY, width, height, paddedWidth, paddedHeight;
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
    for (int i = 0; i < 4; ++i) pc.blackDelta[i] = reference_.blackByPhase[i] - frame.blackByPhase[i];
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

void HdrPlusRecorder::recordCompanionPrepare(VkCommandBuffer c, VkImageView rawU16, const RawNormalization& frame) {
    recordPrepare(c, false, rawU16, frame);
    recordPyramid(c, false);
}

void HdrPlusRecorder::recordCompanionAlignLevel(VkCommandBuffer c, std::uint32_t n) {
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
    executor_.record(c, ShaderId::HdrpTileDiff, {{ib(0, refLevel), ib(1, compLevel)}, {bb(2, corrected), bb(3, cost)}},
                     &tc, sizeof(tc), level.tilesX, level.tilesY);
    computeWriteBarrier(c);
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

}  // namespace rawr::raw_gpu_pipeline
