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
struct AvgPoolPc {
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
    std::int32_t levelWidth, levelHeight, tilesX, tilesY, downscale, tileSize, useSsd, exposureRatio;
};
struct BestTilePc {
    std::int32_t tilesX, tilesY, downscale;
};
struct FinalizePc {
    float black[4];
    float white;
    std::int32_t alignPad, width, height;
};
}  // namespace

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
        AvgPoolPc pc{std::int32_t(level.width), std::int32_t(level.height)};
        executor_.record(c, ShaderId::HdrpAvgPool, {{ib(0, *src), ib(1, dst)}, {}}, &pc, sizeof(pc),
                         divUp(level.width, 16), divUp(level.height, 16));
        computeWriteBarrier(c);
    }
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
                  std::int32_t(level.tilesY), downscale, std::int32_t(level.tileSize), n != 0u ? 1 : 0,
                  std::fabs(companionGain_ - 1.0f) > 1.0e-3f ? 1 : 0};
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
    float scale;
};
struct RmsPc {
    std::int32_t tilesX, tilesY;
    float frameInv;
};
struct WarpRgbaPc {
    std::int32_t width, height, cropX, cropY, paddedWidth, paddedHeight, tilesX, tilesY, halfTileSize;
    std::int32_t alignPad;  // ivec2 offset is 8-byte aligned in the GLSL block
    std::int32_t offsetX, offsetY;
    std::int32_t continuous;
    float scale;
};
VkDeviceSize alignUp256(VkDeviceSize v) { return (v + 255u) / 256u * 256u; }
struct MismatchPc {
    std::int32_t tilesX, tilesY, rgbaWidth, rgbaHeight;
    float noiseSlope, noiseOffset, black, frameInv;
    float energyGain, energyDead, energyScale;
};
struct RegionPc {
    std::int32_t originX, originY, width, height;
};
struct MismatchNormPc {
    std::int32_t tilesX, tilesY;
    float invFrames;
    float gateLo, gateHi;
};
struct FreqMergePc {
    std::int32_t tilesX, tilesY;
    float robustnessNorm, readNoise, maxMotionNorm, gateLo, gateHi, noiseSlope, noiseOffset, black, frameInv, spectrumScale, shrink;
};
// A frame is gated out of a tile where the displacement statistic (1 = noise only) rises from kGateLow to kGateHigh; the
// reference's own bins are then shrunk by kGateShrink times their noise power. Tuned on the tmp bursts (scooter, crowd,
// 33TJ, Pixel street, birds, a 30-frame bay with moving water). 2.0 smoothed the single-frame grain best but also took
// 10-35% of the fine real ripple detail in moving water; 1.0 keeps most of it (75-99% of the finest band) with only mild
// grain, and 0.5 and below bring back the coarse grain of an unshrunk frame.
constexpr float kGateLow = 1.6f, kGateHigh = 2.6f, kGateShrink = 1.0f;
// Second displacement test, on the energy of the difference: the mean-based test cannot see a displaced fine pattern (small ripples,
// foliage) because its block mean cancels. The block's mean square difference over what noise alone gives (q, ~1 for noise) raises the
// statistic to 1 + kEnergyGain * (q - 1 - kEnergyDead) / kEnergyScale once q passes the dead zone; below it noise tiles are untouched.
// Tuned on a 30-frame bay with moving water against pixel4a, 33TJ and the street: far-water ripple detail kept 38% -> 43% at the
// finest band and rocks 82% -> 91%, for ~6% more noise in flat areas of long bursts (a more sensitive 0.4 / 0.2 keeps 58% but costs 18%).
constexpr float kEnergyGain = 1.0f, kEnergyDead = 1.0f, kEnergyScale = 0.5f;
struct BackwardPc {
    std::int32_t tilesX, tilesY;
    float frames, scale;
};
struct DeconvPc {
    std::int32_t tilesX, tilesY;
    float frames;
};
struct SeedPc {
    std::int32_t tilesX, tilesY;
    float frameInv, scale;
};
struct BorderPc {
    std::int32_t width, height;
    float minValue, frameInv;
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
    const auto add = [&](const std::string& name, Extent2D e, PixelStorage s, Lifetime life, const char* placeAt = nullptr) {
        if (placeAt) l.placements.push_back({name, "hdrq_spectra", placeAt});
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
    add("hdrq_ref_rgba", rgba, PixelStorage::RGBA16Float, Lifetime::Burst);
    add("hdrq_aligned_rgba", rgba, PixelStorage::RGBA16Float, Lifetime::Companion);
    add("hdrq_out_rgba", rgba, PixelStorage::RGBA32Float, Lifetime::Output);
    add("hdrq_ref_ft", spectrum, PixelStorage::RGBA16Float, Lifetime::Burst, "");
    add("hdrq_final_ft", spectrum, PixelStorage::RGBA16Float, Lifetime::Output, "end:hdrq_ref_ft");
    add("hdrq_rms", tiles, PixelStorage::RGBA32Float, Lifetime::Burst);
    add("hdrq_mismatch", tiles, PixelStorage::R32Float, Lifetime::Companion);
    add("hdrq_zmap", tiles, PixelStorage::R32Float, Lifetime::Companion);
    add("hdrq_neff", tiles, PixelStorage::R32Float, Lifetime::Output);
    add("hdrq_total_mismatch", tiles, PixelStorage::R32Float, Lifetime::Output);
    add("hdrp_accum", raw, PixelStorage::R32Float, Lifetime::Output);
    // Written only by finalize, after every pass: the reference and running spectra are dead by then and share their memory.
    add("hdrp_output", raw, PixelStorage::RGBA16Float, Lifetime::Output, "start:hdrq_ref_ft");
    add("hdrp_cfa", raw, PixelStorage::R32Float, Lifetime::Output, "end:hdrp_output");
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
    if (!alignsThisPass()) {
        align_.setCompanionGain(frameGain(slot));
        align_.recordCompanionPadded(c, rawU16, frame);
        return;
    }
    recordCompanionAlignCoarse(c, rawU16, frame, slot);
    recordCompanionAlignFinest(c, slot);
}

void HdrPlusFrequencyRecorder::recordCompanionAlignCoarse(VkCommandBuffer c, VkImageView rawU16,
                                                          const RawNormalization& frame, std::uint32_t slot) {
    if (!alignsThisPass()) throw std::logic_error("hdrplus frequency: coarse alignment outside an aligning pass");
    align_.setCompanionGain(frameGain(slot));
    align_.recordCompanionPrepare(c, rawU16, frame);
    for (std::uint32_t level = align_.levelCount(); --level > 0u;) align_.recordCompanionAlignLevel(c, level);
}

void HdrPlusFrequencyRecorder::recordCompanionAlignFinest(VkCommandBuffer c, std::uint32_t slot) {
    align_.recordCompanionAlignLevel(c, 0u);
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
                std::int32_t(geometry_.align.paddedWidth), std::int32_t(geometry_.align.paddedHeight), frameScale()};
    executor_.record(c, ShaderId::HdrqToRgba, {{ib(0, arena_.image("hdrp_ref_padded")), ib(1, rgba)}, {}}, &tr,
                     sizeof(tr), divUp(geometry_.rgbaWidth, 16), divUp(geometry_.rgbaHeight, 16));
    computeWriteBarrier(c);
    RmsPc rmsPc{std::int32_t(tx), std::int32_t(ty), 1.0f / frameScale()};
    executor_.record(c, ShaderId::HdrqRms, {{ib(0, rgba), ib(1, arena_.image("hdrq_rms"))}, {}}, &rmsPc, sizeof(rmsPc),
                     divUp(tx, 16), divUp(ty, 16));
    SeedPc sp{std::int32_t(tx), std::int32_t(ty), 1.0f / frameScale(), spectrumScale()};
    // The reference spectrum seeds both the reference and the running merge.
    // Each shifted pass has its own input/window; only the two identical
    // transforms within this pass are shared.
    executor_.record(c, ShaderId::HdrqSeedDft,
                     {{ib(0, rgba), ib(1, arena_.image("hdrq_ref_ft")), ib(2, arena_.image("hdrq_final_ft"))}, {}},
                     &sp, sizeof(sp), tx, ty);
    // Per-pass total mismatch starts at zero.
    VkMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                               VK_ACCESS_TRANSFER_WRITE_BIT};
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &toTransfer, 0,
                         nullptr, 0, nullptr);
    const VkClearColorValue zero{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(c, arena_.image("hdrq_total_mismatch").image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    vkCmdClearColorImage(c, arena_.image("hdrq_neff").image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
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
                  offset[0], offset[1], config_.frequencyAlignOnce ? 1 : 0, frameScale()};
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
    const auto& black = align_.referenceBlack();
    const float meanBlack = .25f * (black[0] + black[1] + black[2] + black[3]);
    MismatchPc mp{std::int32_t(tx), std::int32_t(ty), std::int32_t(geometry_.rgbaWidth), std::int32_t(geometry_.rgbaHeight),
                  noiseSlope_, noiseOffset_, meanBlack, 1.0f / frameScale(), kEnergyGain, kEnergyDead, kEnergyScale};
    executor_.record(c, ShaderId::HdrqMismatch,
                     {{ib(0, refRgba), ib(1, alignedRgba), ib(2, arena_.image("hdrq_rms")), ib(3, mismatch),
                       ib(4, arena_.image("hdrq_zmap"))},
                      {}},
                     &mp,
                     sizeof(mp), tx, ty);
    if (mark) {
        computeWriteBarrier(c);
        mark(5u);
    }
    // The aligned frame's spectrum is computed inside the merge pass (no stored aligned spectrum).
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
    MismatchNormPc np{std::int32_t(tx), std::int32_t(ty), 1.0f / float(frameCount), kGateLow, kGateHigh};
    executor_.record(c, ShaderId::HdrqMismatchNorm,
                     {{ib(0, mismatch), ib(1, arena_.image("hdrq_total_mismatch")), ib(3, arena_.image("hdrq_zmap")),
                       ib(4, arena_.image("hdrq_neff"))},
                      {bb(2, arena_.buffer("hdrq_mean"))}},
                     &np, sizeof(np), divUp(tx, 16), divUp(ty, 16));
    computeWriteBarrier(c);
    if (mark) mark(3u);
    const auto norms = hp::frequencyNorms(config_.strength);
    FreqMergePc fm{std::int32_t(tx), std::int32_t(ty), norms.robustnessNorm, norms.readNoise, norms.maxMotionNorm,
                   kGateLow, kGateHigh, noiseSlope_, noiseOffset_, meanBlack, 1.0f / frameScale(), spectrumScale(), kGateShrink};
    executor_.record(c, ShaderId::HdrqMerge,
                     {{ib(0, arena_.image("hdrq_ref_ft")), ib(1, alignedRgba),
                       ib(2, arena_.image("hdrq_final_ft")), ib(3, arena_.image("hdrq_rms")), ib(4, mismatch),
                       ib(6, arena_.image("hdrq_zmap"))},
                      {bb(5, arena_.buffer("hdrq_shift_table"))}},
                     &fm, sizeof(fm), tx, ty);
    computeWriteBarrier(c);
}

void HdrPlusFrequencyRecorder::recordEffectiveFramesCopy(VkCommandBuffer c, VkBuffer host) {
    const auto& neff = arena_.image("hdrq_neff");
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0,
                         nullptr);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {neff.extent.width, neff.extent.height, 1};
    vkCmdCopyImageToBuffer(c, neff.image, VK_IMAGE_LAYOUT_GENERAL, host, 1, &region);
    VkMemoryBarrier toHost{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &toHost, 0, nullptr, 0, nullptr);
}

std::pair<std::uint32_t, std::uint32_t> HdrPlusFrequencyRecorder::tileCount() const noexcept {
    return {geometry_.tilesX, geometry_.tilesY};
}

void HdrPlusFrequencyRecorder::recordPassFinish(VkCommandBuffer c, std::uint32_t frameCount) {
    const std::uint32_t tx = geometry_.tilesX, ty = geometry_.tilesY;
    const auto& finalFt = arena_.image("hdrq_final_ft");
    const auto& out = arena_.image("hdrq_out_rgba");
    // The running merge stores the burst's difference from frameCount x the reference spectrum (see hdrq_merge.comp), so the
    // reference spectrum is an input of both the deconvolution and the inverse transform.
    const auto& refFt = arena_.image("hdrq_ref_ft");
    DeconvPc dp{std::int32_t(tx), std::int32_t(ty), float(frameCount)};
    executor_.record(c, ShaderId::HdrqDeconvolute,
                     {{ib(0, finalFt), ib(1, arena_.image("hdrq_total_mismatch")), ib(2, refFt)}, {}}, &dp, sizeof(dp), tx, ty);
    computeWriteBarrier(c);
    BackwardPc bp{std::int32_t(tx), std::int32_t(ty), float(frameCount), spectrumScale()};
    executor_.record(c, ShaderId::HdrqBackwardDft, {{ib(0, finalFt), ib(1, out), ib(2, refFt)}, {}}, &bp, sizeof(bp), tx, ty);
    computeWriteBarrier(c);
    // Exposure control off: upstream passes black level -1, so the clamp floor is -2 * window.
    BorderPc bd{std::int32_t(geometry_.rgbaWidth), std::int32_t(geometry_.rgbaHeight), -2.0f, 1.0f / frameScale()};
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
