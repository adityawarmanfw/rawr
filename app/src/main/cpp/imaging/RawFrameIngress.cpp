#include "RawFrameIngress.h"

#include <android/log.h>
#include <unistd.h>

#include <sstream>
#include <stdexcept>
#include <utility>

#include "imaging/FrameLimits.h"

namespace rawrcam::imaging {
namespace {
constexpr const char* kTag = "RawrCamNative";
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, kTag, __VA_ARGS__)
}  // namespace

RawFrameIngress::RawFrameIngress(FrameCallback frameCallback, Diagnostic diagnostic, Diagnostic pipelineAudit)
    : frameCallback_(std::move(frameCallback)),
      diagnostic_(std::move(diagnostic)),
      pipelineAudit_(std::move(pipelineAudit)) {}

RawFrameIngress::~RawFrameIngress() { destroy(); }

ANativeWindow* RawFrameIngress::create(uint64_t generation, uint32_t width, uint32_t height,
                                       geometry::RawPixelFormat format, uint64_t ahbUsage) {
    destroy();
    generation_ = generation;
    usage_ = ahbUsage;
    imageDescriptionLogged_ = false;
    auditImageCount_ = 0;

    const int32_t imageFormat = format == geometry::RawPixelFormat::Raw10 ? AIMAGE_FORMAT_RAW10 : AIMAGE_FORMAT_RAW16;
    const media_status_t status = AImageReader_newWithUsage(static_cast<int32_t>(width), static_cast<int32_t>(height),
                                                            imageFormat, ahbUsage, kRawReaderMaxImages, &reader_);
    if (status != AMEDIA_OK || !reader_) {
        std::ostringstream oss;
        oss << "AImageReader_newWithUsage " << geometry::rawPixelFormatName(format) << " failed status=" << status
            << " requestedUsage=0x" << std::hex << ahbUsage;
        throw std::runtime_error(oss.str());
    }

    AImageReader_ImageListener listener{};
    listener.context = this;
    listener.onImageAvailable = &RawFrameIngress::imageAvailableThunk;
    if (AImageReader_setImageListener(reader_, &listener) != AMEDIA_OK) {
        destroy();
        throw std::runtime_error("AImageReader_setImageListener failed");
    }

    ANativeWindow* inputWindow = nullptr;
    if (AImageReader_getWindow(reader_, &inputWindow) != AMEDIA_OK || !inputWindow) {
        destroy();
        throw std::runtime_error("AImageReader_getWindow failed");
    }

    std::ostringstream d;
    d << "RAW_READER_CREATED_NATIVE format=AIMAGE_FORMAT_" << geometry::rawPixelFormatName(format) << " dims=" << width
      << "x" << height << " requestedUsage=0x" << std::hex << ahbUsage << std::dec << " maxImages=6";
    LOGI("%s", d.str().c_str());
    if (diagnostic_) diagnostic_(d.str());
    return inputWindow;
}

void RawFrameIngress::destroy() {
    if (!reader_) return;
    AImageReader_ImageListener listener{};
    AImageReader_setImageListener(reader_, &listener);
    AImageReader_delete(reader_);
    reader_ = nullptr;
    generation_ = 0;
    imageDescriptionLogged_ = false;
    auditImageCount_ = 0;
}

void RawFrameIngress::imageAvailableThunk(void* context, AImageReader* reader) {
    static_cast<RawFrameIngress*>(context)->onImageAvailable(reader);
}

void RawFrameIngress::onImageAvailable(AImageReader* reader) {
    // Drain a short burst per callback. A callback is not a promise that a
    // second notification will arrive for an image already queued here.
    for (int drained = 0; drained < static_cast<int>(kRawReaderMaxImages); ++drained) {
        AImage* image = nullptr;
        int fenceFd = -1;
        // Acquire in sensor order. acquireLatestImageAsync silently discards queued
        // frames, so neither recording nor diagnostics can account for those gaps.
        // The downstream bounded queue decides which frame to discard under load.
        const media_status_t status = AImageReader_acquireNextImageAsync(reader, &image, &fenceFd);
        if (status != AMEDIA_OK || !image) return;

        if (!imageDescriptionLogged_) logImageDescription(image);

        int64_t timestamp = 0;
        AHardwareBuffer* ahb = nullptr;
        if (AImage_getTimestamp(image, &timestamp) != AMEDIA_OK || AImage_getHardwareBuffer(image, &ahb) != AMEDIA_OK ||
            !ahb) {
            if (fenceFd >= 0) close(fenceFd);
            AImage_delete(image);
            continue;
        }

        if (auditImageCount_ < 6) {
            ++auditImageCount_;
            int32_t actualWidth = 0;
            int32_t actualHeight = 0;
            int32_t actualFormat = 0;
            int32_t actualPlanes = 0;
            const media_status_t widthStatus = AImage_getWidth(image, &actualWidth);
            const media_status_t heightStatus = AImage_getHeight(image, &actualHeight);
            const media_status_t formatStatus = AImage_getFormat(image, &actualFormat);
            const media_status_t planesStatus = AImage_getNumberOfPlanes(image, &actualPlanes);
            AHardwareBuffer_Desc desc{};
            AHardwareBuffer_describe(ahb, &desc);
            if (pipelineAudit_) {
                pipelineAudit_(
                    "PIPELINE_AIMAGE_ACCEPTED generation=" + std::to_string(generation_) +
                    " timestampNs=" + std::to_string(timestamp) + " widthStatus=" + std::to_string(widthStatus) +
                    " width=" + std::to_string(actualWidth) + " heightStatus=" + std::to_string(heightStatus) +
                    " height=" + std::to_string(actualHeight) + " formatStatus=" + std::to_string(formatStatus) +
                    " format=" + std::to_string(actualFormat) + " planesStatus=" + std::to_string(planesStatus) +
                    " planes=" + std::to_string(actualPlanes) + " ahbWidth=" + std::to_string(desc.width) +
                    " ahbHeight=" + std::to_string(desc.height) + " ahbStridePixels=" + std::to_string(desc.stride) +
                    " ahbFormat=" + std::to_string(desc.format) + " ahbUsage=" + std::to_string(desc.usage));
            }
        }

        if (frameCallback_) {
            frameCallback_(AcquiredRawFrame{image, ahb, fenceFd, static_cast<uint64_t>(timestamp), generation_});
        } else {
            if (fenceFd >= 0) close(fenceFd);
            AImage_delete(image);
        }
    }
}

void RawFrameIngress::logImageDescription(AImage* image) {
    int32_t format = 0;
    int32_t width = 0;
    int32_t height = 0;
    int32_t planes = 0;
    const media_status_t formatStatus = AImage_getFormat(image, &format);
    const media_status_t widthStatus = AImage_getWidth(image, &width);
    const media_status_t heightStatus = AImage_getHeight(image, &height);
    const media_status_t planesStatus = AImage_getNumberOfPlanes(image, &planes);

    std::ostringstream d;
    d << "RAW_AIMAGE_DESCRIPTION formatStatus=" << formatStatus << " format=" << format << " formatHex=0x" << std::hex
      << static_cast<uint32_t>(format) << std::dec
      << " isRAW16=" << ((formatStatus == AMEDIA_OK && format == AIMAGE_FORMAT_RAW16) ? "yes" : "no")
      << " widthStatus=" << widthStatus << " width=" << width << " heightStatus=" << heightStatus
      << " height=" << height << " planesStatus=" << planesStatus << " planes=" << planes;
    LOGI("%s", d.str().c_str());
    if (diagnostic_) diagnostic_(d.str());

    if (planesStatus == AMEDIA_OK && planes > 0) {
        for (int32_t i = 0; i < planes; ++i) {
            int32_t rowStride = 0;
            int32_t pixelStride = 0;
            const media_status_t rowStatus = AImage_getPlaneRowStride(image, i, &rowStride);
            const media_status_t pixelStatus = AImage_getPlanePixelStride(image, i, &pixelStride);
            std::ostringstream pd;
            pd << "RAW_AIMAGE_PLANE index=" << i << " rowStrideStatus=" << rowStatus << " rowStrideBytes=" << rowStride
               << " pixelStrideStatus=" << pixelStatus << " pixelStrideBytes=" << pixelStride << " dataPointerRead=NO";
            LOGI("%s", pd.str().c_str());
            if (diagnostic_) diagnostic_(pd.str());
        }
    }
    imageDescriptionLogged_ = true;
}

}  // namespace rawrcam::imaging
