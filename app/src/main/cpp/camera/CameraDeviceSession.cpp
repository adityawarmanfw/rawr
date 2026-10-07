#include "camera/CameraDeviceSession.h"

#include <camera/NdkCameraMetadataTags.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

#include "camera/CameraControlCapabilities.h"
#include "camera/CameraDiscovery.h"
#include "camera/CameraKeyInjection.h"
#include "metadata/CameraMetadataReader.h"
#include "vivo/VivoVendorTags.h"

namespace rawrcam::camera {
namespace {
bool ok(camera_status_t status) { return status == ACAMERA_OK; }
std::string statusText(camera_status_t status) { return std::to_string(static_cast<int>(status)); }

// Detached device closes still in flight. ACameraDevice_close runs on its own thread so a wedged HAL cannot
// stall teardown, but opening the next camera while one is still closing makes some HALs (MediaTek) refuse with
// MAX_CAMERA_IN_USE. open() waits for these, bounded, before it tries.
std::mutex gPendingCloseMutex;
std::condition_variable gPendingCloseCv;
int gPendingCloses = 0;

// Total time open() may spend waiting for closes and retrying an "in use" refusal.
constexpr std::chrono::milliseconds kOpenBudget{2500};
constexpr std::chrono::milliseconds kOpenRetryInitial{40};
constexpr std::chrono::milliseconds kOpenRetryMax{400};
}  // namespace
CameraDeviceSession::CameraDeviceSession() {
    manager_ = ACameraManager_create();
    if (!manager_) throw std::runtime_error("ACameraManager_create failed");
}
CameraDeviceSession::~CameraDeviceSession() {
    releaseCharacteristics();
    if (manager_) ACameraManager_delete(manager_);
}
void CameraDeviceSession::releaseRequestOutputs(bool orphan) noexcept {
    if (orphan) {
        request_ = nullptr;
        sessionParameters_ = nullptr;
        target_ = nullptr;
        output_ = nullptr;
        outputs_ = nullptr;
        return;
    }
    if (request_) ACaptureRequest_free(request_);
    request_ = nullptr;
    if (sessionParameters_) ACaptureRequest_free(sessionParameters_);
    sessionParameters_ = nullptr;
    if (target_) ACameraOutputTarget_free(target_);
    target_ = nullptr;
    if (outputs_ && output_) (void)ACaptureSessionOutputContainer_remove(outputs_, output_);
    if (output_) ACaptureSessionOutput_free(output_);
    output_ = nullptr;
    if (outputs_) ACaptureSessionOutputContainer_free(outputs_);
    outputs_ = nullptr;
}
void CameraDeviceSession::releaseCharacteristics() noexcept {
    if (characteristics_) ACameraMetadata_free(characteristics_);
    characteristics_ = nullptr;
}
void CameraDeviceSession::closeDeviceDetached(std::function<void()> afterClose) {
    ACameraDevice* closing = std::exchange(device_, nullptr);
    {
        std::lock_guard<std::mutex> lock(gPendingCloseMutex);
        ++gPendingCloses;
    }
    std::thread([closing, afterClose = std::move(afterClose)] {
        if (closing) ACameraDevice_close(closing);
        if (afterClose) afterClose();
        {
            std::lock_guard<std::mutex> lock(gPendingCloseMutex);
            --gPendingCloses;
        }
        gPendingCloseCv.notify_all();
    }).detach();
}

std::optional<LensRoute> CameraDeviceSession::select(const LensRoute& requested, const Diagnostic& diag) {
    ++generation_;
    emitCameraDiscovery(manager_, diag);
    ACameraMetadata* chars = nullptr;
    const camera_status_t cs = ACameraManager_getCameraCharacteristics(manager_, requested.cameraId.c_str(), &chars);
    if (!ok(cs) || !chars) {
        diag("CAMERA_NDK_SELECTION_REJECT cameraId=" + requested.cameraId +
             " stage=characteristics status=" + statusText(cs));
        return std::nullopt;
    }

    // A physical sub-camera streams through its logical parent: the device,
    // controls and vendor keys stay on the logical camera, while the RAW
    // stream sizes and sensor/color metadata come from the physical camera.
    ACameraMetadata* sensorChars = chars;
    if (!requested.physicalCameraId.empty()) {
        sensorChars = nullptr;
        const camera_status_t ps = ACameraManager_getCameraCharacteristics(
            manager_, requested.physicalCameraId.c_str(), &sensorChars);
        if (!ok(ps) || !sensorChars) {
            ACameraMetadata_free(chars);
            diag("CAMERA_NDK_SELECTION_REJECT cameraId=" + requested.cameraId +
                 " physicalCameraId=" + requested.physicalCameraId + " stage=physical_characteristics status=" +
                 statusText(ps));
            return std::nullopt;
        }
    }
    const auto releaseSensorChars = [&] {
        if (sensorChars != chars) ACameraMetadata_free(sensorChars);
        sensorChars = nullptr;
    };

    const auto negotiated = geometry::negotiateRawStream(rawOutputStreams(sensorChars), requested.preferredStream);
    if (!negotiated) {
        releaseSensorChars();
        ACameraMetadata_free(chars);
        diag("CAMERA_NDK_SELECTION_REJECT cameraId=" + requested.cameraId + " stage=raw_stream reason=no_usable_raw");
        return std::nullopt;
    }
    LensRoute route = requested;
    route.stream = *negotiated;
    {
        const auto& p = requested.preferredStream;
        std::ostringstream line;
        line << "CAMERA_NDK_RAW_STREAM_NEGOTIATED cameraId=" << route.cameraId
             << " physicalCameraId=" << (route.physicalCameraId.empty() ? "none" : route.physicalCameraId)
             << " lensId=" << route.lensId
             << " requested=" << geometry::rawPixelFormatName(p.format) << ':' << p.width << 'x' << p.height
             << " selected=" << geometry::rawPixelFormatName(route.stream.format) << ':' << route.stream.width << 'x'
             << route.stream.height;
        diag(line.str());
    }

    std::string metadataError;
    auto parsed = metadata::readCameraContextMetadata(sensorChars, route.cameraId, route.lensId, generation_,
                                                      route.stream.width, route.stream.height, &metadataError);
    releaseSensorChars();
    if (!parsed) {
        ACameraMetadata_free(chars);

        diag("CAMERA_NDK_SELECTION_REJECT cameraId=" + route.cameraId + " stage=metadata error=" + metadataError);
        return std::nullopt;
    }
    parsed->rawFormat = route.stream.format;

    cameraContext_ = std::make_shared<const metadata::CameraContextMetadata>(std::move(*parsed));
    characteristics_ = chars;
    diag(std::string("CAMERA_ZSL_PROBE enableZslInSessionKeys=") +
         (vivo::containsI32Tag(chars, ACAMERA_REQUEST_AVAILABLE_SESSION_KEYS, ACAMERA_CONTROL_ENABLE_ZSL) ? "true"
                                                                                                          : "false") +
         " repeatingIntent=PREVIEW enableZslRequest=false");

    return route;
}
CameraControlState CameraDeviceSession::initialControls(const LensRoute& route) const {
    return readInitialCameraControlState(characteristics_, route, generation_);
}
bool CameraDeviceSession::open(const LensRoute& route, CameraCallbacks& callbacks, const Diagnostic& diag,
                               std::unique_lock<std::mutex>& controllerLock,
                               const std::function<bool()>& stillCurrent) {
    const auto deadline = std::chrono::steady_clock::now() + kOpenBudget;
    {
        std::unique_lock<std::mutex> lock(gPendingCloseMutex);
        if (gPendingCloses > 0) {
            diag("CAMERA_NDK_OPEN_WAIT_CLOSE pending=" + std::to_string(gPendingCloses) +
                 " cameraId=" + route.cameraId);
            // Device close drains callbacks that also need the controller lock.
            controllerLock.unlock();
            gPendingCloseCv.wait_until(lock, deadline, [] { return gPendingCloses == 0; });
            // Never reacquire the controller lock while holding the close lock:
            // a concurrent teardown takes them in the opposite order.
            lock.unlock();
            controllerLock.lock();
        }
    }

    auto backoff = kOpenRetryInitial;
    for (int attempt = 1;; ++attempt) {
        if (!stillCurrent()) return false;
        auto* deviceContext = callbacks.deviceContext(generation_);
        deviceContext_ = deviceContext;
        auto state = CameraCallbacks::deviceState(deviceContext);
        const camera_status_t os = ACameraManager_openCamera(manager_, route.cameraId.c_str(), &state, &device_);
        if (ok(os) && device_) {
            if (attempt > 1) {
                diag("CAMERA_NDK_OPEN_RECOVERED cameraId=" + route.cameraId + " attempts=" + std::to_string(attempt));
            }
            return true;
        }
        // A failed open has no live device that can retain the context.
        delete deviceContext_;
        deviceContext_ = nullptr;
        diag("CAMERA_NDK_OPEN_FAILURE cameraId=" + route.cameraId + " status=" + statusText(os) +
             " attempt=" + std::to_string(attempt));

        // The HAL can keep reporting the previous camera as in use for a moment after its close returns.
        const bool busy = os == ACAMERA_ERROR_MAX_CAMERA_IN_USE || os == ACAMERA_ERROR_CAMERA_IN_USE;
        if (!busy || std::chrono::steady_clock::now() + backoff >= deadline) return false;
        controllerLock.unlock();
        std::this_thread::sleep_for(backoff);
        controllerLock.lock();
        backoff = std::min(backoff * 2, kOpenRetryMax);
    }
}
bool CameraDeviceSession::createSession(ANativeWindow* window, const std::optional<LensRoute>& selected,
                                        const CameraControlState& control, const CameraMeteringRequest& metering,
                                        CameraCallbacks& callbacks, CameraRequestPipeline& pipeline,
                                        const Diagnostic& diag) {
    rawWindow_ = window;

    if (!device_ || !rawWindow_) {
        return false;
    }

    auto fail = [&](const std::string& stage, const std::string& detail) -> bool {
        diag("CAMERA_NDK_SESSION_FAILURE stage=" + stage + " " + detail);

        return false;
    };

    camera_status_t s = ACaptureSessionOutputContainer_create(&outputs_);
    if (!ok(s)) return fail("container", "status=" + statusText(s));

    const std::string physicalCameraId = selected ? selected->physicalCameraId : std::string();
    s = physicalCameraId.empty() ? ACaptureSessionOutput_create(rawWindow_, &output_)
                                 : ACaptureSessionPhysicalOutput_create(rawWindow_, physicalCameraId.c_str(), &output_);
    if (!ok(s))
        return fail(physicalCameraId.empty() ? "output" : "physical_output",
                    "physicalCameraId=" + physicalCameraId + " status=" + statusText(s));

    s = ACaptureSessionOutputContainer_add(outputs_, output_);
    if (!ok(s)) return fail("add_output", "status=" + statusText(s));

    auto* callbackContext = callbacks.sessionContext(generation_);
    callbackContext->physicalCameraId = physicalCameraId;
    sessionContext_ = callbackContext;
    auto sessionCallbacks = CameraCallbacks::sessionState(callbackContext);
    staticLevels_.reset();
    sessionKeysApplied_ = false;

    // Camera2 explicitly recommends supplying a non-template AE target FPS
    // range as a session parameter. Doing this only on the repeating request
    // is too late on some HALs: the session may remain configured around the
    // TEMPLATE_PREVIEW 30/30 assumption and clamp shutter priority near 1/30.
    // Build one session-parameter request, then add the lens profile's
    // session keys (e.g. a vendor sensor/readout mode).
    s = ACameraDevice_createCaptureRequest(device_, TEMPLATE_PREVIEW, &sessionParameters_);
    if (!ok(s) || !sessionParameters_) {
        return fail("session_request", "status=" + statusText(s));
    }
    s = pipeline.applySessionCadence(sessionParameters_, control);
    if (!ok(s)) {
        return fail("priority_session_fps", "status=" + statusText(s));
    }

    const auto joined = [](const std::vector<std::string>& items) {
        std::string out;
        for (const auto& item : items) out += (out.empty() ? "" : ",") + item;
        return out;
    };
    if (selected) {
        // The user configured these keys; a lens that can't get them (e.g. a
        // crop-readout mode) would produce wrong frames, so fail loudly.
        const auto lensKeys = applyCameraKeySettings(sessionParameters_, characteristics_, selected->keys,
                                                     CameraKeySetting::Scope::Session, "lens", diag);
        if (!lensKeys.failures.empty())
            return fail("lens_session_keys", "lensId=" + selected->lensId + " failed=" + joined(lensKeys.failures));
        sessionKeysApplied_ = lensKeys.applied > 0;
        if (selected->levels.isStatic) staticLevels_ = selected->levels;
    }

    s = ACameraDevice_createCaptureSessionWithSessionParameters(device_, outputs_, sessionParameters_,
                                                                &sessionCallbacks, &session_);
    if (!ok(s) || !session_) {
        return fail(sessionKeysApplied_ ? "create_with_session_keys" : "create_standard_with_session_parameters",
                    "status=" + statusText(s));
    }

    const std::string lensDescription = "cameraId=" + (selected ? selected->cameraId : std::string("unknown")) +
                                        " lensId=" + (selected ? selected->lensId : std::string("unknown"));
    if (staticLevels_) {
        const auto& l = *staticLevels_;
        diag("CAMERA_PROFILE_STATIC_LEVELS " + lensDescription +
             " black=" + std::to_string(l.blackRggb[0]) + "," + std::to_string(l.blackRggb[1]) + "," +
             std::to_string(l.blackRggb[2]) + "," + std::to_string(l.blackRggb[3]) +
             " white=" + std::to_string(l.white));
    }

    s = ACameraDevice_createCaptureRequest(device_, TEMPLATE_PREVIEW, &request_);
    if (!ok(s) || !request_) {
        return fail("request", "status=" + statusText(s));
    }

    s = ACameraOutputTarget_create(rawWindow_, &target_);
    if (!ok(s) || !target_) {
        return fail("target", "status=" + statusText(s));
    }

    s = ACaptureRequest_addTarget(request_, target_);
    if (!ok(s)) {
        return fail("add_target", "status=" + statusText(s));
    }

    applyPreviewRequestDefaults(request_, *cameraContext_);
    // Stills, multiframe bursts and video all come from this repeating RAW
    // request, so request-scope keys only need to be set here.
    if (selected) {
        const auto lensKeys = applyCameraKeySettings(request_, characteristics_, selected->keys,
                                                     CameraKeySetting::Scope::Request, "lens", diag);
        if (!lensKeys.failures.empty())
            return fail("lens_request_keys", "lensId=" + selected->lensId + " failed=" + joined(lensKeys.failures));
    }
    if (!pipeline.submit(session_, request_, callbackContext, control, metering))
        return fail("initial_repeating", "status=control_or_submit_rejected");
    diag("CAMERA_NDK_SESSION_RUNNING");
    return true;
}
bool CameraDeviceSession::retire(std::unique_lock<std::mutex>& lock, std::optional<std::chrono::milliseconds> timeout,
                                 const char* diagnosticPrefix, const Diagnostic& diag) {
    if (!session_) return true;

    ACameraCaptureSession* const retiringSession = session_;
    const uint64_t retiringGeneration = generation_;
    closePending_ = true;
    closeGeneration_ = retiringGeneration;

    const camera_status_t stopStatus = ACameraCaptureSession_stopRepeating(retiringSession);
    diag(std::string(diagnosticPrefix) + "_RETIRE generation=" + std::to_string(retiringGeneration) +
         " stopRepeatingStatus=" + statusText(stopStatus));

    ACameraCaptureSession_close(retiringSession);

    const auto closedPredicate = [this, retiringGeneration] {
        return !closePending_ || closeGeneration_ != retiringGeneration;
    };

    bool closed = true;
    if (timeout) {
        closed = closed_.wait_for(lock, *timeout, closedPredicate);
    } else {
        closed_.wait(lock, closedPredicate);
    }
    if (!closed) {
        diag(std::string(diagnosticPrefix) + "_CLOSE_TIMEOUT generation=" + std::to_string(retiringGeneration));
        return false;
    }

    // onClosed owns/deletes its callback context and signals only after its
    // final access to the event sink. Once the predicate is satisfied there can be no
    // late session-state callback dereference through this context.
    session_ = nullptr;
    sessionContext_ = nullptr;
    diag(std::string(diagnosticPrefix) + "_CLOSED generation=" + std::to_string(retiringGeneration));
    return true;
}
void CameraDeviceSession::forceRetire(const char* diagnosticPrefix, const Diagnostic& diag) {
    if (!session_ && !closePending_) return;
    diag(std::string(diagnosticPrefix) + "_CLOSE_FORCED generation=" + std::to_string(generation_));
    session_ = nullptr;
    sessionContext_ = nullptr;
    closePending_ = false;
    closed_.notify_all();
    forcedRetire_ = true;
}
CameraReaderRetirement CameraDeviceSession::release(const Diagnostic& diag) {
    // A live session must be retired before request/output/device state and
    // callback provenance are released.
    if (session_) {
        diag("CAMERA_NDK_TEARDOWN_DEFERRED reason=session_not_retired");
        return {};
    }
    diag("CAMERA_NDK_TEARDOWN_BEGIN hasDevice=" + std::string(device_ ? "true" : "false") +
         " generation=" + std::to_string(generation_));

    ++generation_;
    const bool minimal = forcedRetire_;
    forcedRetire_ = false;
    if (minimal) {
        diag("CAMERA_NDK_TEARDOWN_MINIMAL reason=forced_retire generation=" + std::to_string(generation_));
        rawWindow_ = nullptr;
    }
    releaseRequestOutputs(minimal);
    staticLevels_.reset();
    sessionKeysApplied_ = false;
    if (device_) {
        // ACameraDevice_close() can hang forever on a HAL-wedged device
        // (observed after ERROR_CAMERA_DEVICE with no onClosed: the
        // worker wedged here and every later start queued behind it
        // forever -> black viewfinder with live UI). Close on a detached
        // thread and continue teardown immediately; the orphaned close is
        // bounded to one thread per wedge and the next start reopens the
        // device (retrying via the generation watchdog if the HAL still
        // reports the camera as in use).
        CameraDeviceCallbackContext* const closingContext = deviceContext_;
        deviceContext_ = nullptr;
        diag("CAMERA_NDK_DEVICE_CLOSE_DETACHED generation=" + std::to_string(generation_));
        closeDeviceDetached([closingContext] { delete closingContext; });
    } else if (deviceContext_) {
        delete deviceContext_;
        deviceContext_ = nullptr;
    }
    releaseCharacteristics();
    cameraContext_.reset();
    const auto reader = CameraReaderRetirement{readerGeneration_, !minimal};
    rawWindow_ = nullptr;
    diag("CAMERA_NDK_STOP generation=" + std::to_string(generation_));
    return reader;
}
void CameraDeviceSession::rollbackUnacceptedContext() noexcept {
    if (!session_ && sessionContext_) {
        delete sessionContext_;
        sessionContext_ = nullptr;
    }
}
void CameraDeviceSession::sessionClosed(uint64_t callbackGeneration, ACameraCaptureSession* closedSession) {
    if (session_ == closedSession && callbackGeneration == generation_) {
        session_ = nullptr;
        sessionContext_ = nullptr;
    }
    if (closePending_ && closeGeneration_ == callbackGeneration) {
        closePending_ = false;
        closed_.notify_all();
    }
}

}  // namespace rawrcam::camera
