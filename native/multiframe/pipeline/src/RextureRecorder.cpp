#include <rawr/raw_gpu_pipeline/RextureRecorder.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <stdexcept>
#include <string>

#ifndef RX_PREP_PG
#define RX_PREP_PG 16
#endif
namespace rawr::raw_gpu_pipeline {
namespace {
constexpr std::uint32_t divUp(std::uint32_t x, std::uint32_t y) { return (x + y - 1u) / y; }
ImageBinding ib(std::uint32_t b, const ArenaImage& i) { return {b, i.view, VK_IMAGE_LAYOUT_GENERAL}; }

struct PreparePc {
    float black[4];
    float white;
    std::int32_t w, h, l2w, l2h, cfa, isBase;
    float alpha, beta;
    std::int32_t skip;
    float gain;  // reference exposure / frame exposure
};
struct PyramidPc {
    std::int32_t dstW, dstH;
};
struct TilesPc {
    std::int32_t ts, level;
    float alpha, beta;
};
struct CandPc {
    std::int32_t repeat;
    float scale;
    std::int32_t level;
    float alpha, beta;
    std::int32_t exactTies;
};
struct BlockPc {
    std::int32_t ts, radius, metric, exactTies;
};
struct LkPc {
    std::int32_t ts, which, iterations;
    float clipRadius, eps;
};
struct RobustPc {
    float alpha, beta;
    std::int32_t debug;
};
struct AccPc {
    float black[4];
    float white;
    std::int32_t w, h, cfa, debug;
    float frameWeight;
    std::int32_t frames;
    float gain0, gain1;  // reference exposure / frame exposure of the pass's frames
};
struct FinalPc {
    float black[4];
    float white;
    std::int32_t w, h, cfa, debug;
    std::int32_t row0, rcdY0, rcdRows;  // first row of the dispatch; rows held by the RCD work planes
};
struct FlagsPc {
    std::int32_t w, h;
};
// rx_lowcov_eaw: op 0 / 1 / 2 / 3, image size, strip core rows [r0, r1), rows computed by this dispatch (first row y0),
// first image row held by the two strip scratch images, and the merge's single-frame noise model.
struct LowCoveragePc {
    std::int32_t op, w, h, r0, r1, y0, rows, a1, a2;
    float alpha, beta;
};
// Rows of one strip of the grain pass (only tests change it: every height must give the same output).
constexpr std::int32_t kStripRows = 256, kStripHalo = 14, kStripRowsX1 = kStripRows + 24, kStripRowsX2 = kStripRows + 16;
// The fallback RCD of a strip also needs the rows around it: its five passes depend on each other over 7 rows.
constexpr std::int32_t kRcdHalo = 8, kRcdRows = kStripRows + 2 * kRcdHalo;
std::atomic<std::int32_t> gStripRows{kStripRows};
struct BoundPc {
    float black[4];
    float white;
    std::int32_t w, h, cfa;
    std::int32_t y0, rows, gate;
};
// Push constants of the five RCD passes (rx_rcd_common.glsl): planes hold the image rows [y0, y0 + rows).
struct RcdPc {
    std::uint32_t width, height, pattern;
    std::int32_t y0;
    float black[4], invRange[4], balance[4];
    std::int32_t rows, gate, pad1, pad2;
};
struct AxisPc {
    std::int32_t axis;
};
static_assert(sizeof(PreparePc) <= 128);
}  // namespace

ScratchLayout makeRextureScratchLayout(const MultiframeGeometry& g, bool debug) {
    ScratchLayout r{};
    r.geometry = g;
    auto img = [&](std::string n, Extent2D e, PixelStorage s, Lifetime l, std::string pool = {}, std::string placeAt = {}) {
        if (!e.width || !e.height) throw std::invalid_argument("rexture layout: empty image " + n);
        if (!pool.empty()) r.placements.push_back({n, std::move(pool), std::move(placeAt)});
        r.images.push_back({std::move(n), e, s, l});
    };
    for (const char* who : {"rx_base_", "rx_alt_"}) {
        const bool base = std::string(who) == "rx_base_";
        const Lifetime life = base ? Lifetime::Burst : Lifetime::Companion;
        for (std::size_t l = 0; l < 4u; ++l)
            img(std::string(who) + "l" + std::to_string(l), g.coarseToFine[l].image, PixelStorage::R32Float, life,
                (!base && l == 3u) ? "companion" : "");
        if (base) {
            img("rx_base_mean", g.guide, PixelStorage::RGBA32Float, life);
            img("rx_base_var", g.guide, PixelStorage::RGBA32Float, life);
        }
    }
    // The per-companion guide images share one pool with the base tensor (live only until the first companion is prepared,
    // and the base kernel's scratch is rx_alt_mean / rx_alt_var, placed after it) and with linear_output (written after the
    // last companion): alias, no extra memory.
    img("rx_base_tensor", g.guide, PixelStorage::RGBA32Float, Lifetime::Burst, "companion", "start:rx_alt_l3");
    img("rx_alt_mean", g.guide, PixelStorage::RGBA32Float, Lifetime::Companion, "companion", "end:rx_alt_l3,rx_base_tensor");
    img("rx_alt_var", g.guide, PixelStorage::RGBA32Float, Lifetime::Companion, "companion", "end:rx_alt_mean");
    img("rx_base_cov", g.guide, PixelStorage::RGBA32Float, Lifetime::Burst);
    if (debug) img("rx_dbg_qinv", g.output, PixelStorage::RGBA32Float, Lifetime::Burst);
    // Accumulators (packed, fp32): A = (sum w x [rgb], sum_frames W^2 [r]), B = (sum w [rgb], ... [g]), C = (... [b]).
    img("rx_acc_a", g.output, PixelStorage::RGBA32Float, Lifetime::Output);
    img("rx_acc_b", g.output, PixelStorage::RG32Float, Lifetime::Output);
    img("rx_acc_c", g.output, PixelStorage::RGBA16Float, Lifetime::Output);
    if (debug) {
        img("rx_dbg_ss2", g.output, PixelStorage::RGBA32Float, Lifetime::Output);
        img("rx_dbg_fin_out", g.output, PixelStorage::RGBA32Float, Lifetime::Output);  // (final rgb, neff)
        img("rx_dbg_fin_pre", g.output, PixelStorage::RGBA32Float, Lifetime::Output);  // (pre-lowcov rgb, conf)
    }
    // Robustness: the frame's weight map, and the burst-long support / cover maps (half resolution, fp32 sums).
    img("rx_er", g.guide, PixelStorage::R32Float, Lifetime::Companion);
    img("rx_er_b", g.guide, PixelStorage::R32Float, Lifetime::Companion);
    img("rx_support", g.guide, PixelStorage::R32Float, Lifetime::Burst);
    if (debug) {
        img("rx_dbg_pre", g.guide, PixelStorage::R32Float, Lifetime::Companion);
        img("rx_dbg_scale", Extent2D{g.coarseToFine[3].tilesX, g.coarseToFine[3].tilesY}, PixelStorage::R32Float,
            Lifetime::Companion);
    }
    // Alignment: per-level tile structure of the base (persistent) and the companion's flow / block-match cost.
    for (std::size_t l = 0; l < 4u; ++l) {
        const Extent2D tiles{g.coarseToFine[l].tilesX, g.coarseToFine[l].tilesY};
        img("rx_tiles_l" + std::to_string(l), tiles, PixelStorage::RGBA32Float, Lifetime::Burst);
        img("rx_flow_l" + std::to_string(l), tiles, PixelStorage::RG32Float, Lifetime::Companion);
        if (l == 3) img("rx_flow_l3_b", tiles, PixelStorage::RG32Float, Lifetime::Companion);  // second slot of a pair
        img("rx_bmcost_l" + std::to_string(l), tiles, PixelStorage::R32Float, Lifetime::Companion);
        if (debug) {
            img("rx_dbg_prior_l" + std::to_string(l), tiles, PixelStorage::RG32Float, Lifetime::Companion);
            img("rx_dbg_bm_l" + std::to_string(l), tiles, PixelStorage::RG32Float, Lifetime::Companion);
        }
    }
    img("linear_output", g.output, PixelStorage::RGBA16Float, Lifetime::Output, "companion", "start:rx_alt_l3");
    // Grain pass (rx_lowcov_eaw): the noise-level grid and the strip scratch, behind linear_output in the companion pool
    // (everything else in it is dead once the last companion is accumulated). Memory does not grow with the image height.
    img("rx_lc_m", Extent2D{(g.output.width + 3u) / 4u, (g.output.height + 3u) / 4u}, PixelStorage::R32Float, Lifetime::Output,
        "companion", "end:linear_output");
    img("rx_lc_halo", Extent2D{g.output.width, std::uint32_t(kStripHalo)}, PixelStorage::RGBA16Float, Lifetime::Output,
        "companion", "end:rx_lc_m");
    img("rx_lc_x1", Extent2D{g.output.width, std::uint32_t(kStripRowsX1)}, PixelStorage::RGBA16Float, Lifetime::Output,
        "companion", "end:rx_lc_halo");
    img("rx_lc_x2", Extent2D{g.output.width, std::uint32_t(kStripRowsX2)}, PixelStorage::RGBA16Float, Lifetime::Output,
        "companion", "end:rx_lc_x1");
    // Fallback demosaic (RCD of the base): computed per strip while finalizing, only near the 16 x 16 tiles flagged by
    // rx_fb_flags, so nothing of image size is kept for it. The fp32 work planes (RGB + direction field) alias the grain
    // pass's strip scratch (that pass runs after the last strip). The verification build runs RCD once on the whole frame.
    img("rx_fb_tiles", Extent2D{(g.output.width + 15u) / 16u, (g.output.height + 15u) / 16u}, PixelStorage::R32Float,
        Lifetime::Output);
    if (debug) {
        img("rx_rcd_work", g.output, PixelStorage::RGBA32Float, Lifetime::Burst);
        img("rx_rcd_dir", g.output, PixelStorage::RG32Float, Lifetime::Burst);
    } else {
        img("rx_rcd_work", Extent2D{g.output.width, std::uint32_t(std::min<std::uint32_t>(kRcdRows, g.output.height))},
            PixelStorage::RGBA32Float, Lifetime::Output, "companion", "start:rx_lc_halo");
        img("rx_rcd_dir", Extent2D{g.output.width, std::uint32_t(std::min<std::uint32_t>(kRcdRows, g.output.height))},
            PixelStorage::RG32Float, Lifetime::Output, "companion", "end:rx_rcd_work");
    }
    auto bytes = [](const ImageSpec& s) {
        return static_cast<std::uint64_t>(s.extent.width) * s.extent.height * bytesPerPixel(s.storage);
    };
    for (const auto& q : r.images) r.logicalImageBytes += bytes(q);
    return r;
}

RextureRecorder::RextureRecorder(ResourceArena& arena, VulkanExecutor& executor,
                                 rawr::raw_merge_rexture_gpu::Config config)
    : arena_(arena), executor_(executor), config_(config) {
    if (!rawr::raw_merge_rexture_gpu::valid(config_)) throw std::invalid_argument("rexture recorder: invalid config");
}

void RextureRecorder::recordPrepare(VkCommandBuffer c, bool base, VkImageView rawU16,
                                    const MultiframeFrameParameters& f, float gain) const {
    if (!rawU16) throw std::invalid_argument("rexture recorder: null RAW16 view");
    const std::string who = base ? "rx_base_" : "rx_alt_";
    const auto& l3 = arena_.image(who + "l3");
    const auto& l2 = arena_.image(who + "l2");
    const auto& guide = arena_.image(who + "mean");
    const auto& norm = f.normalization;
    if (!(norm.whiteLevel > *std::max_element(norm.blackByPhase.begin(), norm.blackByPhase.end())))
        throw std::invalid_argument("rexture recorder: invalid black/white");
    PreparePc p{};
    std::copy(norm.blackByPhase.begin(), norm.blackByPhase.end(), p.black);
    p.white = norm.whiteLevel;
    p.w = std::int32_t(l3.extent.width);
    p.h = std::int32_t(l3.extent.height);
    p.l2w = std::int32_t(l2.extent.width);
    p.l2h = std::int32_t(l2.extent.height);
    p.cfa = std::int32_t(config_.cfa);
    p.isBase = base ? 1 : 0;
    p.alpha = config_.noiseAlpha;
    p.beta = config_.noiseBeta;
    p.skip = std::getenv("RX_PREP_SKIP") ? std::atoi(std::getenv("RX_PREP_SKIP")) : 0;
    p.gain = base ? 1.0f : gain;
    ImageBinding raw{0, rawU16, VK_IMAGE_LAYOUT_GENERAL};
    // One workgroup per 16 x 16 guide positions (32 x 32 raw pixels).
    executor_.record(c, ShaderId::RxFramePrep,
                     {{raw, ib(1, l3), ib(2, l2), ib(3, arena_.image(who + "mean")), ib(4, arena_.image(who + "var")),
                       ib(5, arena_.image("rx_base_tensor"))},
                      {}},
                     &p, sizeof(p), divUp(guide.extent.width, RX_PREP_PG), divUp(guide.extent.height, RX_PREP_PG));
    computeWriteBarrier(c);
    if (mark_) mark_("prep");
    recordPyramid(c, base);
}

void RextureRecorder::recordPyramid(VkCommandBuffer c, bool base) const {
    const std::string who = base ? "rx_base_" : "rx_alt_";
    for (int l = 1; l >= 0; --l) {  // l2 -> l1, then l1 -> l0
        const auto& src = arena_.image(who + "l" + std::to_string(l + 1));
        const auto& dst = arena_.image(who + "l" + std::to_string(l));
        PyramidPc p{std::int32_t(dst.extent.width), std::int32_t(dst.extent.height)};
        executor_.record(c, ShaderId::RxPyramid, {{ib(0, src), ib(1, dst)}, {}}, &p, sizeof(p),
                         divUp(dst.extent.width, 8), divUp(dst.extent.height, 8));
        computeWriteBarrier(c);
        if (mark_) mark_(l == 1 ? "pyr_l1" : "pyr_l0");
    }
}

}  // namespace rawr::raw_gpu_pipeline

namespace rawr::raw_gpu_pipeline {
void RextureRecorder::recordBaseKernel(VkCommandBuffer c, bool debug) const {
    const auto& tensor = arena_.image("rx_base_tensor");
    const auto& tmpA = arena_.image("rx_alt_mean");  // scratch: free until the first companion is prepared
    const auto& tmpB = arena_.image("rx_alt_var");
    const auto& cov = arena_.image("rx_base_cov");
    const std::uint32_t gx = divUp(tensor.extent.width, 16), gy = divUp(tensor.extent.height, 16);
    AxisPc vertical{0}, horizontal{1};
    executor_.record(c, ShaderId::RxKernelBlur, {{ib(0, tensor), ib(1, tmpA)}, {}}, &vertical, sizeof(vertical), gx, gy);
    computeWriteBarrier(c);
    if (mark_) mark_("blur_v");
    executor_.record(c, ShaderId::RxKernelBlur, {{ib(0, tmpA), ib(1, tmpB)}, {}}, &horizontal, sizeof(horizontal), gx, gy);
    computeWriteBarrier(c);
    if (mark_) mark_("blur_h");
    executor_.record(c, ShaderId::RxKernelCov, {{ib(0, tmpB), ib(1, tensor), ib(2, cov)}, {}}, nullptr, 0, gx, gy);
    computeWriteBarrier(c);
    if (mark_) mark_("cov");
    if (debug) {
        const auto& q = arena_.image("rx_dbg_qinv");
        executor_.record(c, ShaderId::RxQinvDebug, {{ib(0, cov), ib(1, q)}, {}}, nullptr, 0, divUp(q.extent.width, 16),
                         divUp(q.extent.height, 16));
        computeWriteBarrier(c);
    }
}
}  // namespace rawr::raw_gpu_pipeline

namespace rawr::raw_gpu_pipeline {
namespace {
// Reference alignment configuration (coarse l = 0 .. fine l = 3): tile size, search radius, metric (1 = L2 clamped,
// 0 = L1 zero outside) and the flow scale from the next coarser level.
constexpr std::uint32_t kTileSize[4] = {8, 16, 16, 16};
constexpr std::uint32_t kRadius[4] = {4, 4, 4, 1};
constexpr std::int32_t kMetric[4] = {1, 1, 1, 0};
constexpr std::uint32_t kUpsampleScale[4] = {0, 4, 4, 2};
constexpr std::int32_t kLkIterations = 3;
constexpr float kHessianEps = 1.0e-10f;

void copyFlow(VkCommandBuffer c, const ArenaImage& src, const ArenaImage& dst) {
    VkMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &b, 0, nullptr,
                         0, nullptr);
    VkImageCopy r{};
    r.srcSubresource = r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.extent = {src.extent.width, src.extent.height, 1};
    vkCmdCopyImage(c, src.image, VK_IMAGE_LAYOUT_GENERAL, dst.image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    VkMemoryBarrier a{};
    a.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    a.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    a.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &a, 0, nullptr,
                         0, nullptr);
}
}  // namespace

void RextureRecorder::recordBaseTiles(VkCommandBuffer c) const {
    for (std::uint32_t l = 0; l < 4u; ++l) {
        const auto& ref = arena_.image("rx_base_l" + std::to_string(l));
        const auto& tiles = arena_.image("rx_tiles_l" + std::to_string(l));
        TilesPc p{std::int32_t(kTileSize[l]), std::int32_t(l), config_.noiseAlpha, config_.noiseBeta};
        executor_.record(c, ShaderId::RxRefTiles, {{ib(0, ref), ib(1, tiles)}, {}}, &p, sizeof(p), tiles.extent.width,
                         tiles.extent.height);
        computeWriteBarrier(c);
    }
    if (mark_) mark_("ref_tiles");
}

void RextureRecorder::recordAlignmentLevel(VkCommandBuffer c, std::uint32_t l, bool debug, std::uint32_t slot) const {
    if (l >= 4u) throw std::invalid_argument("rexture recorder: invalid alignment level");
    const std::string ls = std::to_string(l);
    const auto& ref = arena_.image("rx_base_l" + ls);
    const auto& mov = arena_.image("rx_alt_l" + ls);
    const auto& flow = arena_.image(l == 3 && slot == 1 ? std::string("rx_flow_l3_b") : "rx_flow_l" + ls);
    const auto& tiles = arena_.image("rx_tiles_l" + ls);
    const std::uint32_t tx = flow.extent.width, ty = flow.extent.height;
    if (l == 0) {
        if (tx * ty > 1024u) throw std::runtime_error("rexture recorder: coarsest alignment grid too large");
        const VkClearColorValue zero{};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(c, flow.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
        VkMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b, 0,
                             nullptr, 0, nullptr);
    } else {
        const auto& coarse = arena_.image("rx_flow_l" + std::to_string(l - 1));
        const std::uint32_t coarseTs = kTileSize[l - 1];
        CandPc p{std::int32_t((kUpsampleScale[l] * coarseTs) / 16u), float(kUpsampleScale[l]), std::int32_t(l),
                 config_.noiseAlpha, config_.noiseBeta, debug ? 1 : 0};
        executor_.record(c, ShaderId::RxCandSelect, {{ib(0, ref), ib(1, mov), ib(2, coarse), ib(3, flow)}, {}}, &p,
                         sizeof(p), tx, ty);
        computeWriteBarrier(c);
    }
    if (mark_ && l > 0) mark_("cand");
    if (debug) copyFlow(c, flow, arena_.image("rx_dbg_prior_l" + ls));
    BlockPc bp{std::int32_t(kTileSize[l]), std::int32_t(kRadius[l]), kMetric[l], debug ? 1 : 0};
    executor_.record(c, ShaderId::RxBlockMatch,
                     {{ib(0, ref), ib(1, mov), ib(2, flow), ib(3, tiles), ib(4, arena_.image("rx_bmcost_l" + ls))}, {}}, &bp,
                     sizeof(bp), tx, ty);
    computeWriteBarrier(c);
    if (mark_) mark_("bm");
    if (debug) copyFlow(c, flow, arena_.image("rx_dbg_bm_l" + ls));
    // Levels above the coarsest: mode 2 and mode 1 tiles are independent, one dispatch. The coarsest level fills the
    // unconstrained tiles from the constrained ones between the two.
    LkPc lk{std::int32_t(kTileSize[l]), l == 0 ? 2 : 3, kLkIterations, float(kRadius[l]), kHessianEps};
    executor_.record(c, ShaderId::RxLk, {{ib(0, ref), ib(1, mov), ib(2, flow), ib(3, tiles)}, {}}, &lk, sizeof(lk), tx, ty);
    computeWriteBarrier(c);
    if (mark_) mark_("lk");
    if (l == 0) {
        executor_.record(c, ShaderId::RxFill, {{ib(0, flow), ib(1, tiles)}, {}}, nullptr, 0, 1, 1);
        computeWriteBarrier(c);
        if (mark_) mark_("fill");
        lk.which = 1;
        executor_.record(c, ShaderId::RxLk, {{ib(0, ref), ib(1, mov), ib(2, flow), ib(3, tiles)}, {}}, &lk, sizeof(lk), tx,
                         ty);
        computeWriteBarrier(c);
        if (mark_) mark_("lk1");
    }
}
}  // namespace rawr::raw_gpu_pipeline

namespace rawr::raw_gpu_pipeline {
void RextureRecorder::recordBurstClear(VkCommandBuffer c) const {
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkClearColorValue zero{};
    vkCmdClearColorImage(c, arena_.image("rx_support").image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    for (const char* n : {"rx_acc_a", "rx_acc_b", "rx_acc_c", "rx_dbg_ss2"}) {
        try {
            vkCmdClearColorImage(c, arena_.image(n).image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
        } catch (const std::out_of_range&) {  // debug-only image
        }
    }
    VkMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b, 0, nullptr, 0,
                         nullptr);
}

void RextureRecorder::recordRobustness(VkCommandBuffer c, bool debug, std::uint32_t slot) const {
    const auto& er = arena_.image(slot == 1 ? "rx_er_b" : "rx_er");
    RobustPc p{config_.noiseAlpha, config_.noiseBeta, debug ? 1 : 0};
    const ArenaImage& dbgPre = debug ? arena_.image("rx_dbg_pre") : er;       // never written without debug
    const ArenaImage& dbgScale = debug ? arena_.image("rx_dbg_scale") : er;
    executor_.record(c, ShaderId::RxRobust,
                     {{ib(0, arena_.image(slot == 1 ? "rx_flow_l3_b" : "rx_flow_l3")), ib(1, arena_.image("rx_base_mean")),
                       ib(2, arena_.image("rx_base_var")), ib(3, arena_.image("rx_alt_mean")),
                       ib(4, arena_.image("rx_alt_var")), ib(5, er), ib(6, arena_.image("rx_support")),
                       ib(7, dbgPre), ib(8, dbgScale)},
                      {}},
                     &p, sizeof(p), divUp(er.extent.width, 16), divUp(er.extent.height, 16));
    computeWriteBarrier(c);
    if (mark_) mark_("robust");
}
}  // namespace rawr::raw_gpu_pipeline

namespace rawr::raw_gpu_pipeline {
namespace {
ImageBinding raw8(VkImageView v) { return {8, v, VK_IMAGE_LAYOUT_GENERAL}; }
}  // namespace
void RextureRecorder::recordAccumulate(VkCommandBuffer c, VkImageView rawU16, const MultiframeFrameParameters& f,
                                       bool debug, float gain) const {
    if (!rawU16) throw std::invalid_argument("rexture recorder: null RAW16 view");
    const auto& a = arena_.image("rx_acc_a");
    AccPc p{};
    std::copy(f.normalization.blackByPhase.begin(), f.normalization.blackByPhase.end(), p.black);
    p.white = f.normalization.whiteLevel;
    p.w = std::int32_t(a.extent.width);
    p.h = std::int32_t(a.extent.height);
    p.cfa = std::int32_t(config_.cfa);
    p.debug = debug ? 1 : 0;
    p.frameWeight = 1.0f;
    p.frames = 1;
    p.gain0 = p.gain1 = gain;
    ImageBinding raw{0, rawU16, VK_IMAGE_LAYOUT_GENERAL};
    const ArenaImage& ss2 = debug ? arena_.image("rx_dbg_ss2") : a;   // never touched without debug
    executor_.record(c, ShaderId::RxAccumulate,
                     {{raw, ib(1, arena_.image("rx_flow_l3")), ib(2, arena_.image("rx_er")), ib(3, arena_.image("rx_base_cov")),
                       ib(4, a), ib(5, arena_.image("rx_acc_b")), ib(6, arena_.image("rx_acc_c")), ib(7, ss2),
                       raw8(rawU16), ib(9, arena_.image("rx_flow_l3")), ib(10, arena_.image("rx_er"))},
                      {}},
                     &p, sizeof(p), divUp(a.extent.width, 16), divUp(a.extent.height, 16));
    computeWriteBarrier(c);
    if (mark_) mark_("accumulate");
}
}  // namespace rawr::raw_gpu_pipeline

namespace rawr::raw_gpu_pipeline {
void RextureRecorder::recordFinalize(VkCommandBuffer c, VkImageView rawU16, const MultiframeFrameParameters& f,
                                     bool debug) const {
    if (!rawU16) throw std::invalid_argument("rexture recorder: null RAW16 view");
    const auto& out = arena_.image("linear_output");
    const std::int32_t w = std::int32_t(out.extent.width), h = std::int32_t(out.extent.height);
    const auto& tiles = arena_.image("rx_fb_tiles");
    if (debug) {  // the verification build keeps the whole-frame RCD of recordBaseDemosaic and every tile on
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        const VkClearColorValue one{{1.0f, 1.0f, 1.0f, 1.0f}};
        vkCmdClearColorImage(c, tiles.image, VK_IMAGE_LAYOUT_GENERAL, &one, 1, &range);
        VkMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b, 0, nullptr, 0,
                             nullptr);
    } else {
        const FlagsPc fp{w, h};
        executor_.record(c, ShaderId::RxFbFlags,
                         {{ib(0, arena_.image("rx_acc_a")), ib(1, arena_.image("rx_acc_b")), ib(2, arena_.image("rx_acc_c")),
                           ib(3, arena_.image("rx_support")), ib(4, tiles)},
                          {}},
                         &fp, sizeof(fp), divUp(std::uint32_t(w), 16), divUp(std::uint32_t(h), 16));
        computeWriteBarrier(c);
        if (mark_) mark_("fb_flags");
    }
    FinalPc p{};
    std::copy(f.normalization.blackByPhase.begin(), f.normalization.blackByPhase.end(), p.black);
    p.white = f.normalization.whiteLevel;
    p.w = w;
    p.h = h;
    p.cfa = std::int32_t(config_.cfa);
    p.debug = debug ? 1 : 0;
    ImageBinding raw{0, rawU16, VK_IMAGE_LAYOUT_GENERAL};
    const auto& accA = arena_.image("rx_acc_a");
    const auto& accC = arena_.image("rx_acc_c");
    const ArenaImage& dbgOut = debug ? arena_.image("rx_dbg_fin_out") : accA;  // never written without debug
    const ArenaImage& dbgPre = debug ? arena_.image("rx_dbg_fin_pre") : accA;
    const auto& noiseGrid = arena_.image("rx_lc_m");
    const auto& work = arena_.image("rx_rcd_work");
    // strips of whole 16 x 16 groups: RCD (fallback tiles only) on the strip's rows plus a margin, then the final pass
    const std::int32_t strip = debug ? h : std::max(16, gStripRows / 16 * 16);
    // one descriptor set per dispatch and program, from the executor's pool of 1024 sets per chunk
    if ((h + strip - 1) / strip > 1000) throw std::invalid_argument("rexture recorder: image too tall for the final pass");
    for (std::int32_t r0 = 0; r0 < h; r0 += strip) {
        const std::int32_t r1 = std::min(r0 + strip, h);
        if (debug) {
            p.rcdY0 = 0;
            p.rcdRows = h;
        } else {
            const std::int32_t y0 = std::max(r0 - kRcdHalo, 0), y1 = std::min(r1 + kRcdHalo, h);
            if (y1 - y0 > std::int32_t(work.extent.height)) throw std::logic_error("rexture recorder: RCD strip too tall");
            recordRcd(c, rawU16, f, y0, y1 - y0, true);
            if (mark_) mark_("rcd");
            p.rcdY0 = y0;
            p.rcdRows = y1 - y0;
        }
        p.row0 = r0;
        executor_.record(c, ShaderId::RxFinalize,
                         {{raw, ib(1, arena_.image("rx_base_cov")), ib(2, accA), ib(3, arena_.image("rx_acc_b")), ib(4, accC),
                           ib(5, arena_.image("rx_support")), ib(6, work), ib(7, out), ib(8, dbgOut), ib(9, dbgPre),
                           ib(10, noiseGrid), ib(11, tiles)},
                          {}},
                         &p, sizeof(p), divUp(std::uint32_t(w), 16), divUp(std::uint32_t(r1 - r0), 16));
        computeWriteBarrier(c);
        if (mark_) mark_("final");
    }
    recordGrainPass(c);
}

std::uint32_t RextureRecorder::finalizeMarks(std::uint32_t height, bool debug) noexcept {
    const std::uint32_t strip = debug ? height : std::uint32_t(std::max(16, gStripRows / 16 * 16));
    const std::uint32_t strips = (height + strip - 1u) / strip;
    return (debug ? 0u : 1u) + strips * (debug ? 1u : 2u) + 1u;  // flags, per strip (rcd, final), grain
}

void RextureRecorder::setStripRowsForTesting(std::uint32_t rows) noexcept {
    gStripRows = rows == 0 ? kStripRows : std::clamp(std::int32_t(rows), 16, kStripRows);
}

// Evens out the grain where the merge kept few frames, strip by strip, in place on linear_output (see rx_lowcov_eaw.comp).
// A strip core [r0, r1) needs x0 rows +-14, x1 rows +-12 and x2 rows +-8, clamped to the image like the filter itself.
void RextureRecorder::recordGrainPass(VkCommandBuffer c) const {
    const auto& out = arena_.image("linear_output");
    const std::int32_t w = std::int32_t(out.extent.width), h = std::int32_t(out.extent.height);
    const std::int32_t strip = gStripRows;
    // four dispatches per strip, one descriptor set each, from the executor's pool of 1024 sets per program and chunk
    if ((h + strip - 1) / strip * 4 > 1000) throw std::invalid_argument("rexture recorder: image too tall for the grain pass");
    const auto& noiseGrid = arena_.image("rx_lc_m");
    const auto& halo = arena_.image("rx_lc_halo");
    const auto& x1 = arena_.image("rx_lc_x1");
    const auto& x2 = arena_.image("rx_lc_x2");
    const auto run = [&](std::int32_t op, std::int32_t r0, std::int32_t r1, std::int32_t y0, std::int32_t rows,
                         std::int32_t a1, std::int32_t a2) {
        const LowCoveragePc q{op, w, h, r0, r1, y0, rows, a1, a2, config_.noiseAlpha, config_.noiseBeta};
        executor_.record(c, ShaderId::RxLowcovEaw, {{ib(0, out), ib(1, halo), ib(2, x1), ib(3, x2), ib(4, noiseGrid)}, {}}, &q,
                         sizeof(q), divUp(std::uint32_t(w), 16), divUp(std::uint32_t(rows), 16));
        computeWriteBarrier(c);
    };
    for (std::int32_t r0 = 0; r0 < h; r0 += strip) {
        const std::int32_t r1 = std::min(r0 + strip, h);
        const std::int32_t a1 = std::max(r0 - 12, 0), b1 = std::min(r1 + 12, h);
        const std::int32_t a2 = std::max(r0 - 8, 0), b2 = std::min(r1 + 8, h);
        run(0, r0, r1, a1, b1 - a1, a1, a2);                                   // x1 = A1(x0)
        run(1, r0, r1, a2, b2 - a2, a1, a2);                                   // x2 = A2(x1)
        if (r1 < h) run(3, r0, r1, std::max(r1 - kStripHalo, r0), std::min(kStripHalo, r1 - r0), a1, a2);  // next strip's halo
        run(2, r0, r1, r0, r1 - r0, a1, a2);                                   // out = x3 + g (x0 - x3), in place
    }
    if (mark_) mark_("grain");
}
}  // namespace rawr::raw_gpu_pipeline

namespace rawr::raw_gpu_pipeline {
void RextureRecorder::recordBaseDemosaic(VkCommandBuffer c, VkImageView rawU16, const MultiframeFrameParameters& f) const {
    // Verification build only: RCD + bound of the whole frame, ungated, into the full-size work planes.
    recordRcd(c, rawU16, f, 0, std::int32_t(arena_.image("rx_rcd_work").extent.height), false);
}

// fp32 RCD of the image rows [y0, y0 + rows) into rx_rcd_work (direction field in rx_rcd_dir), then the native-range bound
// in place. gate: only near the tiles flagged in rx_fb_tiles.
void RextureRecorder::recordRcd(VkCommandBuffer c, VkImageView rawU16, const MultiframeFrameParameters& f, std::int32_t y0,
                                std::int32_t rows, bool gate) const {
    if (!rawU16) throw std::invalid_argument("rexture recorder: null RAW16 view");
    const auto& out = arena_.image("rx_rcd_work");
    const auto& dir = arena_.image("rx_rcd_dir");
    const auto& tiles = arena_.image("rx_fb_tiles");
    if (rows <= 0 || rows > std::int32_t(out.extent.height)) throw std::invalid_argument("rexture recorder: invalid RCD rows");
    RcdPc p{};
    p.width = out.extent.width;
    p.height = arena_.image("linear_output").extent.height;
    p.pattern = static_cast<std::uint32_t>(config_.cfa);
    p.y0 = y0;
    p.rows = rows;
    p.gate = gate ? 1 : 0;
    for (int i = 0; i < 4; ++i) {
        p.black[i] = f.normalization.blackByPhase[i];
        p.invRange[i] = 1.0f / (f.normalization.whiteLevel - f.normalization.blackByPhase[i]);
    }
    for (int i = 0; i < 3; ++i) p.balance[i] = config_.whiteBalance[i];
    p.balance[3] = 1.0f;
    ImageBinding raw{0, rawU16, VK_IMAGE_LAYOUT_GENERAL};
    const DispatchBindings b{{raw, ib(1, dir), ib(2, out), ib(3, tiles)}, {}};
    const std::uint32_t half = (p.width + 1u) / 2u, n = std::uint32_t(rows);
    // production workgroup sizes of the library: direction 4x32, green 4x32, diagonal 32x8 (half width), green sites 8x32
    // (half width), export 4x32
    executor_.record(c, ShaderId::RxRcdDirection, b, &p, sizeof(p), divUp(p.width, 4), divUp(n, 32));
    computeWriteBarrier(c);
    if (mark_ && !gate) mark_("rcd_dir");
    executor_.record(c, ShaderId::RxRcdGreen, b, &p, sizeof(p), divUp(p.width, 4), divUp(n, 32));
    computeWriteBarrier(c);
    if (mark_ && !gate) mark_("rcd_green");
    executor_.record(c, ShaderId::RxRcdDiagonal, b, &p, sizeof(p), divUp(half, 32), divUp(n, 8));
    computeWriteBarrier(c);
    if (mark_ && !gate) mark_("rcd_diag");
    executor_.record(c, ShaderId::RxRcdGreenSites, b, &p, sizeof(p), divUp(half, 8), divUp(n, 32));
    computeWriteBarrier(c);
    if (mark_ && !gate) mark_("rcd_sites");
    executor_.record(c, ShaderId::RxRcdExport, b, &p, sizeof(p), divUp(p.width, 4), divUp(n, 32));
    computeWriteBarrier(c);
    if (mark_ && !gate) mark_("rcd_export");
    BoundPc bp{};
    std::copy(f.normalization.blackByPhase.begin(), f.normalization.blackByPhase.end(), bp.black);
    bp.white = f.normalization.whiteLevel;
    bp.w = std::int32_t(p.width);
    bp.h = std::int32_t(p.height);
    bp.cfa = std::int32_t(config_.cfa);
    bp.y0 = y0;
    bp.rows = rows;
    bp.gate = gate ? 1 : 0;
    executor_.record(c, ShaderId::RxRcdBound, {{raw, ib(1, out), ib(2, tiles)}, {}}, &bp, sizeof(bp), divUp(p.width, 16),
                     divUp(n, 16));
    computeWriteBarrier(c);
    if (mark_ && !gate) mark_("rcd_bound");
}
}  // namespace rawr::raw_gpu_pipeline

namespace rawr::raw_gpu_pipeline {
void RextureRecorder::recordAccumulatePair(VkCommandBuffer c, VkImageView rawSlot0, VkImageView rawSlot1,
                                           const MultiframeFrameParameters& f, float gain0, float gain1) const {
    if (!rawSlot0 || !rawSlot1) throw std::invalid_argument("rexture recorder: null RAW16 view");
    const auto& a = arena_.image("rx_acc_a");
    AccPc p{};
    std::copy(f.normalization.blackByPhase.begin(), f.normalization.blackByPhase.end(), p.black);
    p.white = f.normalization.whiteLevel;
    p.w = std::int32_t(a.extent.width);
    p.h = std::int32_t(a.extent.height);
    p.cfa = std::int32_t(config_.cfa);
    p.debug = 0;
    p.frameWeight = 1.0f;
    p.frames = 2;
    p.gain0 = gain0;
    p.gain1 = gain1;
    ImageBinding raw{0, rawSlot0, VK_IMAGE_LAYOUT_GENERAL};
    executor_.record(c, ShaderId::RxAccumulate,
                     {{raw, ib(1, arena_.image("rx_flow_l3")), ib(2, arena_.image("rx_er")), ib(3, arena_.image("rx_base_cov")),
                       ib(4, a), ib(5, arena_.image("rx_acc_b")), ib(6, arena_.image("rx_acc_c")), ib(7, a), raw8(rawSlot1),
                       ib(9, arena_.image("rx_flow_l3_b")), ib(10, arena_.image("rx_er_b"))},
                      {}},
                     &p, sizeof(p), divUp(a.extent.width, 16), divUp(a.extent.height, 16));
    computeWriteBarrier(c);
    if (mark_) mark_("accumulate");
}
}  // namespace rawr::raw_gpu_pipeline
