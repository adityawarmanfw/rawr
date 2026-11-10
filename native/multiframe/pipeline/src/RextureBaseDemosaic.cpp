#include <rawr/raw_gpu_pipeline/RextureBaseDemosaic.h>

#include <algorithm>
#include <stdexcept>

#if defined(RAWR_REXTURE_HAS_RCD)
#include <raw_demosaic/EmbeddedShaders.hpp>
#include <rcd/Pipeline.hpp>
#endif

namespace rawr::raw_gpu_pipeline {

#if defined(RAWR_REXTURE_HAS_RCD)
struct RextureBaseDemosaic::Impl {
    std::unique_ptr<::rcd::RcdPipeline> pipeline;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    std::uint32_t width = 0, height = 0, cfa = 0;
    std::array<float, 3> balance{{0, 0, 0}};
};
RextureBaseDemosaic::RextureBaseDemosaic() : impl_(new Impl) {}
RextureBaseDemosaic::~RextureBaseDemosaic() = default;
bool RextureBaseDemosaic::available() noexcept { return true; }
void RextureBaseDemosaic::ensure(VkPhysicalDevice physical, VkDevice device, std::uint32_t queueFamily,
                                 std::uint32_t width, std::uint32_t height, std::uint32_t cfa,
                                 const std::array<float, 3>& whiteBalance) {
    auto& s = *impl_;
    if (s.pipeline && s.device == device && s.width == width && s.height == height && s.cfa == cfa &&
        s.balance == whiteBalance)
        return;
    s.pipeline.reset();
    ::rcd::PipelineConfig cfg{};
    cfg.width = width;
    cfg.height = height;
    cfg.pattern = static_cast<::rcd::BayerPattern>(cfa);
    cfg.inputMode = ::rcd::InputMode::RawR16UintImage;
    cfg.autoBalance = false;
    // The library takes a balance in [0.05, 1] (the largest gain maps to 1): the gains divided by their maximum. RCD is
    // invariant to a common scale of the three gains except through its tiny epsilon terms, and the export divides it out.
    const float top = std::max({whiteBalance[0], whiteBalance[1], whiteBalance[2]});
    for (int k = 0; k < 3; ++k) cfg.inputBalance[k] = std::clamp(whiteBalance[k] / top, 0.05f, 1.0f);
    cfg.outputScale = 1.0f / 255.0f;  // 0..255 working domain -> the input's own normalisation
    cfg.outputAlpha = 1.0f;
    const char* reason = nullptr;
    if (!::rcd::RcdPipeline::validateConfig(cfg, {}, &reason))
        throw std::runtime_error(std::string("rexture base demosaic: RCD config rejected: ") + (reason ? reason : "?"));
    ::rcd::VulkanContext context{};
    context.physicalDevice = physical;
    context.device = device;
    context.queueFamilyIndex = queueFamily;
    s.pipeline = std::make_unique<::rcd::RcdPipeline>(context, raw_demosaic::embeddedRcdShaders(), cfg);
    s.physical = physical;
    s.device = device;
    s.width = width;
    s.height = height;
    s.cfa = cfa;
    s.balance = whiteBalance;
}
void RextureBaseDemosaic::record(VkCommandBuffer c, VkImage rawImage, VkImageView rawView, const RawNormalization& n,
                                 const ArenaImage& out) {
    auto& s = *impl_;
    if (!s.pipeline) throw std::logic_error("rexture base demosaic: not ensured");
    ::rcd::RawCfaImageView in{};
    in.image = rawImage;
    in.view = rawView;
    in.format = VK_FORMAT_R16_UINT;
    in.layout = VK_IMAGE_LAYOUT_GENERAL;
    in.width = s.width;
    in.height = s.height;
    in.pattern = static_cast<::rcd::BayerPattern>(s.cfa);
    std::copy(n.blackByPhase.begin(), n.blackByPhase.end(), in.blackLevel);
    in.whiteLevel = n.whiteLevel;
    ::rcd::LinearRgbImage o{};
    o.image = out.image;
    o.view = out.view;
    o.format = out.format;
    o.layout = VK_IMAGE_LAYOUT_GENERAL;
    o.width = out.extent.width;
    o.height = out.extent.height;
    s.pipeline->record(c, in, o);
}
void RextureBaseDemosaic::release() noexcept {
    if (impl_) impl_->pipeline.reset();
}
std::uint64_t RextureBaseDemosaic::allocatedBytes() const noexcept {
    return impl_ && impl_->pipeline ? impl_->pipeline->peakAllocatedBytes() : 0u;
}
#else
struct RextureBaseDemosaic::Impl {};
RextureBaseDemosaic::RextureBaseDemosaic() = default;
RextureBaseDemosaic::~RextureBaseDemosaic() = default;
bool RextureBaseDemosaic::available() noexcept { return false; }
void RextureBaseDemosaic::ensure(VkPhysicalDevice, VkDevice, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                                 const std::array<float, 3>&) {
    throw std::runtime_error("rexture base demosaic: built without the RCD library");
}
void RextureBaseDemosaic::record(VkCommandBuffer, VkImage, VkImageView, const RawNormalization&, const ArenaImage&) {
    throw std::runtime_error("rexture base demosaic: built without the RCD library");
}
void RextureBaseDemosaic::release() noexcept {}
std::uint64_t RextureBaseDemosaic::allocatedBytes() const noexcept { return 0; }
#endif

}  // namespace rawr::raw_gpu_pipeline
