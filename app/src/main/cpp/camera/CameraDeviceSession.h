#pragma once
#include <android/native_window.h>
#include <camera/NdkCameraManager.h>

#include <chrono>
#include <functional>
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
    bool createSession(ANativeWindow*, const std::optional<LensRoute>&, const CameraControlState&,
                       const CameraMeteringRequest&, CameraCallbacks&, CameraRequestPipeline&, const Diagnostic&);
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

   private:
    void releaseRequestOutputs(bool orphan) noexcept;
    void releaseCharacteristics() noexcept;
    void closeDeviceDetached(std::function<void()> afterClose);
    ACameraManager* manager_ = nullptr;
    ACameraDevice* device_ = nullptr;
    ACameraCaptureSession* session_ = nullptr;
    ACaptureRequest* request_ = nullptr;
    ACaptureRequest* sessionParameters_ = nullptr;
    ACameraOutputTarget* target_ = nullptr;
    ACaptureSessionOutput* output_ = nullptr;
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
    std::condition_variable closed_;
};
}  // namespace rawrcam::camera
