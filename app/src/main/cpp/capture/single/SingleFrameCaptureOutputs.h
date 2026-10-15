#pragma once
#include "capture/single/SingleFrameCaptureContextBuilder.h"
#include "encoding/dng/DngCaptureWriter.h"
#include "encoding/jpeg/JpegCaptureWriter.h"
namespace rawrcam::capture {
class SingleFrameCaptureOutputs final {
   public:
    SingleFrameCaptureOutputs(std::string filesDir, CaptureDiagnostic diagnostic)
        : filesDir_(std::move(filesDir)), diagnostic_(std::move(diagnostic)) {}
    ~SingleFrameCaptureOutputs();
    static bool validate(const encoding::dng::DngCaptureContext&, const JpegCaptureRequest&);
    void accept(encoding::dng::DngCaptureContext dng, JpegCaptureRequest jpeg, bool jpegRequested);
    const JpegCaptureRequest* jpegRequest() const { return jpeg_ ? &*jpeg_ : nullptr; }
    bool pending() const noexcept { return dng_.has_value() || jpeg_.has_value(); }
    bool busy() const noexcept { return pending() || dngWriter_.busy() || jpegWriter_.busy(); }
    void prepare(const imaging::RawSnapshot&, const tonemap::TonemapParams&, float, bool);
    bool startDng(std::shared_ptr<const imaging::RawSnapshot> frame, const std::string& recipe);
    bool startJpeg(const SingleFrameDevelopResult& result);
    bool advanceDng();
    bool advanceJpeg();
    void failDng(uint64_t id, const std::string& reason);
    void failJpeg(uint64_t id, const std::string& reason, bool filmFallback = false);
    void cancelDng() noexcept;
    void resetJpeg() noexcept;
    std::string pollDngCompletion();
    std::string pollJpegCompletion();
    bool hasDngCompletion() const noexcept { return dngCompletion_.has_value(); }

   private:
    bool dumpPreJpegPpm(uint64_t, uint32_t, uint32_t, const void*, size_t);
    void emit(const std::string& line) const {
        if (diagnostic_) diagnostic_(line);
    }
    std::string filesDir_;
    CaptureDiagnostic diagnostic_;
    encoding::dng::DngCaptureWriter dngWriter_;
    encoding::jpeg::JpegCaptureWriter jpegWriter_;
    // Pending contexts own their output descriptors until a writer takes them.
    std::optional<encoding::dng::DngCaptureContext> dng_;
    std::optional<JpegCaptureRequest> jpeg_;
    std::optional<encoding::dng::DngWriteCompletion> dngCompletion_;
    std::optional<encoding::jpeg::JpegWriteCompletion> jpegCompletion_;
};
}  // namespace rawrcam::capture
