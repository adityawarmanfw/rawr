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
    if (debug) img("rx_dbg_qinv", g.output, PixelStorage::RGBA32Float, Lifetime::Burst);
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
