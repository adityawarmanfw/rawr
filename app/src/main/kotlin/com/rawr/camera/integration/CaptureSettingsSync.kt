package com.rawr.camera.integration

import com.rawr.camera.architecture.CaptureScreenController
import com.rawr.camera.architecture.SetControlSurfaceStyle
import com.rawr.camera.architecture.SyncToneControls
import com.rawr.camera.model.CaptureMode
import com.rawr.camera.settings.model.*
import com.rawr.camera.video.VideoResolutionMode
import com.rawr.camera.video.toVideoImageSettings
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.filterNotNull
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.take
import kotlinx.coroutines.launch

/** Projects settings onto native preview and reports initial submission to its start gate. */
internal class CaptureSettingsSync(
    private val scope: CoroutineScope,
    private val values: Flow<SettingsValues>,
    private val latestSettings: () -> SettingsValues,
    private val previewCoordinator: RawPreviewCoordinator,
    private val controller: CaptureScreenController
) {
    private val bindings = mutableListOf<suspend () -> Unit>()
    private var firstApplies = 0
    private var started = false

    fun start() {
        check(!started)
        started = true
        syncSetting({ it.controlSurfaceStyle }) { controller.dispatch(SetControlSurfaceStyle(it)) }
        syncSetting({ it.oisEnabledPreference }) { previewCoordinator.setOisEnabled(it) }
        syncSetting({ it.lensProfiles }) { previewCoordinator.setLensProfiles(it) }
        // Start on the last used lens. Startup only: later values come from
        // the user's own switches, and replaying one could undo a newer switch.
        // Native falls back to the first lens if this one left the profile.
        syncStream(values.map { it.lastLensId }.take(1)) { id -> id?.let(previewCoordinator::selectLens) }
        syncSetting({ it.antiFlicker }) {
            previewCoordinator.setAntibandingMode(com.rawr.camera.settings.model.antibandingModeFor(it))
        }
        syncSetting({ it.highlightProtection }) {
            previewCoordinator.setPostGainKneeWidthEv(com.rawr.camera.settings.model.postGainKneeWidthEvFor(it))
        }
        syncSetting({ it.maxPostGainId }) {
            previewCoordinator.setMaxAePostGain(com.rawr.camera.settings.model.maxPostGainForId(it))
        }
        syncSetting({ it.aePriorityDisabled }) { previewCoordinator.setAePriorityDisabled(it) }
        syncSetting({ it.autoMinFpsId }) {
            previewCoordinator.setAutoMinFps(com.rawr.camera.settings.model.autoMinFpsForId(it))
        }
        syncSetting({ it.persistentDiagnosticsEnabled }) { previewCoordinator.setPersistentDiagnosticsEnabled(it) }
        syncSetting({ it.persistZslRingEnabled }) { previewCoordinator.setPersistZslRingEnabled(it) }
        syncSetting({ it.internalTraceCaptureEnabled }) { previewCoordinator.setInternalTraceCaptureEnabled(it) }
        syncSetting({ it.photoLensShadingEnabled }) { previewCoordinator.setLensShadingCorrectionEnabled(it) }
        // Idle video uses RawPreview until recording supplies the monitor
        // image. Match its recovery switch to the recorder, including when
        // entering/leaving Video; photo recovery can alter LOG highlights.
        syncSetting({ if (it.isVideo) it.videoHighlightEnabled else it.photoHighlightEnabled }) {
            previewCoordinator.setHighlightReconstructionEnabled(it)
        }
        syncSetting({ it.toVideoImageSettings() }) {
            previewCoordinator.setVideoImageSettings(it)
            // FCC and defringe are part of the prewarmed pipeline.
            prewarmVideo()
        }
        syncSetting({ it.effectiveVideoBitDepth() }) { prewarmVideo() }
        syncSetting({ it.experimentalZeroCopyEnabled }) { previewCoordinator.setExperimentalZeroCopy(it) }
        syncSetting({ it.experimentalMultiframeEnabled }) { previewCoordinator.setExperimentalMultiframeEnabled(it) }
        syncSetting({
            it.experimentalMultiframeEnabled &&
                it.multiframeTuning.mergeAlgorithm == MultiframeMergeAlgorithm.HdrPlusBracketed
        }) { previewCoordinator.setHdrPlusBracketEnabled(it) }
        syncSetting({ it.persistentEngineEnabled }) { previewCoordinator.setPersistentEngineEnabled(it) }
        syncSetting({ it.internalTraceRetainedRows }) {
            com.rawr.camera.integration.InternalTraceNative.setRetainedRows(it)
        }
        // Keyed on profile identity + LUT structure (tone normalized out):
        // native profile switches rebuild the tonemap engine, so tone-only
        // edits must not retrigger them (tone flows via setTonemapParameters).
        syncSetting({ it.renderProfileSyncKey() }) { (profile, id, _) ->
            previewCoordinator.setColorRenderProfile(profile, id)
        }
        syncSetting({ it.filmSimEnabled }) { previewCoordinator.setFilmSim(it) }
        syncSetting({ it.viewfinderDivisor }) { previewCoordinator.setViewfinderDivisor(it) }
        syncSetting({ it.filmSimLook }) { look ->
            previewCoordinator.setFilmSimLook(look.toFloatArray(), look.toIntArray())
        }
        syncSetting({ it.peakingSensitivityId }) { id ->
            val sensitivity =
                when (id) {
                    "peaking.low" -> 0.25f
                    "peaking.high" -> 0.75f
                    else -> 0.50f
                }
            previewCoordinator.setFocusPeakingSensitivity(sensitivity)
        }
        syncStream(LiveTonemapState.values.filterNotNull()) { applyTonemapState(it) }
        firstApplies = bindings.size
        bindings.forEach { collect -> scope.launch { collect() } }
    }

    private fun <T> syncSetting(select: (SettingsValues) -> T, apply: suspend (T) -> Unit) {
        syncStream(values.map(select).distinctUntilChanged(), apply)
    }

    private fun <T> syncStream(source: Flow<T>, apply: suspend (T) -> Unit) {
        bindings += {
            var first = true
            source.collect { value ->
                apply(value)
                if (first) {
                    first = false
                    firstApplies--
                    if (firstApplies == 0) previewCoordinator.setInitialConfigReady()
                }
            }
        }
    }

    /**
     * Arms/clears the native idle-preview crop to the exact record window.
     * 16:9 outputs crop the open-gate preview to the centered record rect;
     * Open Gate and Photo show the full frame (clear). Output sizes are
     * sensor-landscape, matching the recorder's frame sizes.
     */
    fun updatePreviewCrop(mode: CaptureMode, resolution: VideoResolutionMode) {
        when {
            mode != CaptureMode.Video -> previewCoordinator.clearVideoPreviewCrop()
            resolution == VideoResolutionMode.HD1080 -> previewCoordinator.setVideoPreviewCropOut(1920, 1080)
            resolution == VideoResolutionMode.UHD4K -> previewCoordinator.setVideoPreviewCropOut(3840, 2160)
            else -> previewCoordinator.clearVideoPreviewCrop()
        }
        prewarmVideo(mode, resolution)
    }

    /** Builds (Video) or frees (Photo) the recording pipeline so recording starts at once. */
    fun prewarmVideo(
        mode: CaptureMode = controller.state.value.captureMode,
        resolution: VideoResolutionMode = controller.state.value.videoResolution
    ) {
        if (mode != CaptureMode.Video) {
            previewCoordinator.releaseVideoProcessing()
            return
        }
        val (width, height) = when (resolution) {
            VideoResolutionMode.HD1080 -> 1920 to 1080
            VideoResolutionMode.UHD4K -> 3840 to 2160
            VideoResolutionMode.OPEN_GATE -> 0 to 0
        }
        previewCoordinator.prewarmVideo(width, height, latestSettings().effectiveVideoBitDepth())
    }

    private fun applyTonemapState(tone: com.rawr.camera.model.ImageToneState) {
        runCatching {
            previewCoordinator.setTonemapParameters(tone)
        }.onFailure {
            // A native hiccup must never cancel the LiveTonemapState collector,
            // or settings -> capture tone sync would die for the rest of the session.
        }
        controller.dispatch(SyncToneControls.fromImageTone(tone))
    }

}
