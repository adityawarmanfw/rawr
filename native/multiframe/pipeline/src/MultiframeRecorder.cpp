#include <rawr/raw_gpu_pipeline/MultiframeRecorder.h>
#include <rawr/raw_gpu_pipeline/ReconstructionTiling.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>

namespace rawr::raw_gpu_pipeline {
namespace {
constexpr std::uint32_t divUp(std::uint32_t x, std::uint32_t y) { return (x + y - 1u) / y; }
ImageBinding ib(std::uint32_t b, const ArenaImage& i) { return {b, i.view, VK_IMAGE_LAYOUT_GENERAL}; }
BufferBinding bb(std::uint32_t b, const ArenaBuffer& x) { return {b, x.buffer, 0, x.bytes}; }
void transferToCompute(VkCommandBuffer c) {
    VkMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b, 0, nullptr,
                         0, nullptr);
}
struct NormalizePc {
    float black[4];
    float white;
    std::int32_t w, h;
};
struct Local5Pc {
    std::int32_t axis, w, h;
};
struct GaussianPc {
    std::int32_t axis, radius, factor, outW, outH;
    float sigma;
};
struct DecimatePc {
    std::int32_t factor, w, h;
};
struct UpsamplePc {
    std::int32_t repeatFactor, flowScale, w, h;
};
struct BlockPc {
    std::int32_t tileSize, radius, metric, tilesX, tilesY;
};
struct LkPc {
    std::int32_t ts, tilesX, tilesY, iterations;
    float eps;
    float clipRadius;
    float noiseSlope, noiseOffset;
};
struct KernelPc {
    float alpha[4], beta[4], highAlpha[4], highBeta[4], knee[4];
    float kDetail, kDenoise, dThreshold, dTransition, kStretch, kShrink;
    float flatSigma, detailFloorEffective;
    std::int32_t gw, gh;
};
struct GuidePc {
    float wb[3];
    std::int32_t w, h, cfa;
};
struct FlowScalePc {
    float s1, s2, threshold;
    std::int32_t tilesX, tilesY;
};
struct PhotoPc {
    float t;
    std::int32_t clampNoise, tileSize, w, h;
};
struct ErodePc {
    std::int32_t w, h;
};
struct AffineFitPc {
    std::int32_t imageW, imageH;
    float tileSize;
};
struct AffineGatePc {
    std::int32_t imageW, imageH, tileSize;
    float deadzoneSigma, softnessSigma;
    std::int32_t apertureAware;
};
struct AccPc {
    float scale;
    std::int32_t outW, outH, tileSize, cfa, yBase;
};
struct RefAccPc {
    float scale, threshold, companions;
    std::int32_t outW, outH, cfa, yBase;
    float neffLo, neffHi, massLo, massHi;
};
struct FinalPc {
    std::int32_t yBase;
};
static_assert(sizeof(NormalizePc) <= 128 && sizeof(KernelPc) <= 128 && sizeof(RefAccPc) <= 128);
}  // namespace

MultiframeRecorder::MultiframeRecorder(ResourceArena& a, VulkanExecutor& e, rawr::raw_alignment_gpu::Config ac,
                                     rawr::raw_merge_wronski_gpu::Config mc, std::uint32_t sh)
    : arena_(a), executor_(e), alignment_(ac), merge_(mc), stripeHeight_(sh) {
    if (!rawr::raw_alignment_gpu::valid(alignment_))
        throw std::invalid_argument("multiframe recorder: invalid alignment config");
    if (!rawr::raw_merge_wronski_gpu::valid(merge_))
        throw std::invalid_argument("multiframe recorder: invalid merge config");
    if (!stripeHeight_) throw std::invalid_argument("multiframe recorder: zero stripe height");
    const auto& out = arena_.image("linear_output");
    if (stripeHeight_ < out.extent.height)
        throw std::invalid_argument(
            "multiframe recorder: striped execution requires stripe-major scheduling; recorder currently "
            "requires a full-height accumulator");
}

void MultiframeRecorder::recordUploadNoiseCurves(VkCommandBuffer c, const float* s, const float* d) const {
    if (!c || !s || !d) throw std::invalid_argument("multiframe recorder: invalid noise curve upload");
    const auto& sb = arena_.buffer("noise_std_curve");
    const auto& db = arena_.buffer("noise_diff_curve");
    constexpr VkDeviceSize n = 3u * 1001u * sizeof(float);
    if (sb.bytes < n || db.bytes < n) throw std::logic_error("multiframe recorder: noise buffers too small");
    vkCmdUpdateBuffer(c, sb.buffer, 0, n, s);
    vkCmdUpdateBuffer(c, db.buffer, 0, n, d);
    transferToCompute(c);
}

void MultiframeRecorder::recordNormalize(VkCommandBuffer c, bool ref, VkImageView rawU16,
                                        const MultiframeFrameParameters& f, const TraceTarget* tr) const {
    if (!rawU16) throw std::invalid_argument("multiframe recorder: null RAW16 view");
    const auto& o = arena_.image(ref ? "reference_raw_f32" : "companion_raw_f32");
    if (!(f.normalization.whiteLevel >
          *std::max_element(f.normalization.blackByPhase.begin(), f.normalization.blackByPhase.end())))
        throw std::invalid_argument("multiframe recorder: invalid black/white");
    NormalizePc p{};
    std::copy(f.normalization.blackByPhase.begin(), f.normalization.blackByPhase.end(), p.black);
    p.white = f.normalization.whiteLevel;
    p.w = std::int32_t(o.extent.width);
    p.h = std::int32_t(o.extent.height);
    ImageBinding raw{0, rawU16, VK_IMAGE_LAYOUT_GENERAL};
    executor_.record(c, ShaderId::RawNormalize, {{raw, ib(1, o)}, {}}, &p, sizeof(p), divUp(o.extent.width, 16),
                     divUp(o.extent.height, 16), 1, tr);
    computeWriteBarrier(c);
    if (hotPixelBuffer_ && hotPixelCount_ > 0u) {
        struct {
            std::int32_t count, w, h;
        } hp{std::int32_t(hotPixelCount_), std::int32_t(o.extent.width), std::int32_t(o.extent.height)};
        executor_.record(c, ShaderId::HotPixelConceal, {{ib(0, o)}, {BufferBinding{1u, hotPixelBuffer_}}}, &hp, sizeof(hp),
                         divUp(hotPixelCount_, 64), 1, 1, tr);
        computeWriteBarrier(c);
    }
}

void MultiframeRecorder::recordAlignmentInput(VkCommandBuffer c, bool ref, const TraceTarget* tr) const {
    if (!alignment_.cfaSuppressionLocal5) return;
    const auto& src = arena_.image(ref ? "reference_raw_f32" : "companion_raw_f32");
    const auto& tmp = arena_.image("robust_a");
    const auto& dst = arena_.image(ref ? "reference_alignment_f32" : "robust_b");
    Local5Pc horizontal{0, std::int32_t(src.extent.width), std::int32_t(src.extent.height)};
    executor_.record(c, ShaderId::AlignmentLocal5, {{ib(0, src), ib(1, tmp)}, {}}, &horizontal, sizeof(horizontal),
                     divUp(src.extent.width, 16), divUp(src.extent.height, 16), 1, tr);
    computeWriteBarrier(c);
    Local5Pc vertical{1, std::int32_t(src.extent.width), std::int32_t(src.extent.height)};
    executor_.record(c, ShaderId::AlignmentLocal5, {{ib(0, tmp), ib(1, dst)}, {}}, &vertical, sizeof(vertical),
                     divUp(src.extent.width, 16), divUp(src.extent.height, 16), 1, tr);
    computeWriteBarrier(c);
}

void MultiframeRecorder::recordPyramid(VkCommandBuffer c, bool ref, const TraceTarget* tr) const {
    const auto& geom = arena_.image(ref ? "reference_raw_f32" : "companion_raw_f32");
    (void)geom;
    static constexpr std::uint32_t factors[4] = {1, 2, 4, 4};
    std::string src;
    if (alignment_.cfaSuppressionLocal5)
        src = ref ? "reference_alignment_f32" : "robust_b";
    else
        src = ref ? "reference_raw_f32" : "companion_raw_f32";
    for (std::uint32_t fi = 1; fi < 4; ++fi) {
        const std::string y = "pyramid_tmp_y_f" + std::to_string(fi), x = "pyramid_tmp_x_f" + std::to_string(fi);
        const std::string dst = (ref ? "reference_pyramid_l" : "companion_pyramid_l") + std::to_string(3u - fi);
        const float sigma = .5f * float(factors[fi]);
        const int radius = int(4.f * sigma + .5f);
        const auto& si = arena_.image(src);
        const auto& yi = arena_.image(y);
        const auto& xi = arena_.image(x);
        const auto& di = arena_.image(dst);
        GaussianPc py{
            1, radius, std::int32_t(factors[fi]), std::int32_t(yi.extent.width), std::int32_t(yi.extent.height), sigma};
        executor_.record(c, ShaderId::PyramidGaussian, {{ib(0, si), ib(1, yi)}, {}}, &py, sizeof(py),
                         divUp(yi.extent.width, 16), divUp(yi.extent.height, 16), 1, tr);
        computeWriteBarrier(c);
        GaussianPc px{
            0, radius, std::int32_t(factors[fi]), std::int32_t(xi.extent.width), std::int32_t(xi.extent.height), sigma};
        executor_.record(c, ShaderId::PyramidGaussian, {{ib(0, yi), ib(1, xi)}, {}}, &px, sizeof(px),
                         divUp(xi.extent.width, 16), divUp(xi.extent.height, 16), 1, tr);
        computeWriteBarrier(c);
        DecimatePc pd{std::int32_t(factors[fi]), std::int32_t(di.extent.width), std::int32_t(di.extent.height)};
        executor_.record(c, ShaderId::PyramidDecimate, {{ib(0, xi), ib(1, di)}, {}}, &pd, sizeof(pd),
                         divUp(di.extent.width, 16), divUp(di.extent.height, 16), 1, tr);
        computeWriteBarrier(c);
        src = dst;
    }
}

void MultiframeRecorder::recordAlignmentLevel(VkCommandBuffer c, std::uint32_t l, const TraceTarget* tr) const {
    if (l >= 4) throw std::invalid_argument("multiframe recorder: invalid alignment level");
    if (l == 0) {
        arena_.recordPrepareCompanionAlignment(c);
        transferToCompute(c);
    }
    const std::string rs = l == 3
                               ? (alignment_.cfaSuppressionLocal5 ? "reference_alignment_f32" : "reference_raw_f32")
                               : "reference_pyramid_l" + std::to_string(l);
    const std::string ms = l == 3 ? (alignment_.cfaSuppressionLocal5 ? "robust_b" : "companion_raw_f32")
                                  : "companion_pyramid_l" + std::to_string(l);
    const auto& ri = arena_.image(rs);
    const auto& mi = arena_.image(ms);
    const auto& flow = arena_.image("tile_flow_l" + std::to_string(l));
    if (l > 0) {
        const auto& coarse = arena_.image("tile_flow_l" + std::to_string(l - 1));
        const auto& prev =
            arena_.image(l - 1 == 3 ? "reference_raw_f32" : "reference_pyramid_l" + std::to_string(l - 1));
        (void)prev;
        // Geometry identity: one coarse tile spans flowScale*coarseTileSize fine pixels.
        const std::uint32_t coarseTs = (l - 1 == 0 ? 8u : 16u);
        const std::uint32_t fineTs = 16u;
        const std::uint32_t scale = (l == 1 ? 4u : (l == 2 ? 4u : 2u));
        const std::uint32_t repeat = (scale * coarseTs) / fineTs;
        UpsamplePc p{std::int32_t(repeat), std::int32_t(scale), std::int32_t(flow.extent.width),
                     std::int32_t(flow.extent.height)};
        executor_.record(c, ShaderId::FlowUpsample, {{ib(0, coarse), ib(1, flow)}, {}}, &p, sizeof(p),
                         divUp(flow.extent.width, 16), divUp(flow.extent.height, 16), 1, tr);
        computeWriteBarrier(c);
    }
    const std::uint32_t ts = (l == 0 ? 8u : 16u), radius = (l == 3 ? 1u : 4u);
    const int metric = (l == 3 ? 0 : 1);
    BlockPc bp{std::int32_t(ts), std::int32_t(radius), metric, std::int32_t(flow.extent.width),
               std::int32_t(flow.extent.height)};
    executor_.record(c, ShaderId::BlockMatch, {{ib(0, ri), ib(1, mi), ib(2, flow)}, {}}, &bp,
                     sizeof(bp), flow.extent.width, flow.extent.height, 1, tr);
    computeWriteBarrier(c);
    float noiseSlope = merge_.noiseAlpha, noiseOffset = merge_.noiseBeta;
    if (merge_.sensorNoiseProfile) {
        const auto& n = *merge_.sensorNoiseProfile;
        noiseSlope = .25f * (n.slopeBySite[0] + n.slopeBySite[1] + n.slopeBySite[2] + n.slopeBySite[3]);
        noiseOffset = .25f * (n.offsetBySite[0] + n.offsetBySite[1] + n.offsetBySite[2] + n.offsetBySite[3]);
    }
    LkPc lp{std::int32_t(ts), std::int32_t(flow.extent.width), std::int32_t(flow.extent.height),
            std::int32_t(alignment_.lkIterations), alignment_.hessianEpsilon, float(radius), noiseSlope, noiseOffset};
    executor_.record(c, ShaderId::LkRefine,
                     {{ib(0, ri), ib(1, mi), ib(2, flow), ib(3, arena_.image("tile_tensor_l3"))}, {}}, &lp,
                     // One workgroup per tile (tile-parallel LK): dispatch the full tile grid.
                     sizeof(lp), flow.extent.width, flow.extent.height, 1, tr);
    computeWriteBarrier(c);
}

void MultiframeRecorder::recordKernelAndStats(VkCommandBuffer c, bool ref, const MultiframeFrameParameters& f,
                                             const TraceTarget* tr) const {
    const auto& raw = arena_.image(ref ? "reference_raw_f32" : "companion_raw_f32");
    if (ref) {
        const auto& cov = arena_.image("reference_kernel");
        KernelPc kp{};
        if (merge_.sensorNoiseProfile) {
            std::copy(merge_.sensorNoiseProfile->slopeBySite.begin(), merge_.sensorNoiseProfile->slopeBySite.end(),
                      kp.alpha);
            std::copy(merge_.sensorNoiseProfile->offsetBySite.begin(), merge_.sensorNoiseProfile->offsetBySite.end(),
                      kp.beta);
            const auto& high = merge_.highSignalNoiseProfile.value_or(*merge_.sensorNoiseProfile);
            std::copy(high.slopeBySite.begin(), high.slopeBySite.end(), kp.highAlpha);
            std::copy(high.offsetBySite.begin(), high.offsetBySite.end(), kp.highBeta);
            std::copy(merge_.noiseKneeBySite.begin(), merge_.noiseKneeBySite.end(), kp.knee);
        } else {
            std::fill(std::begin(kp.alpha), std::end(kp.alpha), merge_.noiseAlpha);
            std::fill(std::begin(kp.beta), std::end(kp.beta), merge_.noiseBeta);
            std::fill(std::begin(kp.highAlpha), std::end(kp.highAlpha), merge_.noiseAlpha);
            std::fill(std::begin(kp.highBeta), std::end(kp.highBeta), merge_.noiseBeta);
        }
        kp.kDetail = merge_.kDetail;
        kp.kDenoise = merge_.kDenoise;
        kp.dThreshold = merge_.dThreshold;
        kp.dTransition = merge_.dTransition;
        kp.kStretch = merge_.kStretch;
        kp.kShrink = merge_.kShrink;
        kp.flatSigma = merge_.flatSigma;
        // Scale-aware floor precomputed on CPU to stay within the 128-byte
        // push-constant limit: floor grows with output scale only when
        // scaleBandwidthGain > 0, otherwise scale-agnostic behavior.
        kp.detailFloorEffective =
            merge_.detailFloorSigma * (1.f + merge_.scaleBandwidthGain * (merge_.outputScale - 1.f));
        kp.gw = std::int32_t(cov.extent.width);
        kp.gh = std::int32_t(cov.extent.height);
        executor_.record(c, ShaderId::KernelEstimate, {{ib(0, raw), ib(1, cov)}, {}}, &kp, sizeof(kp),
                         divUp(cov.extent.width, 16), divUp(cov.extent.height, 16), 1, tr);
        computeWriteBarrier(c);
    }
    const auto& guide = arena_.image(ref ? "reference_guide" : "companion_guide");
    GuidePc gp{{1.0f, 1.0f, 1.0f}, std::int32_t(guide.extent.width), std::int32_t(guide.extent.height),
                std::int32_t(merge_.cfa)};
    (void)f.whiteBalance;
    executor_.record(c, ShaderId::RobustGuide, {{ib(0, raw), ib(1, guide)}, {}}, &gp, sizeof(gp),
                     divUp(guide.extent.width, 16), divUp(guide.extent.height, 16), 1, tr);
    computeWriteBarrier(c);
    const auto& mean = arena_.image(ref ? "reference_mean_lr" : "companion_mean_lr");
    const auto& var = arena_.image(ref ? "reference_var_lr" : "companion_var_scratch");
    executor_.record(c, ShaderId::RobustStats, {{ib(0, guide), ib(1, mean), ib(2, var)}, {}}, nullptr, 0,
                     divUp(guide.extent.width, 16), divUp(guide.extent.height, 16), 1, tr);
    computeWriteBarrier(c);
}

void MultiframeRecorder::recordReferencePrepare(VkCommandBuffer c, VkImageView rawU16, const MultiframeFrameParameters& f,
                                               const TraceTarget* tr) const {
    recordNormalize(c, true, rawU16, f, tr);
    recordAlignmentInput(c, true, tr);
    recordPyramid(c, true, tr);
}

void MultiframeRecorder::recordReferenceStats(VkCommandBuffer c, const MultiframeFrameParameters& f,
                                             const TraceTarget* tr) const {
    recordKernelAndStats(c, true, f, tr);
}

void MultiframeRecorder::recordCompanionPrepare(VkCommandBuffer c, VkImageView rawU16, const MultiframeFrameParameters& f,
                                               const TraceTarget* tr) const {
    recordNormalize(c, false, rawU16, f, tr);
    recordAlignmentInput(c, false, tr);
    recordPyramid(c, false, tr);
}

void MultiframeRecorder::recordCompanionAlignmentLevel(VkCommandBuffer c, std::uint32_t l,
                                                      const TraceTarget* tr) const {
    recordAlignmentLevel(c, l, tr);
}

void MultiframeRecorder::recordCompanionStats(VkCommandBuffer c, const MultiframeFrameParameters& f,
                                             const TraceTarget* tr) const {
    recordKernelAndStats(c, false, f, tr);
}

void MultiframeRecorder::recordCompanionRobustness(VkCommandBuffer c, const TraceTarget* tr) const {
    const auto& flow = arena_.image("tile_flow_l3");
    const auto& valid = arena_.image("tile_valid");
    const auto& scale = arena_.image("robust_scale_tiles");
    FlowScalePc sp{merge_.robustnessS1, merge_.robustnessS2, merge_.motionThreshold, std::int32_t(flow.extent.width),
                   std::int32_t(flow.extent.height)};
    executor_.record(c, ShaderId::RobustFlowScale, {{ib(0, flow), ib(1, valid), ib(2, scale)}, {}}, &sp, sizeof(sp),
                     divUp(scale.extent.width, 16), divUp(scale.extent.height, 16), 1, tr);
    computeWriteBarrier(c);
    const auto& pre = arena_.image("robust_a");
    const auto robustnessExtent = arena_.image("reference_guide").extent;
    PhotoPc pp{merge_.robustnessT, merge_.noiseDomainClamp ? 1 : 0, 16, std::int32_t(robustnessExtent.width),
               std::int32_t(robustnessExtent.height)};
    DispatchBindings pb{{ib(0, arena_.image("reference_mean_lr")), ib(1, arena_.image("reference_var_lr")),
                         ib(2, arena_.image("companion_mean_lr")), ib(3, flow), ib(4, valid), ib(5, scale), ib(8, pre)},
                        {bb(6, arena_.buffer("noise_std_curve")), bb(7, arena_.buffer("noise_diff_curve"))}};
    executor_.record(c, ShaderId::RobustPhotometric, pb, &pp, sizeof(pp), divUp(robustnessExtent.width, 16),
                     divUp(robustnessExtent.height, 16), 1, tr);
    computeWriteBarrier(c);
    const auto& er = arena_.image("robust_b");
    ErodePc ep{std::int32_t(robustnessExtent.width), std::int32_t(robustnessExtent.height)};
    executor_.record(c, ShaderId::RobustErode5, {{ib(0, pre), ib(1, er)}, {}}, &ep, sizeof(ep),
                     divUp(robustnessExtent.width, 16), divUp(robustnessExtent.height, 16), 1, tr);
    computeWriteBarrier(c);
    AffineFitPc af{std::int32_t(er.extent.width), std::int32_t(er.extent.height), 16.f};
    DispatchBindings ab{{ib(0, flow), ib(1, valid)},
                        {bb(2, arena_.buffer("affine_weights")), bb(3, arena_.buffer("affine_residuals")),
                         bb(4, arena_.buffer("affine_scratch")), bb(5, arena_.buffer("affine_model"))}};
    executor_.record(c, ShaderId::AffineFit, ab, &af, sizeof(af), 1, 1, 1, tr);
    computeWriteBarrier(c);
    AffineGatePc ag{std::int32_t(robustnessExtent.width), std::int32_t(robustnessExtent.height), 8,
                    merge_.affineDeadzoneSigma,
                    merge_.affineSoftnessSigma, merge_.affineApertureAware ? 1 : 0};
    DispatchBindings gb{{ib(0, flow), ib(1, valid), ib(2, er), ib(3, pre), ib(5, arena_.image("tile_tensor_l3"))},
                        {bb(4, arena_.buffer("affine_model"))}};
    executor_.record(c, ShaderId::AffineGate, gb, &ag, sizeof(ag),
                     divUp(robustnessExtent.width, 16),
                     divUp(robustnessExtent.height, 16), 1, tr);
    computeWriteBarrier(c);
    const auto& support = arena_.image("support");
    executor_.record(c, ShaderId::SupportAccumulate, {{ib(0, pre), ib(1, support)}, {}}, nullptr, 0,
                     divUp(robustnessExtent.width, 16), divUp(robustnessExtent.height, 16), 1, tr);
    computeWriteBarrier(c);
}

void MultiframeRecorder::recordCompanionAccumulate(VkCommandBuffer c, const TraceTarget* tr) const {
    const auto& flow = arena_.image("tile_flow_l3");
    const auto& valid = arena_.image("tile_valid");
    const auto& pre = arena_.image("robust_a");
    const auto& sum = arena_.image("rgb_sum");
    const auto& wt = arena_.image("rgb_weight");
    const auto& out = arena_.image("linear_output");
    AccPc ap{merge_.outputScale,
             std::int32_t(out.extent.width),
             std::int32_t(out.extent.height),
             16,
             std::int32_t(merge_.cfa),
             0};
    DispatchBindings bindings{{ib(0, arena_.image("companion_raw_f32")), ib(1, flow), ib(2, valid), ib(3, pre),
        ib(4, arena_.image("reference_kernel")), ib(5, sum), ib(6, wt),
        ib(7, arena_.image("rgb_weight_square_b"))}, {}};
    executor_.record(c, ShaderId::Accumulate,
                     bindings, &ap, sizeof(ap), divUp(out.extent.width, 16), divUp(out.extent.height, 16), 1, tr);
    computeWriteBarrier(c);
}

void MultiframeRecorder::recordFinalize(VkCommandBuffer c, std::uint32_t companions, const TraceTarget* tr) const {
    if (!companions) throw std::invalid_argument("multiframe recorder: zero companions");
    const auto& sum = arena_.image("rgb_sum");
    const auto& wt = arena_.image("rgb_weight");
    const auto& out = arena_.image("linear_output");
    RefAccPc rp{merge_.outputScale,
                merge_.normalizedSupportThreshold,
                float(companions),
                std::int32_t(out.extent.width),
                std::int32_t(out.extent.height),
                std::int32_t(merge_.cfa),
                0,
                merge_.coverageNeffLo,
                merge_.coverageNeffHi,
                merge_.coverageMassLo,
                merge_.coverageMassHi};
    executor_.record(c, ShaderId::ReferenceAccumulate,
                     {{ib(0, arena_.image("reference_raw_f32")), ib(1, arena_.image("reference_kernel")),
                       ib(2, arena_.image("support")), ib(3, sum), ib(4, wt),
                       ib(5, arena_.image("rgb_weight_square_b"))},
                      {}},
                     &rp, sizeof(rp), divUp(out.extent.width, 16), divUp(out.extent.height, 16), 1, tr);
    computeWriteBarrier(c);
    if (merge_.fallbackChromaGain > 0.f || merge_.fallbackLumaGain > 0.f) {
        float noiseSlope = merge_.noiseAlpha, noiseOffset = merge_.noiseBeta;
        if (merge_.sensorNoiseProfile) {
            const auto& n = *merge_.sensorNoiseProfile;
            noiseSlope = .25f * (n.slopeBySite[0] + n.slopeBySite[1] + n.slopeBySite[2] + n.slopeBySite[3]);
            noiseOffset = .25f * (n.offsetBySite[0] + n.offsetBySite[1] + n.offsetBySite[2] + n.offsetBySite[3]);
        }
        struct {
            std::int32_t yBase;
            float targetFrames, gain, maxSigma, lumaGain, noiseSlope, noiseOffset, scale;
            std::int32_t pass;
        } cp{0,
             float(companions + 1u),
             merge_.fallbackChromaGain,
             merge_.fallbackChromaMaxSigma,
             merge_.fallbackLumaGain,
             noiseSlope,
             noiseOffset,
             merge_.outputScale,
             0};
        // Separable: horizontal into the output image as scratch, then
        // vertical back into sum/weight, then the plain finalize below.
        for (std::int32_t pass = 0; pass < 2; ++pass) {
            cp.pass = pass;
            executor_.record(c, ShaderId::FallbackChroma,
                             {{ib(0, sum), ib(1, wt), ib(2, out), ib(3, arena_.image("support"))}, {}}, &cp,
                             sizeof(cp), divUp(out.extent.width, 16), divUp(out.extent.height, 16), 1, tr);
            computeWriteBarrier(c);
        }
    }
    FinalPc fp{0};
    executor_.record(c, ShaderId::A11Finalize, {{ib(0, sum), ib(1, wt), ib(2, out)}, {}}, &fp, sizeof(fp),
                     divUp(out.extent.width, 16), divUp(out.extent.height, 16), 1, tr);
    computeWriteBarrier(c);
}

}  // namespace rawr::raw_gpu_pipeline
