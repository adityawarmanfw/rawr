#pragma once
#include <android/native_window.h>
#include <camera/NdkCameraManager.h>
#include <media/NdkImageReader.h>

#include <chrono>
#include <functional>
#include <map>
#include <optional>

#include "camera/CameraCallbacks.h"
#include "camera/CameraRequestPipeline.h"
#include "camera/CameraRouting.h"
#include "metadata/CameraContextMetadata.h"
namespace rawrcam::camera {
struct CameraReaderRetirement {
    uint64_t generation = 0;
    bool destroy = false;
};
class CameraDeviceSession final {
   public:
    using Diagnostic = std::function<void(const std::string&)>;
    CameraDeviceSession();
    ~CameraDeviceSession();
    CameraDeviceSession(const CameraDeviceSession&) = delete;
    CameraDeviceSession& operator=(const CameraDeviceSession&) = delete;
    // Reads characteristics and negotiates the RAW stream. Returns the route
    // with `stream` resolved, or nullopt when the camera can't be used.
    std::optional<LensRoute> select(const LensRoute&, const Diagnostic&);
    CameraControlState initialControls(const LensRoute& route) const;
    // Releases the controller lock while waiting; stillCurrent is checked with
    // that lock held before touching session state after each wait.
    bool open(const LensRoute&, CameraCallbacks&, const Diagnostic&, std::unique_lock<std::mutex>&,
              const std::function<bool()>& stillCurrent);
    // companionStream adds a small discarded YUV output next to RAW: some HALs
    // (MediaTek) accept a RAW-only session but never deliver a RAW buffer.
    bool createSession(ANativeWindow*, const std::optional<LensRoute>&, const CameraControlState&,
                       const CameraMeteringRequest&, CameraCallbacks&, CameraRequestPipeline&, const Diagnostic&,
                       bool companionStream);
    bool retire(std::unique_lock<std::mutex>&, std::optional<std::chrono::milliseconds>, const char*,
                const Diagnostic&);
    void forceRetire(const char*, const Diagnostic&);
    CameraReaderRetirement release(const Diagnostic&);
    void sessionClosed(uint64_t generation, ACameraCaptureSession* session);
    void rollbackUnacceptedContext() noexcept;
    void setRawWindow(ANativeWindow* window) noexcept {
        rawWindow_ = window;
        readerGeneration_ = generation_;
    }
    // Window of LensRoute::captureStream: a session output no repeating request targets.
    void setCaptureWindow(ANativeWindow* window) noexcept { captureWindow_ = window; }
    bool hasCaptureStream() const noexcept { return captureTarget_ != nullptr; }
    // Burst of full-size stills (one per exposure, tagged tagId): copies of the
    // repeating request that also fill the capture stream, with STILL_CAPTURE
    // intent and the lens's still-scope keys.
    bool captureFullSize(CameraRequestPipeline&, const CameraControlState&,
                         const std::vector<CameraRequestPipeline::BracketExposure>&, uint64_t tagId, const Diagnostic&);
    bool hasDevice() const noexcept { return device_ != nullptr; }
    bool hasSession() const noexcept { return session_ != nullptr; }
    ACaptureRequest* request() const noexcept { return request_; }
    ACameraCaptureSession* session() const noexcept { return session_; }
    CameraSessionCallbackContext* callbackContext() const noexcept { return sessionContext_; }
    uint64_t generation() const noexcept { return generation_; }
    const metadata::CameraContextMetadataPtr& cameraContext() const noexcept { return cameraContext_; }
    // Static black/white levels for the running session (from the lens
    // profile); nullopt means dynamic levels.
    const std::optional<LevelOverride>& staticLevels() const noexcept { return staticLevels_; }
    // True when profile session keys changed the sensor readout, so reported
    // sensitivity no longer maps 1:1 onto requested sensitivity.
    bool sensorModeOverridden() const noexcept { return sessionKeysApplied_; }
    // Removes the companion target from the repeating request; the stream stays
    // configured. True when a target was removed (resubmit to apply).
    bool detachCompanionTarget(const Diagnostic&);
    // Integer value of a vendor result tag, looked up by name once per open
    // camera; nullopt when the camera or this result lacks it.
    std::optional<int64_t> vendorResultValue(const ACameraMetadata* result, const std::string& name);

   private:
    void releaseRequestOutputs(bool orphan) noexcept;
    void releaseCharacteristics() noexcept;
    // Adds the companion YUV output and request target; false leaves the session RAW-only.
    bool addCompanionStream(const std::string& physicalCameraId, const Diagnostic&);
    void closeDeviceDetached(std::function<void()> afterClose);
    // Releases a vendor gate taken for a device that never opened or is already gone.
    void releaseHeldGate() noexcept;
    ACameraManager* manager_ = nullptr;
    ACameraDevice* device_ = nullptr;
    ACameraCaptureSession* session_ = nullptr;
    ACaptureRequest* request_ = nullptr;
    ACaptureRequest* sessionParameters_ = nullptr;
    ACameraOutputTarget* target_ = nullptr;
    ACaptureSessionOutput* output_ = nullptr;
    ANativeWindow* captureWindow_ = nullptr;
    ACaptureSessionOutput* captureOutput_ = nullptr;
    ACameraOutputTarget* captureTarget_ = nullptr;
    std::vector<CameraKeySetting> stillKeys_;
    std::string stillKeysCameraId_;
    AImageReader* companionReader_ = nullptr;
    ACaptureSessionOutput* companionOutput_ = nullptr;
    ACameraOutputTarget* companionTarget_ = nullptr;
    bool companionTargeted_ = false;
    ACaptureSessionOutputContainer* outputs_ = nullptr;
    ACameraMetadata* characteristics_ = nullptr;
    ANativeWindow* rawWindow_ = nullptr;
    CameraSessionCallbackContext* sessionContext_ = nullptr;
    CameraDeviceCallbackContext* deviceContext_ = nullptr;
    metadata::CameraContextMetadataPtr cameraContext_;
    std::optional<LevelOverride> staticLevels_;
    bool sessionKeysApplied_ = false;
    uint64_t generation_ = 0, readerGeneration_ = 0, closeGeneration_ = 0;
    bool closePending_ = false, forcedRetire_ = false;
    std::string heldGate_;  // LensRoute::vendorGate held for the open device.
    std::map<std::string, std::optional<uint32_t>> vendorTags_;  // name -> tag of the open camera
    std::condition_variable closed_;
};
}  // namespace rawrcam::camera
