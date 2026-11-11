#include <rawr/raw_gpu_pipeline/RextureRecorder.h>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace rawr::raw_gpu_pipeline {
namespace {
constexpr std::uint32_t divUp(std::uint32_t x, std::uint32_t y) { return (x + y - 1u) / y; }
ImageBinding ib(std::uint32_t b, const ArenaImage& i) { return {b, i.view, VK_IMAGE_LAYOUT_GENERAL}; }

struct PreparePc {
    float black[4];
    float white;
    std::int32_t w, h, l2w, l2h, cfa, isBase;
    float alpha, beta;
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
};
struct BlockPc {
    std::int32_t ts, radius, metric;
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
};
struct FinalPc {
    float black[4];
    float white;
    std::int32_t w, h, cfa, debug;
    float alpha, beta;
};
struct BoundPc {
    float black[4];
    float white;
    std::int32_t w, h, cfa;
};
struct AxisPc {
    std::int32_t axis;
};
static_assert(sizeof(PreparePc) <= 128);
}  // namespace

ScratchLayout makeRextureScratchLayout(const MultiframeGeometry& g, bool debug) {
    ScratchLayout r{};
    r.geometry = g;
    auto img = [&](std::string n, Extent2D e, PixelStorage s, Lifetime l) {
        if (!e.width || !e.height) throw std::invalid_argument("rexture layout: empty image " + n);
        r.images.push_back({std::move(n), e, s, l});
    };
    for (const char* who : {"rx_base_", "rx_alt_"}) {
        const bool base = std::string(who) == "rx_base_";
        const Lifetime life = base ? Lifetime::Burst : Lifetime::Companion;
        for (std::size_t l = 0; l < 4u; ++l)
            img(std::string(who) + "l" + std::to_string(l), g.coarseToFine[l].image, PixelStorage::R32Float, life);
        img(std::string(who) + "mean", g.guide, PixelStorage::RGBA32Float, life);
        img(std::string(who) + "var", g.guide, PixelStorage::RGBA32Float, life);
    }
    img("rx_base_tensor", g.guide, PixelStorage::RGBA32Float, Lifetime::Burst);
    img("rx_base_cov", g.guide, PixelStorage::RGBA32Float, Lifetime::Burst);
    img("rx_base_rcd", g.output, PixelStorage::RGBA32Float, Lifetime::Burst);  // fallback demosaic, fp32 work planes
    img("rx_rcd_dir", g.output, PixelStorage::RG32Float, Lifetime::Burst);       // RCD direction field (base pass only)
    if (debug) img("rx_dbg_qinv", g.output, PixelStorage::RGBA32Float, Lifetime::Burst);
    // Accumulators (packed, fp32): A = (sum w x [rgb], sum_frames W^2 [r]), B = (sum w [rgb], ... [g]), C = (... [b]).
    img("rx_acc_a", g.output, PixelStorage::RGBA32Float, Lifetime::Output);
    img("rx_acc_b", g.output, PixelStorage::RGBA32Float, Lifetime::Output);
    img("rx_acc_c", g.output, PixelStorage::R32Float, Lifetime::Output);
    if (debug) {
        img("rx_dbg_ss2", g.output, PixelStorage::RGBA32Float, Lifetime::Output);
        img("rx_dbg_fin_out", g.output, PixelStorage::RGBA32Float, Lifetime::Output);  // (final rgb, neff)
        img("rx_dbg_fin_pre", g.output, PixelStorage::RGBA32Float, Lifetime::Output);  // (pre-lowcov rgb, conf)
        img("rx_dbg_fin_w", g.output, PixelStorage::R32Float, Lifetime::Output);
    }
    // Robustness: the frame's weight map, and the burst-long support / cover maps (half resolution, fp32 sums).
    img("rx_er", g.guide, PixelStorage::R32Float, Lifetime::Companion);
    img("rx_support", g.guide, PixelStorage::R32Float, Lifetime::Burst);
    img("rx_cover", g.guide, PixelStorage::R32Float, Lifetime::Burst);
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
        img("rx_bmcost_l" + std::to_string(l), tiles, PixelStorage::R32Float, Lifetime::Companion);
        if (debug) {
            img("rx_dbg_prior_l" + std::to_string(l), tiles, PixelStorage::RG32Float, Lifetime::Companion);
            img("rx_dbg_bm_l" + std::to_string(l), tiles, PixelStorage::RG32Float, Lifetime::Companion);
        }
    }
    img("linear_output", g.output, PixelStorage::RGBA16Float, Lifetime::Output);
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
                                    const MultiframeFrameParameters& f) const {
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
    ImageBinding raw{0, rawU16, VK_IMAGE_LAYOUT_GENERAL};
    // One workgroup per 16 x 16 guide positions (32 x 32 raw pixels).
    executor_.record(c, ShaderId::RxFramePrep,
                     {{raw, ib(1, l3), ib(2, l2), ib(3, arena_.image(who + "mean")), ib(4, arena_.image(who + "var")),
                       ib(5, arena_.image("rx_base_tensor"))},
                      {}},
                     &p, sizeof(p), divUp(guide.extent.width, 16), divUp(guide.extent.height, 16));
    computeWriteBarrier(c);
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
    executor_.record(c, ShaderId::RxKernelBlur, {{ib(0, tmpA), ib(1, tmpB)}, {}}, &horizontal, sizeof(horizontal), gx, gy);
    computeWriteBarrier(c);
    executor_.record(c, ShaderId::RxKernelCov, {{ib(0, tmpB), ib(1, tensor), ib(2, cov)}, {}}, nullptr, 0, gx, gy);
    computeWriteBarrier(c);
    if (debug) {
        const auto& q = arena_.image("rx_dbg_qinv");
        executor_.record(c, ShaderId::RxQinvDebug, {{ib(0, cov), ib(1, q)}, {}}, nullptr, 0, divUp(q.extent.width, 16),
                         divUp(q.extent.height, 16));
        computeWriteBarrier(c);
    }
}
}  // namespace rawr::raw_gpu_pipeline

namespace rawr::raw_gpu_pipeline {
void RextureRecorder::recordBaseDemosaicBound(VkCommandBuffer c, VkImageView rawU16,
                                              const MultiframeFrameParameters& f) const {
    const auto& rcd = arena_.image("rx_base_rcd");
    BoundPc p{};
    std::copy(f.normalization.blackByPhase.begin(), f.normalization.blackByPhase.end(), p.black);
    p.white = f.normalization.whiteLevel;
    p.w = std::int32_t(rcd.extent.width);
    p.h = std::int32_t(rcd.extent.height);
    p.cfa = std::int32_t(config_.cfa);
    ImageBinding raw{0, rawU16, VK_IMAGE_LAYOUT_GENERAL};
    executor_.record(c, ShaderId::RxRcdBound, {{raw, ib(1, rcd)}, {}}, &p, sizeof(p), divUp(rcd.extent.width, 16),
                     divUp(rcd.extent.height, 16));
    computeWriteBarrier(c);
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
}

void RextureRecorder::recordAlignmentLevel(VkCommandBuffer c, std::uint32_t l, bool debug) const {
    if (l >= 4u) throw std::invalid_argument("rexture recorder: invalid alignment level");
    const std::string ls = std::to_string(l);
    const auto& ref = arena_.image("rx_base_l" + ls);
    const auto& mov = arena_.image("rx_alt_l" + ls);
    const auto& flow = arena_.image("rx_flow_l" + ls);
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
                 config_.noiseAlpha, config_.noiseBeta};
        executor_.record(c, ShaderId::RxCandSelect, {{ib(0, ref), ib(1, mov), ib(2, coarse), ib(3, flow)}, {}}, &p,
                         sizeof(p), tx, ty);
        computeWriteBarrier(c);
    }
    if (debug) copyFlow(c, flow, arena_.image("rx_dbg_prior_l" + ls));
    BlockPc bp{std::int32_t(kTileSize[l]), std::int32_t(kRadius[l]), kMetric[l]};
    executor_.record(c, ShaderId::RxBlockMatch,
                     {{ib(0, ref), ib(1, mov), ib(2, flow), ib(3, tiles), ib(4, arena_.image("rx_bmcost_l" + ls))}, {}}, &bp,
                     sizeof(bp), tx, ty);
    computeWriteBarrier(c);
    if (debug) copyFlow(c, flow, arena_.image("rx_dbg_bm_l" + ls));
    // Levels above the coarsest: mode 2 and mode 1 tiles are independent, one dispatch. The coarsest level fills the
    // unconstrained tiles from the constrained ones between the two.
    LkPc lk{std::int32_t(kTileSize[l]), l == 0 ? 2 : 3, kLkIterations, float(kRadius[l]), kHessianEps};
    executor_.record(c, ShaderId::RxLk, {{ib(0, ref), ib(1, mov), ib(2, flow), ib(3, tiles)}, {}}, &lk, sizeof(lk), tx, ty);
    computeWriteBarrier(c);
    if (l == 0) {
        executor_.record(c, ShaderId::RxFill, {{ib(0, flow), ib(1, tiles)}, {}}, nullptr, 0, 1, 1);
        computeWriteBarrier(c);
        lk.which = 1;
        executor_.record(c, ShaderId::RxLk, {{ib(0, ref), ib(1, mov), ib(2, flow), ib(3, tiles)}, {}}, &lk, sizeof(lk), tx,
                         ty);
        computeWriteBarrier(c);
    }
}
}  // namespace rawr::raw_gpu_pipeline

namespace rawr::raw_gpu_pipeline {
void RextureRecorder::recordBurstClear(VkCommandBuffer c) const {
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkClearColorValue zero{}, one{};
    one.float32[0] = 1.0f;
    vkCmdClearColorImage(c, arena_.image("rx_support").image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    vkCmdClearColorImage(c, arena_.image("rx_cover").image, VK_IMAGE_LAYOUT_GENERAL, &one, 1, &range);
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

void RextureRecorder::recordRobustness(VkCommandBuffer c, bool debug) const {
    const auto& er = arena_.image("rx_er");
    RobustPc p{config_.noiseAlpha, config_.noiseBeta, debug ? 1 : 0};
    const ArenaImage& dbgPre = debug ? arena_.image("rx_dbg_pre") : er;       // never written without debug
    const ArenaImage& dbgScale = debug ? arena_.image("rx_dbg_scale") : er;
    executor_.record(c, ShaderId::RxRobust,
                     {{ib(0, arena_.image("rx_flow_l3")), ib(1, arena_.image("rx_base_mean")),
                       ib(2, arena_.image("rx_base_var")), ib(3, arena_.image("rx_alt_mean")),
                       ib(4, arena_.image("rx_alt_var")), ib(5, er), ib(6, arena_.image("rx_support")),
                       ib(7, arena_.image("rx_cover")), ib(8, dbgPre), ib(9, dbgScale)},
                      {}},
                     &p, sizeof(p), divUp(er.extent.width, 16), divUp(er.extent.height, 16));
    computeWriteBarrier(c);
}
}  // namespace rawr::raw_gpu_pipeline

namespace rawr::raw_gpu_pipeline {
void RextureRecorder::recordAccumulate(VkCommandBuffer c, VkImageView rawU16, const MultiframeFrameParameters& f,
                                       bool debug) const {
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
    ImageBinding raw{0, rawU16, VK_IMAGE_LAYOUT_GENERAL};
    const ArenaImage& ss2 = debug ? arena_.image("rx_dbg_ss2") : a;   // never touched without debug
    executor_.record(c, ShaderId::RxAccumulate,
                     {{raw, ib(1, arena_.image("rx_flow_l3")), ib(2, arena_.image("rx_er")), ib(3, arena_.image("rx_base_cov")),
                       ib(4, a), ib(5, arena_.image("rx_acc_b")), ib(6, arena_.image("rx_acc_c")), ib(7, ss2)},
                      {}},
                     &p, sizeof(p), divUp(a.extent.width, 16), divUp(a.extent.height, 16));
    computeWriteBarrier(c);
}
}  // namespace rawr::raw_gpu_pipeline

namespace rawr::raw_gpu_pipeline {
void RextureRecorder::recordFinalize(VkCommandBuffer c, VkImageView rawU16, const MultiframeFrameParameters& f,
                                     bool debug) const {
    if (!rawU16) throw std::invalid_argument("rexture recorder: null RAW16 view");
    const auto& out = arena_.image("linear_output");
    FinalPc p{};
    std::copy(f.normalization.blackByPhase.begin(), f.normalization.blackByPhase.end(), p.black);
    p.white = f.normalization.whiteLevel;
    p.w = std::int32_t(out.extent.width);
    p.h = std::int32_t(out.extent.height);
    p.cfa = std::int32_t(config_.cfa);
    p.debug = debug ? 1 : 0;
    p.alpha = config_.noiseAlpha;
    p.beta = config_.noiseBeta;
    ImageBinding raw{0, rawU16, VK_IMAGE_LAYOUT_GENERAL};
    const auto& accA = arena_.image("rx_acc_a");
    const auto& accC = arena_.image("rx_acc_c");
    const ArenaImage& dbgOut = debug ? arena_.image("rx_dbg_fin_out") : accA;  // never written without debug
    const ArenaImage& dbgPre = debug ? arena_.image("rx_dbg_fin_pre") : accA;
    const ArenaImage& dbgW = debug ? arena_.image("rx_dbg_fin_w") : accC;
    executor_.record(c, ShaderId::RxFinalize,
                     {{raw, ib(1, arena_.image("rx_base_cov")), ib(2, accA), ib(3, arena_.image("rx_acc_b")), ib(4, accC),
                       ib(5, arena_.image("rx_support")), ib(6, arena_.image("rx_cover")), ib(7, arena_.image("rx_base_rcd")),
                       ib(8, out), ib(9, dbgOut), ib(10, dbgPre), ib(11, dbgW)},
                      {}},
                     &p, sizeof(p), divUp(out.extent.width, 16), divUp(out.extent.height, 16));
    computeWriteBarrier(c);
}
}  // namespace rawr::raw_gpu_pipeline

namespace rawr::raw_gpu_pipeline {
void RextureRecorder::recordBaseDemosaic(VkCommandBuffer c, VkImageView rawU16, const MultiframeFrameParameters& f) const {
    if (!rawU16) throw std::invalid_argument("rexture recorder: null RAW16 view");
    const auto& out = arena_.image("rx_base_rcd");
    const auto& dir = arena_.image("rx_rcd_dir");
    struct Pc {
        std::uint32_t width, height, pattern, pad;
        float black[4], invRange[4], balance[4];
    } p{};
    p.width = out.extent.width;
    p.height = out.extent.height;
    p.pattern = static_cast<std::uint32_t>(config_.cfa);
    for (int i = 0; i < 4; ++i) {
        p.black[i] = f.normalization.blackByPhase[i];
        p.invRange[i] = 1.0f / (f.normalization.whiteLevel - f.normalization.blackByPhase[i]);
    }
    for (int i = 0; i < 3; ++i) p.balance[i] = config_.whiteBalance[i];
    p.balance[3] = 1.0f;
    ImageBinding raw{0, rawU16, VK_IMAGE_LAYOUT_GENERAL};
    const DispatchBindings b{{raw, ib(1, dir), ib(2, out)}, {}};
    const std::uint32_t half = (p.width + 1u) / 2u;
    // production workgroup sizes of the library: direction 4x32, green 4x32, diagonal 32x8 (half width), green sites 8x32
    // (half width), export 4x32
    executor_.record(c, ShaderId::RxRcdDirection, b, &p, sizeof(p), divUp(p.width, 4), divUp(p.height, 32));
    computeWriteBarrier(c);
    executor_.record(c, ShaderId::RxRcdGreen, b, &p, sizeof(p), divUp(p.width, 4), divUp(p.height, 32));
    computeWriteBarrier(c);
    executor_.record(c, ShaderId::RxRcdDiagonal, b, &p, sizeof(p), divUp(half, 32), divUp(p.height, 8));
    computeWriteBarrier(c);
    executor_.record(c, ShaderId::RxRcdGreenSites, b, &p, sizeof(p), divUp(half, 8), divUp(p.height, 32));
    computeWriteBarrier(c);
    executor_.record(c, ShaderId::RxRcdExport, b, &p, sizeof(p), divUp(p.width, 4), divUp(p.height, 32));
    computeWriteBarrier(c);
    recordBaseDemosaicBound(c, rawU16, f);
}
}  // namespace rawr::raw_gpu_pipeline
