#include <android/hardware_buffer.h>

#include <sstream>
#include <stdexcept>

#include "diagnostics/logging/NativeLog.h"
#include "session/SessionEngine.h"
#include "vulkan/RawAhbImporter.h"
#include "vulkan/VulkanDispatch.h"
namespace rawrcam::session {
namespace {
#define SESSION_LOGI(...) LOGI(__VA_ARGS__)
#define SESSION_LOGE(...) LOGE(__VA_ARGS__)
void vkCheck(VkResult result, const char* what) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(what) + " VkResult=" + std::to_string(result));
}
}  // namespace
bool SessionEngine::setPresentationSurface(JNIEnv* env, jobject surfaceObj, int displayRotationDegrees) {
    realtime_.setDisplayRotationDegrees(displayRotationDegrees);
    realtime_.coordinator().setDisplayRotationDegrees(realtime_.displayRotationDegrees());
    swapchainRenderer_.resetLogging();
    try {
        if (surfaceObj == nullptr) {
            // Still acquisition may outlive the activity surface. Stop feeding
            // the old swapchain immediately, even when resource detach must
            // wait for the claimed still to finish.
            realtime_.coordinator().setPresentationPaused(true);
            if (hqStillDetachedWorkActive()) {
                deferredSurfaceDetach_ = true;
                appendDiagnostic("STILL_BACKGROUND_SURFACE_DETACH_DEFERRED reason=hq_still_in_flight");
                return true;
            }
            detachPresentationSurface();
            return true;
        }

        if (!vulkanContext_.instance()) vulkanContext_.createInstance();

        realtime_.coordinator().setPresentationPaused(true);
        // A replacement Surface supersedes any previously deferred detach. HQ
        // still work owns independent resources and must survive foregrounding.
        deferredSurfaceDetach_ = false;
        if (vulkanContext_.device()) {
            std::lock_guard<std::mutex> queueLock(queueSubmitMutex_);
            vkCheck(vkQueueWaitIdle(vulkanContext_.queue()), "vkQueueWaitIdle surface replace");
        }

        swapchainRenderer_.destroySwapchain();
        presentationSurface_.replace(vulkanContext_.instance(), env, surfaceObj);

        if (!vulkanContext_.device()) {
            vulkanContext_.createDeviceForSurface(presentationSurface_.handle());
            realtime_.initializeDevice();
            swapchainRenderer_.initialize(vulkanContext_);

            SESSION_LOGI(
                "Vulkan GPU=%s driver=%s backend=%s R16_UINT storage=yes timestampPeriod=%.3fns ahbBufFeatures=0x%x "
                "ahbBufImportable=%s",
                vulkanContext_.gpuName().c_str(), vulkanContext_.driverName().c_str(),
                rawrcam::vulkan::dispatch::backendDescription().c_str(), vulkanContext_.timestampPeriod(),
                vulkanContext_.ahbExternalBufferFeatures(),
                (vulkanContext_.ahbExternalBufferFeatures() & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ? "yes"
                                                                                                         : "no");
            std::ostringstream out;
            out << "VULKAN_DEVICE gpu=" << vulkanContext_.gpuName() << " vendorId=0x" << std::hex
                << vulkanContext_.vendorId() << " deviceId=0x" << vulkanContext_.deviceId() << std::dec
                << " driverName=" << (vulkanContext_.driverName().empty() ? "unknown" : vulkanContext_.driverName())
                << " driverInfo=" << (vulkanContext_.driverInfo().empty() ? "unknown" : vulkanContext_.driverInfo())
                << " backend=" << rawrcam::vulkan::dispatch::backendDescription()
                << " R16_UINT_storage=yes timestampPeriodNs=" << vulkanContext_.timestampPeriod()
                << " ahbBufFeatures=0x" << std::hex << vulkanContext_.ahbExternalBufferFeatures() << std::dec
                << " ahbBufImportable="
                << ((vulkanContext_.ahbExternalBufferFeatures() & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ? "yes"
                                                                                                             : "no");
            appendDiagnostic(out.str());
        }

        swapchainRenderer_.createSwapchain(presentationSurface_.handle(), presentationSurface_.window());
        realtime_.coordinator().setPresentationPaused(false);
        return true;
    } catch (const std::exception& e) {
        realtime_.coordinator().setPresentationPaused(true);
        SESSION_LOGE("setSurface failed: %s", e.what());
        appendDiagnostic(std::string("SET_SURFACE_FAILURE ") + e.what());
        return false;
    }
}
ANativeWindow* SessionEngine::createRawInputWindow(uint64_t generation, uint32_t width, uint32_t height,
                                                   rawrcam::geometry::RawPixelFormat format) {
    try {
        destroyRawInputInternal();
        if (!realtime_.configured() || generation != realtime_.generation() || width != realtime_.rawWidth() ||
            height != realtime_.rawHeight()) {
            throw std::runtime_error("RAW input window geometry/generation mismatch");
        }

        if (!realtime_.importer()) {
            throw std::runtime_error("RAW AHB importer is unavailable");
        }

        // RAW10 can't be imported as R16 and a latched CPU ingress never imports
        // at all: request a plain CPU-readable buffer so the HAL doesn't pick a
        // GPU-private (compressed) layout the CPU copy can't read.
        if (format == rawrcam::geometry::RawPixelFormat::Raw10 || realtime_.coordinator().cpuIngressRequired()) {
            std::ostringstream out;
            out << "RAW_READER_CPU_INGRESS format=" << rawrcam::geometry::rawPixelFormatName(format)
                << " latched=" << (realtime_.coordinator().cpuIngressRequired() ? "true" : "false");
            SESSION_LOGI("%s", out.str().c_str());
            appendDiagnostic(out.str());
            // RAW10 normally goes through the GPU unpack, which reads the AHB
            // as a storage buffer: ask for GPU buffer access too, or the
            // allocation is CPU-only and the GPU sees incoherent data. CPU
            // read stays for the fallback upload.
            uint64_t usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;
            if (format == rawrcam::geometry::RawPixelFormat::Raw10 && !realtime_.coordinator().cpuIngressRequired())
                usage |= AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER;
            return rawFrameIngress_.create(generation, width, height, format, usage);
        }

        const uint32_t diagnosticMode = realtime_.coordinator().diagnosticMode();
        const VkImageUsageFlags importedVkUsage = realtime_.coordinator().importedRawUsage();
        uint64_t baselineReaderUsage = realtime_.importer()->requiredHardwareBufferUsage(importedVkUsage);
        // fix53a: COMPUTE sweep modes use a second, TRANSFER_SRC-only import of
        // the same AHB as the full-frame oracle. The reader must therefore be
        // allocated with the union of the AHB usage required by BOTH legal
        // VkImage import contracts. The candidate VkImage remains pure
        // STORAGE|SAMPLED; this only makes the independent oracle import legal.
        if (diagnosticMode == 13u || diagnosticMode == 14u) {
            baselineReaderUsage |= realtime_.importer()->requiredHardwareBufferUsage(VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        }
        const bool zeroCopySweep = diagnosticMode >= 11u && diagnosticMode <= 15u;
        const bool sweepCpuReader = diagnosticMode == 11u || diagnosticMode == 13u || diagnosticMode == 15u;
        const bool cpuReadableRawRequested =
            zeroCopySweep ? sweepCpuReader : (capture_.single().rawSnapshotConfigured() || rawCpuCopyProbe_.enabled());

        if (zeroCopySweep) {
            std::ostringstream out;
            out << "RAW_ZERO_COPY_SWEEP_CONFIG mode=" << diagnosticMode
                << " readerCpu=" << (sweepCpuReader ? "yes" : "no") << " importedVkUsage="
                << ((diagnosticMode == 11u || diagnosticMode == 12u || diagnosticMode == 15u) ? "MIXED" : "COMPUTE");
            appendDiagnostic(out.str());
        }

        if (cpuReadableRawRequested) {
            const uint64_t cpuReaderUsage = baselineReaderUsage | AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;
            {
                std::ostringstream out;
                out << "RAW_STILL_CAPTURE_READER_USAGE baseline=0x" << std::hex << baselineReaderUsage
                    << " requested=0x" << cpuReaderUsage
                    << " reason=" << (zeroCopySweep ? "zero_copy_sweep_cpu_reader" : "one_shot_cpu_snapshot");
                appendDiagnostic(out.str());
            }
            try {
                return rawFrameIngress_.create(generation, width, height, format, cpuReaderUsage);
            } catch (const std::exception& e) {
                if (zeroCopySweep) {
                    appendDiagnostic(std::string("RAW_ZERO_COPY_SWEEP_READER_FAILURE reason=") + e.what());
                    throw;
                }
                appendDiagnostic(std::string("RAW_STILL_CAPTURE_READER_USAGE_FALLBACK reason=") + e.what());
                capture_.single().disableRawSnapshot("cpu_readable_reader_configuration_failed");
                rawCpuCopyProbe_.disable("cpu_readable_reader_configuration_failed");
                return rawFrameIngress_.create(generation, width, height, format, baselineReaderUsage);
            }
        }

        return rawFrameIngress_.create(generation, width, height, format, baselineReaderUsage);
    } catch (const std::exception& e) {
        SESSION_LOGE("createRawInputWindow failed: %s", e.what());
        appendDiagnostic(std::string("RAW_READER_FAILURE ") + e.what());
        destroyRawInputInternal();
        return nullptr;
    }
}
void SessionEngine::destroyRawInput() {
    capture_.single().cancelPendingDngBeforeIngressDestroy();

    const bool preserveDetachedStill = hqStillDetachedWorkActive();
    if (!preserveDetachedStill) {
        capture_.single().resetHqProcessing();
        if (vulkanContext_.device()) vulkanContext_.waitIdle();
        realtime_.coordinator().releaseCompletedSlots(true);
    } else {
        appendDiagnostic("STILL_BACKGROUND_CAMERA_INGRESS_RELEASED hqStillPreserved=true");
        realtime_.coordinator().releaseCompletedSlots(false);
    }
    destroyRawInputInternal();
}
void SessionEngine::destroyRawInputInternal() noexcept {
    realtime_.coordinator().clearPairing();
    rawFrameIngress_.destroy();
}
bool SessionEngine::hqStillDetachedWorkActive() const noexcept { return capture_.detachedWorkActive(); }
void SessionEngine::detachPresentationSurface() noexcept {
    if (vulkanContext_.device()) {
        vulkanContext_.waitIdle();
    }
    swapchainRenderer_.destroySwapchain();
    presentationSurface_.reset();
    deferredSurfaceDetach_ = false;
}
void SessionEngine::finalizeDeferredSurfaceDetach() {
    if (!deferredSurfaceDetach_ || hqStillDetachedWorkActive()) return;
    appendDiagnostic("STILL_BACKGROUND_SURFACE_DETACH_FINALIZED hq_still_complete=true");
    detachPresentationSurface();
}
void SessionEngine::recreateSwapchainAfterOutOfDate() {
    if (!presentationSurface_.handle() || !presentationSurface_.window()) return;
    // Swapchain recovery is presentation-only. Never cancel independent
    // Demosaic/render/JPEG work to make presentation resources reusable.
    if (vulkanContext_.device()) {
        std::lock_guard<std::mutex> queueLock(queueSubmitMutex_);
        vkCheck(vkQueueWaitIdle(vulkanContext_.queue()), "vkQueueWaitIdle swapchain recovery");
    }
    swapchainRenderer_.createSwapchain(presentationSurface_.handle(), presentationSurface_.window());
}
}  // namespace rawrcam::session
