// Monitoring/overlay/probe/diagnostic toggles.
#include "diagnostics/logging/NativeLog.h"
#include "session/SessionEngine.h"

namespace rawrcam::session {

void SessionEngine::setCpuRawCopyProbeFrames(uint32_t frames) {
    std::lock_guard<std::mutex> lock(mu_);
    rawCpuCopyProbe_.setRequestedFrames(frames);
    appendDiagnostic("CPU_RAW_COPY_PROBE_REQUEST frames=" + std::to_string(rawCpuCopyProbe_.requestedFrames()) +
                     " appliesOnNextRawReader=true");
}
void SessionEngine::setPersistentDiagnosticsEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mu_);
    if (diagnosticSink_) {
        diagnosticSink_->setEnabled(enabled);
        if (enabled) {
            // Vulkan dispatch is selected before SessionEngine construction. Persist
            // its resolved backend here as well so diagnostics activation order cannot
            // hide an earlier startup/fallback decision.
            diagnosticSink_->appendValidation(std::string("GPU_DRIVER_BACKEND backend=") +
                                              rawrcam::vulkan::dispatch::backendDescription());

            // Vulkan may already have been initialized before persistent diagnostics
            // became enabled. Replay the current device identity so enabling the sink
            // cannot erase the only proof of which ICD actually created the device.
            if (vulkanContext_.device() != VK_NULL_HANDLE) {
                std::ostringstream out;
                out << "VULKAN_DEVICE gpu=" << vulkanContext_.gpuName() << " vendorId=0x" << std::hex
                    << vulkanContext_.vendorId() << " deviceId=0x" << vulkanContext_.deviceId() << std::dec
                    << " driverName=" << (vulkanContext_.driverName().empty() ? "unknown" : vulkanContext_.driverName())
                    << " driverInfo=" << (vulkanContext_.driverInfo().empty() ? "unknown" : vulkanContext_.driverInfo())
                    << " backend=" << rawrcam::vulkan::dispatch::backendDescription()
                    << " R16_UINT_storage=yes timestampPeriodNs=" << vulkanContext_.timestampPeriod()
                    << " ahbBufFeatures=0x" << std::hex << vulkanContext_.ahbExternalBufferFeatures() << std::dec
                    << " ahbBufImportable="
                    << ((vulkanContext_.ahbExternalBufferFeatures() & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)
                            ? "yes"
                            : "no");
                diagnosticSink_->appendValidation(out.str());
            }
        }
    }
    if (frameAuditWriter_) frameAuditWriter_->setEnabled(enabled);
    LOGI("PERSISTENT_DIAGNOSTICS enabled=%s", enabled ? "true" : "false");
}

void SessionEngine::setLensShadingCorrectionEnabled(bool enabled) {
    realtime_.coordinator().setLensShadingCorrectionEnabled(enabled);
    const std::string line = "LENS_SHADING_CORRECTION enabled=" + std::string(enabled ? "true" : "false");
    LOGI("%s", line.c_str());
    appendDiagnostic(line);
}

void SessionEngine::setHighlightReconstructionEnabled(bool enabled) {
    realtime_.coordinator().setHighlightReconstructionEnabled(enabled);
    appendDiagnostic("HIGHLIGHT_RECONSTRUCTION enabled=" + std::string(enabled ? "true" : "false") +
                     " domain=shared_cfa preview=true still=per_capture");
}

void SessionEngine::setMaxAePostGain(float gain) {
    if (!std::isfinite(gain)) return;
    gain = std::clamp(gain, 1.0f, 16.0f);
    // Snap to the supported UI steps (1x/2x/4x); nearest wins.
    float snapped = 4.0f;
    float best = std::abs(gain - 4.0f);
    if (std::abs(gain - 1.0f) < best) {
        snapped = 1.0f;
        best = std::abs(gain - 1.0f);
    }
    if (std::abs(gain - 2.0f) < best) {
        snapped = 2.0f;
    }
    maxAePostGain_.store(snapped, std::memory_order_relaxed);
    appendDiagnostic("AE_POSTGAIN_CAP maxGain=" + std::to_string(snapped));
}

void SessionEngine::setPostGainKneeWidthEv(float widthEv) {
    if (!std::isfinite(widthEv)) return;
    widthEv = std::clamp(widthEv, 0.25f, 4.0f);
    postGainKneeWidthEv_.store(widthEv, std::memory_order_relaxed);
    appendDiagnostic("AE_POSTGAIN_KNEE widthEv=" + std::to_string(widthEv));
}

void SessionEngine::setExperimentalZeroCopy(bool enabled) {
    std::lock_guard<std::mutex> lock(mu_);
    realtime_.coordinator().setExperimentalZeroCopy(enabled);
    cameraControls_.setZeroCopy(enabled);
    std::ostringstream d;
    d << "EXPERIMENTAL_ZERO_COPY enabled=" << (enabled ? "true" : "false")
      << " ingress=" << (enabled ? "imported_AHB_STORAGE_direct" : "vkCmdCopyImage_bridge");
    LOGI("%s", d.str().c_str());
    appendDiagnostic(d.str());
}

void SessionEngine::setPipelineDiagnostic(uint32_t mode) {
    std::lock_guard<std::mutex> lock(mu_);
    static constexpr const char* kNames[] = {"camera_transfer",
                                             "screen_checker",
                                             "tone_pattern",
                                             "linear_pattern",
                                             "linear_actual",
                                             "uv_transform",
                                             "raw_copy",
                                             "raw_storage_viz",
                                             "raw_sampled_viz",
                                             "raw_transfer_copy",
                                             "camera_direct",
                                             "zero_copy_mixed_cpu",
                                             "zero_copy_mixed_gpu",
                                             "zero_copy_compute_cpu",
                                             "zero_copy_compute_gpu",
                                             "zero_copy_final",
                                             "zero_copy_buffer"};
    static constexpr uint32_t kDiagnosticModeCount = static_cast<uint32_t>(sizeof(kNames) / sizeof(kNames[0]));
    if (mode >= kDiagnosticModeCount) mode = 0u;
    diagnosticMode_ = mode;
    realtime_.coordinator().setDiagnosticMode(diagnosticMode_);
    swapchainRenderer_.resetLogging();
    std::ostringstream d;
    d << "PIPELINE_DIAGNOSTIC mode=" << kNames[mode] << " value=" << mode;
    LOGI("%s", d.str().c_str());
    appendDiagnostic(d.str());
    if (mode == 0u) {
        appendDiagnostic(
            "RAW_INGRESS_POLICY mode=production bridge=vkCmdCopyImage imported_AHB_TRANSFER_SRC->owned_R16_UINT "
            "zeroCopy=false reason=direct_storage_access_corrupt_on_target");
    } else if (mode == 10u) {
        appendDiagnostic(
            "RAW_INGRESS_POLICY mode=diagnostic_direct imported_AHB_STORAGE->raw_preview zeroCopy=true "
            "knownBrokenOnTarget=true");
    } else if (mode == 16u) {
        appendDiagnostic(
            "RAW_INGRESS_POLICY mode=buffer_parity bridge=vkCmdCopyImage imported_AHB_TRANSFER_SRC->owned_R16_UINT "
            "plus imported_AHB_storage_buffer parity zeroCopy=buffer_view");
    }
}

}  // namespace rawrcam::session
