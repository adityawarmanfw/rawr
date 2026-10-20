#include "camera/CameraDeviceSession.h"
#include "camera/VendorCameraGate.h"

#include <camera/NdkCameraMetadataTags.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

#include "camera/CameraControlCapabilities.h"
#include "camera/CameraDiscovery.h"
#include "camera/CameraKeyInjection.h"
#include "diagnostics/logging/RuntimeTraceRecorder.h"
#include "geometry/CfaPattern.h"
#include "metadata/CameraMetadataReader.h"
#include "camera/CameraMetadataTags.h"

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
    releaseHeldGate();
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
        // The orphaned session may still write to the companion and capture windows.
        captureWindow_ = nullptr;
        captureOutput_ = nullptr;
        captureTarget_ = nullptr;
        companionReader_ = nullptr;
        companionOutput_ = nullptr;
        companionTarget_ = nullptr;
        companionTargeted_ = false;
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
    if (captureTarget_) ACameraOutputTarget_free(captureTarget_);
    captureTarget_ = nullptr;
    if (outputs_ && captureOutput_) (void)ACaptureSessionOutputContainer_remove(outputs_, captureOutput_);
    if (captureOutput_) ACaptureSessionOutput_free(captureOutput_);
    captureOutput_ = nullptr;
    captureWindow_ = nullptr;
    stillKeys_.clear();
    if (companionTarget_) ACameraOutputTarget_free(companionTarget_);
    companionTarget_ = nullptr;
    companionTargeted_ = false;
    if (outputs_ && companionOutput_) (void)ACaptureSessionOutputContainer_remove(outputs_, companionOutput_);
    if (companionOutput_) ACaptureSessionOutput_free(companionOutput_);
    companionOutput_ = nullptr;
    if (companionReader_) AImageReader_delete(companionReader_);
    companionReader_ = nullptr;
    if (outputs_) ACaptureSessionOutputContainer_free(outputs_);
    outputs_ = nullptr;
}
void CameraDeviceSession::releaseCharacteristics() noexcept {
    if (characteristics_) ACameraMetadata_free(characteristics_);
    characteristics_ = nullptr;
    vendorTags_.clear();
}
std::optional<int64_t> CameraDeviceSession::vendorResultValue(const ACameraMetadata* result, const std::string& name) {
    if (!result || !characteristics_ || !cameraContext_) return std::nullopt;
    auto it = vendorTags_.find(name);
    if (it == vendorTags_.end())
        it = vendorTags_.emplace(name, resolveMetadataTagByNameCompat(characteristics_, cameraContext_->cameraId.c_str(),
                                                                      name.c_str(), nullptr)).first;
    if (!it->second) return std::nullopt;
    ACameraMetadata_const_entry e{};
    if (ACameraMetadata_getConstEntry(result, *it->second, &e) != ACAMERA_OK || e.count == 0) return std::nullopt;
    switch (e.type) {
        case ACAMERA_TYPE_BYTE: return e.data.u8[0];
        case ACAMERA_TYPE_INT32: return e.data.i32[0];
        case ACAMERA_TYPE_INT64: return e.data.i64[0];
        default: return std::nullopt;
    }
}
void CameraDeviceSession::releaseHeldGate() noexcept {
    if (heldGate_.empty()) return;
    releaseVendorCameraGate(heldGate_);
    heldGate_.clear();
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

    // An unlisted (vendor full-size) stream is configured as the profile says; the HAL decides at configure time.
    const auto negotiated =
        requested.unlistedStream && requested.preferredStream.width && requested.preferredStream.height
            ? std::optional<geometry::RawStreamOption>(geometry::RawStreamOption{requested.preferredStream.format,
                                                                                requested.preferredStream.width,
                                                                                requested.preferredStream.height})
            : geometry::negotiateRawStream(rawOutputStreams(sensorChars), requested.preferredStream);
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
    parsed->rawReaderUsage = route.readerUsage;
    parsed->cfaBlock = route.cfaBlock;
    parsed->cfaLiveQuality = route.cfaLiveQuality;
    parsed->cfaPhotoQuality = route.cfaPhotoQuality;
    if (route.cfaOrder && int32_t(*route.cfaOrder) != parsed->camera2Cfa) {
        // The black pattern is per sensor position: undo the reported order, apply the profile's.
        const auto pattern = geometry::reorderRggbByCode(parsed->baselineBlackLevelPhysicalRggb, parsed->camera2Cfa);
        parsed->baselineBlackLevelPhysicalRggb = geometry::reorderRggbByCode(pattern, *route.cfaOrder);
        parsed->camera2Cfa = int32_t(*route.cfaOrder);
        parsed->rawPreviewCfa = *route.cfaOrder;
    }
    if (route.cfaBlock > 1 || route.cfaOrder)
        diag("CAMERA_CFA_OVERRIDE lens=" + route.lensId + " block=" + std::to_string(route.cfaBlock) +
             " order=" + std::to_string(parsed->rawPreviewCfa) + " live=" + std::to_string(route.cfaLiveQuality) +
             " photo=" + std::to_string(route.cfaPhotoQuality));

    cameraContext_ = std::make_shared<const metadata::CameraContextMetadata>(std::move(*parsed));
    characteristics_ = chars;
    diag(std::string("CAMERA_ZSL_PROBE enableZslInSessionKeys=") +
         (containsI32Tag(chars, ACAMERA_REQUEST_AVAILABLE_SESSION_KEYS, ACAMERA_CONTROL_ENABLE_ZSL) ? "true"
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

    // Vendor state the route needs while open; released when the device has closed.
    if (!route.vendorGate.empty() && heldGate_ != route.vendorGate) {
        releaseHeldGate();
        std::string gateError;
        if (!acquireVendorCameraGate(route.vendorGate, &gateError)) {
            diag("CAMERA_NDK_VENDOR_GATE_REFUSED gate=" + route.vendorGate + " cameraId=" + route.cameraId +
                 " reason=" + gateError);
            return false;
        }
        heldGate_ = route.vendorGate;
        diag("CAMERA_NDK_VENDOR_GATE_ACQUIRED gate=" + route.vendorGate + " cameraId=" + route.cameraId);
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
        if (!busy || std::chrono::steady_clock::now() + backoff >= deadline) {
            releaseHeldGate();
            return false;
        }
        controllerLock.unlock();
        std::this_thread::sleep_for(backoff);
        controllerLock.lock();
        backoff = std::min(backoff * 2, kOpenRetryMax);
    }
}
bool CameraDeviceSession::addCompanionStream(const std::string& physicalCameraId, const Diagnostic& diag) {
    // A physical output must use a size the physical camera advertises; the
    // logical camera's list can differ and the HAL rejects the configure.
    ACameraMetadata* sizeChars = characteristics_;
    if (!physicalCameraId.empty() &&
        !ok(ACameraManager_getCameraCharacteristics(manager_, physicalCameraId.c_str(), &sizeChars)))
        sizeChars = nullptr;
    const auto size = sizeChars ? smallestYuvOutput(sizeChars, 320, 240) : std::pair<int32_t, int32_t>{0, 0};
    if (sizeChars && sizeChars != characteristics_) ACameraMetadata_free(sizeChars);
    const int32_t width = size.first, height = size.second;
    const auto fail = [&](const std::string& stage, int status) {
        diag("CAMERA_COMPANION_STREAM size=" + std::to_string(width) + "x" + std::to_string(height) +
             " stage=" + stage + " status=" + std::to_string(status) + " fallback=raw_only");
        return false;
    };
    if (width <= 0 || height <= 0) return fail("size", 0);
    const media_status_t ms = AImageReader_new(width, height, AIMAGE_FORMAT_YUV_420_888, 2, &companionReader_);
    if (ms != AMEDIA_OK || !companionReader_) return fail("reader", ms);
    // Frames exist only to keep the HAL streaming; drop each one on arrival.
    AImageReader_ImageListener listener{};
    listener.onImageAvailable = [](void*, AImageReader* reader) {
        AImage* image = nullptr;
        while (AImageReader_acquireNextImage(reader, &image) == AMEDIA_OK && image) {
            AImage_delete(image);
            image = nullptr;
        }
    };
    if (const auto ls = AImageReader_setImageListener(companionReader_, &listener); ls != AMEDIA_OK)
        return fail("listener", ls);
    ANativeWindow* window = nullptr;
    if (const auto ws = AImageReader_getWindow(companionReader_, &window); ws != AMEDIA_OK || !window)
        return fail("window", ws);
    camera_status_t s = physicalCameraId.empty()
                            ? ACaptureSessionOutput_create(window, &companionOutput_)
                            : ACaptureSessionPhysicalOutput_create(window, physicalCameraId.c_str(), &companionOutput_);
    if (!ok(s)) return fail("output", s);
    s = ACaptureSessionOutputContainer_add(outputs_, companionOutput_);
    if (!ok(s)) return fail("add_output", s);
    s = ACameraOutputTarget_create(window, &companionTarget_);
    if (!ok(s) || !companionTarget_) return fail("target", s);
    diag("CAMERA_COMPANION_STREAM size=" + std::to_string(width) + "x" + std::to_string(height) +
         " format=YUV_420_888 status=0");
    return true;
}
bool CameraDeviceSession::detachCompanionTarget(const Diagnostic& diag) {
    if (!companionTargeted_ || !request_ || !companionTarget_) return false;
    const camera_status_t s = ACaptureRequest_removeTarget(request_, companionTarget_);
    diag("CAMERA_COMPANION_TARGET_DETACHED status=" + statusText(s));
    if (!ok(s)) return false;
    companionTargeted_ = false;
    return true;
}
bool CameraDeviceSession::createSession(ANativeWindow* window, const std::optional<LensRoute>& selected,
                                        const CameraControlState& control, const CameraMeteringRequest& metering,
                                        CameraCallbacks& callbacks, CameraRequestPipeline& pipeline,
                                        const Diagnostic& diag, bool companionStream) {
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
    if (companionStream && !addCompanionStream(physicalCameraId, diag)) {
        // RAW-only still works on most devices; never block the start on it.
        if (outputs_ && companionOutput_) (void)ACaptureSessionOutputContainer_remove(outputs_, companionOutput_);
        if (companionOutput_) ACaptureSessionOutput_free(companionOutput_);
        companionOutput_ = nullptr;
        if (companionTarget_) ACameraOutputTarget_free(companionTarget_);
        companionTarget_ = nullptr;
        if (companionReader_) AImageReader_delete(companionReader_);
        companionReader_ = nullptr;
    }

    if (captureWindow_) {
        // Physical outputs would need the physical camera's sizes; capture routes name a camera directly.
        if (!physicalCameraId.empty()) return fail("capture_output", "reason=physical_camera_unsupported");
        s = ACaptureSessionOutput_create(captureWindow_, &captureOutput_);
        if (!ok(s)) return fail("capture_output", "status=" + statusText(s));
        s = ACaptureSessionOutputContainer_add(outputs_, captureOutput_);
        if (!ok(s)) return fail("add_capture_output", "status=" + statusText(s));
        s = ACameraOutputTarget_create(captureWindow_, &captureTarget_);
        if (!ok(s) || !captureTarget_) return fail("capture_target", "status=" + statusText(s));
        if (selected) {
            stillKeys_ = selected->keys;
            stillKeysCameraId_ = selected->cameraId;
        }
    }

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
        const auto lensKeys = applyCameraKeySettings(sessionParameters_, characteristics_, selected->keys, selected->cameraId.c_str(),
                                                     CameraKeySetting::Scope::Session, "lens", diag);
        if (!lensKeys.failures.empty())
            return fail("lens_session_keys", "lensId=" + selected->lensId + " failed=" + joined(lensKeys.failures));
        sessionKeysApplied_ = lensKeys.applied > 0;
        if (selected->levels.isStatic) staticLevels_ = selected->levels;
    }

    // cameraserver reads the vendor gate (vivo key 40) only while it
    // configures the streams, which this call does synchronously: hold it for
    // the call alone, so a killed process can't leave it set behind a running
    // session. A later session on the same device takes it again.
    if (selected && !selected->vendorGate.empty() && heldGate_.empty()) {
        std::string gateError;
        if (!acquireVendorCameraGate(selected->vendorGate, &gateError))
            return fail("vendor_gate", "gate=" + selected->vendorGate + " reason=" + gateError);
        heldGate_ = selected->vendorGate;
    }
    s = ACameraDevice_createCaptureSessionWithSessionParameters(device_, outputs_, sessionParameters_,
                                                                &sessionCallbacks, &session_);
    // Capture-stream routes keep it until the device closes: the HAL rebuilds
    // its still pipeline per full-size request and crashes if the client is no
    // longer vivo's camera by then.
    if (!heldGate_.empty() && !captureWindow_) {
        diag("CAMERA_NDK_VENDOR_GATE_RELEASED gate=" + heldGate_ + " after=configure");
        releaseHeldGate();
    }
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
    if (companionTarget_) {
        s = ACaptureRequest_addTarget(request_, companionTarget_);
        if (!ok(s)) return fail("add_companion_target", "status=" + statusText(s));
        companionTargeted_ = true;
    }

    applyPreviewRequestDefaults(request_, *cameraContext_);
    // Stills, multiframe bursts and video all come from this repeating RAW
    // request, so request-scope keys only need to be set here.
    if (selected) {
        const auto lensKeys = applyCameraKeySettings(request_, characteristics_, selected->keys, selected->cameraId.c_str(),
                                                     CameraKeySetting::Scope::Request, "lens", diag);
        if (!lensKeys.failures.empty())
            return fail("lens_request_keys", "lensId=" + selected->lensId + " failed=" + joined(lensKeys.failures));
    }
    if (!pipeline.submit(session_, request_, callbackContext, control, metering))
        return fail("initial_repeating", "status=control_or_submit_rejected");
    diag("CAMERA_NDK_SESSION_RUNNING companion=" + std::string(companionTarget_ ? "1" : "0") +
         " captureStream=" + (captureTarget_ ? "1" : "0"));
    const auto& raw = cameraContext_->geometry;
    diagnostics::RuntimeTraceRecorder::instance().record(
        diagnostics::RuntimeTraceStage::CameraSessionCreated, 0, generation_, -1, 0, 0, companionTarget_ ? 1u : 0u,
        (static_cast<int64_t>(raw.rawBufferWidth) << 32) | static_cast<int64_t>(raw.rawBufferHeight),
        std::atoi(control.capabilities.cameraId.c_str()));
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
        // The vendor gate goes back only after ACameraDevice_close has returned.
        std::string gate = std::exchange(heldGate_, std::string());
        closeDeviceDetached([closingContext, gate] {
            delete closingContext;
            if (!gate.empty()) releaseVendorCameraGate(gate);
        });
    } else {
        if (deviceContext_) {
            delete deviceContext_;
            deviceContext_ = nullptr;
        }
        releaseHeldGate();
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

namespace rawrcam::camera {
bool CameraDeviceSession::captureFullSize(CameraRequestPipeline& pipeline, const CameraControlState& control,
                                          const std::vector<CameraRequestPipeline::BracketExposure>& exposures,
                                          uint64_t tagId, const Diagnostic& diag) {
    if (!session_ || !request_ || !captureTarget_ || !sessionContext_) return false;
    bool keysLogged = false;
    // vivo's per-still ids (its camera app's values: request id = frame flag =
    // a millisecond stamp, global capture id = stamp + 5); a still without
    // them crashes the provider's request metadata handling.
    int64_t stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
    // Build from the still template, not the repeating request: a copy of the
    // preview request (rawr's statistics / map / region settings) crashes the
    // provider's request metadata handling on these stills.
    ACaptureRequest* base = nullptr;
    if (!ok(ACameraDevice_createCaptureRequest(device_, TEMPLATE_STILL_CAPTURE, &base)) || !base) return false;
    struct FreeBase {
        ACaptureRequest* r;
        ~FreeBase() { ACaptureRequest_free(r); }
    } freeBase{base};
    if (!ok(ACaptureRequest_addTarget(base, target_))) return false;
    const auto requestKeys = applyCameraKeySettings(base, characteristics_, stillKeys_, stillKeysCameraId_.c_str(),
                                                    CameraKeySetting::Scope::Request, "lens",
                                                    [](const std::string&) {});
    if (!requestKeys.failures.empty()) return false;
    return pipeline.captureBracket(session_, base, sessionContext_, control, exposures, tagId,
                                   [&](ACaptureRequest* request) {
        if (ACaptureRequest_addTarget(request, captureTarget_) != ACAMERA_OK) return false;
        const uint8_t intent = ACAMERA_CONTROL_CAPTURE_INTENT_STILL_CAPTURE;
        (void)ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_CAPTURE_INTENT, 1, &intent);
        // Preserve the actual preview AF mode and retained tap region. Changing
        // AUTO/CONTINUOUS to OFF for one still resets AF and can move the lens.
        // Copy only these focus keys onto the fresh template: cloning the whole
        // preview request is unsafe on this vendor's full-size path.
        ACameraMetadata_const_entry af{};
        if (ACaptureRequest_getConstEntry(request_, ACAMERA_CONTROL_AF_MODE, &af) != ACAMERA_OK || af.count != 1 ||
            !af.data.u8)
            return false;
        const uint8_t afMode = af.data.u8[0];
        if (ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_AF_MODE, 1, &afMode) != ACAMERA_OK) return false;
        ACameraMetadata_const_entry region{};
        if (ACaptureRequest_getConstEntry(request_, ACAMERA_CONTROL_AF_REGIONS, &region) == ACAMERA_OK &&
            region.count == 5 && region.data.i32) {
            if (ACaptureRequest_setEntry_i32(request, ACAMERA_CONTROL_AF_REGIONS, 5, region.data.i32) != ACAMERA_OK)
                return false;
        }
        // Only the user's MF mode owns a lens-distance request. An AF result's
        // distance is telemetry, not a reason to switch to manual on shutter.
        if (afMode == ACAMERA_CONTROL_AF_MODE_OFF && control.capabilities.manualFocusSupported) {
            if (!control.stillFocusDistance || !std::isfinite(*control.stillFocusDistance)) return false;
            const float distance = *control.stillFocusDistance;
            if (ACaptureRequest_setEntry_float(request, ACAMERA_LENS_FOCUS_DISTANCE, 1, &distance) != ACAMERA_OK)
                return false;
        }
        auto keys = stillKeys_;
        const double id = double(stamp++);
        for (const auto& [tag, value] : {std::pair{"vivo.control.requestId", id}, std::pair{"vivo.control.frameFlag", id},
                                         std::pair{"vivo.control.globalCaptureId", id + 5}}) {
            CameraKeySetting k;
            k.tag = tag;
            k.type = CameraKeySetting::Type::Int64;
            k.scope = CameraKeySetting::Scope::Still;
            k.values = {value};
            keys.push_back(std::move(k));
        }
        // Log the still keys once per burst.
        const auto result = applyCameraKeySettings(request, characteristics_, keys, stillKeysCameraId_.c_str(),
                                                   CameraKeySetting::Scope::Still, "lens",
                                                   keysLogged ? Diagnostic([](const std::string&) {}) : diag);
        keysLogged = true;
        return result.failures.empty();
    });
}
}  // namespace rawrcam::camera
