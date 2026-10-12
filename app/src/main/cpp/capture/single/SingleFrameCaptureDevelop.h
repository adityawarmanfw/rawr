#pragma once
#include "capture/single/SingleFrameCaptureContextBuilder.h"
#include "develop/demosaic/AmazeStillProcessor.h"
#include "develop/demosaic/DualStillProcessor.h"
#include "develop/demosaic/RcdStillProcessor.h"
#include "develop/demosaic/VngStillProcessor.h"
#include "develop/render/StillImageRenderer.h"
namespace rawrcam::capture {
class SingleFrameCaptureDevelop final {
   public:
    SingleFrameCaptureDevelop(const vulkan::VulkanContext&, std::mutex&, std::string, CaptureDiagnostic);
    void setAssetManager(AAssetManager* assets) { renderer_.setAssetManager(assets); }
    bool start(const std::shared_ptr<const imaging::RawSnapshot>&, const tonemap::TonemapParams&, float, bool,
               const spektrafilm_native::FilmLook&, const JpegCaptureRequest&);
    std::optional<SingleFrameDevelopResult> pollCompletion();
    bool busy() const noexcept;
    void releasePixels() noexcept {
        renderer_.releasePixels();
        requestId_.reset();
        context_.reset();
    }
    void reset() noexcept;
    void shutdown() noexcept;

   private:
    template <class Processor>
    void advanceDemosaic(Processor& processor, const char* name);
    void failPendingJpeg(uint64_t id, const std::string& error);
    void emit(const std::string& line) const {
        if (diagnostic_) diagnostic_(line);
    }
    const vulkan::VulkanContext& vulkanContext_;
    std::mutex& queueSubmitMutex_;
    std::string filesDir_;
    CaptureDiagnostic diagnostic_;
    develop::demosaic::amaze::AmazeStillProcessor amaze_;
    develop::demosaic::rcd::RcdStillProcessor rcd_;
    develop::demosaic::vng4::VngStillProcessor vng_;
    develop::demosaic::dual::DualStillProcessor dual_;
    develop::rendered::StillImageRenderer renderer_;
    std::optional<uint64_t> requestId_;
    std::optional<develop::rendered::RenderedStillContext> context_;
    std::optional<develop::rendered::RenderedStillCompletion> failure_;
    bool diagnostics_ = false;
    double demosaicMs_ = 0.0, demosaicSetupMs_ = 0.0;
};
}  // namespace rawrcam::capture
