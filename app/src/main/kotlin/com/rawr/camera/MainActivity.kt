package com.rawr.camera

import android.Manifest
import android.content.ActivityNotFoundException
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Bundle
import android.view.WindowManager
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.compose.runtime.getValue
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.lifecycleScope
import com.rawr.camera.architecture.TriggerCapture
import com.rawr.camera.integration.NativeCameraUiSnapshot
import com.rawr.camera.settings.SettingsActivity
import com.rawr.camera.ui.CaptureRoute
import com.rawr.camera.ui.CaptureTheme
import com.rawr.camera.ui.VideoControlState
import com.rawr.camera.video.AvSyncProbe
import com.rawr.camera.video.P010WriterProbe
import com.rawr.camera.video.VideoCapabilityProbe
import com.rawr.camera.video.VideoEncoderProbe
import com.rawr.camera.video.VideoResolutionMode
import com.rawr.camera.video.VideoVulkanProbe
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withContext

class MainActivity : ComponentActivity() {
    private val captureViewModel: CaptureViewModel by viewModels()
    private var started = false
    private var startGeneration = 0L
    private var diagnosticCapturePending = false
    private var highlightReplayPending = false
    private var highlightReplayInputPath: String? = null
    private var diagnosticTargetCameraId: String? = null
    private var autoVideoSeconds = 0

    private val microphonePermissionLauncher =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
            if (granted && started) toggleVideoRecording()
        }

    private val permissionLauncher =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
            if (granted && started) startPreviewAndMaybeDiagnosticCapture()
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // Dev/test hooks: win over persisted settings via the startup override
        // (applied when the settings stream emits, so there is no race).
        captureViewModel.applyStartupVideoOverrides(
            resolution = if (intent.hasExtra("raw_video_mode")) {
                VideoResolutionMode.fromName(intent.getStringExtra("raw_video_mode"))
            } else {
                null
            },
            fps = if (intent.hasExtra("raw_video_fps")) {
                intent.getIntExtra("raw_video_fps", 30).takeIf { it == 24 || it == 30 }
            } else {
                null
            }
        )
        enableEdgeToEdge()
        if (intent.getBooleanExtra("video_capability_probe", false)) {
            lifecycleScope.launch {
                val report = withContext(Dispatchers.IO) { VideoCapabilityProbe.report().toString(2) }
                withContext(Dispatchers.IO) {
                    java.io.File(filesDir, "video_capabilities.json").writeText(report)
                }
                android.util.Log.i("RawrVideoProbe", "Wrote video_capabilities.json")
            }
        }
        if (intent.getBooleanExtra("video_encoder_probe", false)) {
            val report = runBlocking(Dispatchers.IO) { VideoEncoderProbe.run(filesDir).toString(2) }
            java.io.File(filesDir, "video_encoder_probe.json").writeText(report)
            android.util.Log.i("RawrVideoProbe", "Wrote video_encoder_probe.json")
        }
        if (intent.getBooleanExtra("p010_writer_probe", false)) {
            // The probe creates its own VulkanContext; finish it before the
            // preview starts, because Rawr's Vulkan loader dispatch is shared.
            val report = runBlocking(Dispatchers.IO) { P010WriterProbe.run().toString(2) }
            java.io.File(filesDir, "p010_writer_probe.json").writeText(report)
            android.util.Log.i("RawrVideoProbe", "Wrote p010_writer_probe.json")
        }
        if (intent.getBooleanExtra("video_vulkan_probe", false)) {
            val report = runBlocking(Dispatchers.IO) { VideoVulkanProbe.run(filesDir).toString(2) }
            java.io.File(filesDir, "video_vulkan_probe.json").writeText(report)
            android.util.Log.i("RawrVideoProbe", "Wrote video_vulkan_probe.json")
        }
        if (intent.getBooleanExtra("av_sync_probe", false)) {
            val report = runBlocking(Dispatchers.IO) { AvSyncProbe.run(filesDir).toString(2) }
            java.io.File(filesDir, "av_sync_probe.json").writeText(report)
            android.util.Log.i("RawrVideoProbe", "Wrote av_sync_probe.json")
        }
        intent.getStringExtra(EXTRA_CAMERA_ID)?.takeIf { id -> id.toIntOrNull()?.let { it in 0..11 } == true }?.let {
            diagnosticTargetCameraId = it
            captureViewModel.selectCameraIdFromIntent(it)
        }
        autoVideoSeconds = intent.getIntExtra("raw_video_seconds", 0).coerceIn(0, 600)
        val encoderOverrideNames = listOf(
            "raw_video_bitrate", "raw_video_intra_seconds", "raw_video_bitrate_mode",
            "raw_video_max_b_frames", "raw_video_audio_channels", "raw_video_audio_bitrate", "raw_video_bit_depth"
        )
        captureViewModel.recordingOptions = com.rawr.camera.video.RecordingOptions(
            shutterNs = if (BuildConfig.DEBUG && intent.hasExtra("raw_video_test_shutter_ns"))
                intent.getLongExtra("raw_video_test_shutter_ns", 33_333_333L) else null,
            iso = if (BuildConfig.DEBUG && intent.hasExtra("raw_video_test_iso"))
                intent.getIntExtra("raw_video_test_iso", 1600) else null,
            encoderOverrides = if (BuildConfig.DEBUG) encoderOverrideNames.filter(intent::hasExtra)
                .associateWith { intent.getIntExtra(it, 0) } else emptyMap(),
            inspectOutput = autoVideoSeconds > 0
        )
        captureViewModel.previewCoordinator.setPreferredAccessRoute(intent.getStringExtra(EXTRA_CAMERA_ROUTE))
        captureViewModel.previewCoordinator.setPreferredColorMode(intent.getStringExtra(EXTRA_COLOR_MODE))
        val diagnosticMode =
            when (intent.getStringExtra(EXTRA_DIAGNOSTIC_MODE)?.lowercase()) {
                null, "", "camera" -> 0
                "checker", "screen_checker" -> 1
                "tone_pattern" -> 2
                "linear_pattern" -> 3
                "linear_actual" -> 4
                "uv", "uv_transform" -> 5
                "raw_copy", "copy_raw" -> 6
                "raw_storage_viz", "raw_viz_storage" -> 7
                "raw_sampled_viz", "raw_viz_sampled" -> 8
                "raw_transfer_copy", "transfer_raw" -> 9
                "camera_direct", "direct_storage" -> 10
                "zero_copy_ab", "raw_zero_copy_ab", "zero_copy_mixed_cpu" -> 11
                "zero_copy_mixed_gpu" -> 12
                "zero_copy_compute_cpu", "zero_copy_storage_cpu" -> 13
                "zero_copy_compute_gpu", "zero_copy_storage_gpu" -> 14
                "zero_copy_final", "zero_copy_advanced" -> 15
                "zero_copy_buffer", "raw_zero_copy_buffer" -> 16
                else -> 0
            }
        captureViewModel.previewCoordinator.setPipelineDiagnostic(diagnosticMode)
        intent.getIntExtra(EXTRA_CPU_RAW_COPY_PROBE_FRAMES, 0).takeIf { it > 0 }?.let {
            captureViewModel.previewCoordinator.setCpuRawCopyProbeFrames(it.coerceAtMost(200))
        }
        diagnosticCapturePending = intent.getBooleanExtra(EXTRA_RAW_STILL_CAPTURE_ONCE, false)
        val replayRequestFile = java.io.File(filesDir, "highlight_replay_request.txt")
        val replayRequestPath =
            runCatching {
                replayRequestFile
                    .takeIf { it.isFile }
                    ?.readText()
                    ?.trim()
                    ?.takeIf { it.isNotEmpty() }
            }.getOrNull()
        highlightReplayPending = intent.getBooleanExtra(EXTRA_HIGHLIGHT_REPLAY_ONCE, false) || replayRequestPath != null
        highlightReplayInputPath = intent.getStringExtra(EXTRA_HIGHLIGHT_REPLAY_INPUT)
            ?: replayRequestPath
            ?: java.io.File(filesDir, "highlight_replay_input.rgba16f").absolutePath
        if (highlightReplayPending) {
            val source = if (intent.getBooleanExtra(EXTRA_HIGHLIGHT_REPLAY_ONCE, false)) "intent" else "request_file"
            android.util.Log.i("RawrCamReplay", "HIGHLIGHT_REPLAY_STAGE intent_received source=$source")
        }
        setContent {
            val renderProfiles by captureViewModel.renderProfiles.collectAsStateWithLifecycle()
            val filmSimQuick by captureViewModel.filmSimQuick.collectAsStateWithLifecycle()
            val latestGalleryImage by captureViewModel.latestGalleryImage.collectAsStateWithLifecycle()
            val videoSelection = androidx.compose.runtime.remember(captureViewModel) {
                captureViewModel.controller.state.map { it.videoResolution to it.videoFps }.distinctUntilChanged()
            }
            val initialVideoSelection = androidx.compose.runtime.remember(captureViewModel) {
                captureViewModel.controller.state.value.let { it.videoResolution to it.videoFps }
            }
            val selectedVideo by videoSelection.collectAsStateWithLifecycle(initialVideoSelection)
            val recording by captureViewModel.recording.state.collectAsStateWithLifecycle()
            androidx.compose.runtime.LaunchedEffect(Unit) {
                captureViewModel.recording.errors.collect { message ->
                    Toast.makeText(this@MainActivity, message, Toast.LENGTH_LONG).show()
                }
            }
            CaptureTheme {
                CaptureRoute(
                    controller = captureViewModel.controller,
                    previewCoordinator = captureViewModel.previewCoordinator,
                    renderProfiles = renderProfiles,
                    onSelectRenderProfile = captureViewModel::selectRenderProfileFromCapture,
                    onOpenSettings = { startActivity(Intent(this@MainActivity, SettingsActivity::class.java)) },
                    onOpenFilmSimSettings = {
                        startActivity(
                            SettingsActivity.openIntent(
                                this@MainActivity,
                                com.rawr.camera.settings.model.SettingsSection.FilmSim
                            )
                        )
                    },
                    onOpenMultiframeSettings = {
                        startActivity(
                            SettingsActivity.openIntent(
                                this@MainActivity,
                                com.rawr.camera.settings.model.SettingsSection.Multiframe
                            )
                        )
                    },
                    latestImageUri = latestGalleryImage,
                    onOpenLatestImage = { latestGalleryImage?.let(::openGalleryImage) },
                    onOpenRenderer = { startActivity(Intent(this@MainActivity, com.rawr.camera.renderer.RendererActivity::class.java)) },
                    filmQuick = filmSimQuick,
                    onFilmEvent = captureViewModel::onFilmEvent,
                    videoControls = VideoControlState(
                        resolution = selectedVideo.first.label,
                        fps = selectedVideo.second,
                        recording = recording.recording,
                        busy = recording.busy,
                        status = recording.status,
                        timing = recording.timing
                    ),
                    onToggleVideoRecording = ::toggleVideoRecording
                )
            }
        }
    }

    override fun onStart() {
        super.onStart()
        started = true
        // A viewfinder you are only looking at must not time out; onStop clears this.
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        val generation = ++startGeneration
        lifecycleScope.launch {
            val renderer = com.rawr.camera.renderer.RendererStore.get(this@MainActivity)
            renderer.awaitReady()
            if (!started || generation != startGeneration) return@launch
            if (renderer.busy.value || renderer.hasInterruptedExport()) {
                startActivity(Intent(this@MainActivity, com.rawr.camera.renderer.RendererActivity::class.java))
                return@launch
            }
            captureViewModel.refreshLatestGalleryImage()
            if (checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) {
                startPreviewAndMaybeDiagnosticCapture()
            } else {
                permissionLauncher.launch(Manifest.permission.CAMERA)
            }
        }
    }

    private fun startPreviewAndMaybeDiagnosticCapture() {
        captureViewModel.startPreview()
        if (autoVideoSeconds > 0) {
            val seconds = autoVideoSeconds
            autoVideoSeconds = 0
            lifecycleScope.launch {
                var readyGeneration = 0L
                var readySamples = 0
                repeat(150) {
                    val snapshot = NativeCameraUiSnapshot.parse(
                        captureViewModel.previewCoordinator.cameraControlSnapshot())
                    if (snapshot != null && snapshot.appliedRawFps != null &&
                        snapshot.measuredViewfinderFps != null) {
                        readySamples = if (snapshot.generation == readyGeneration) readySamples + 1 else 1
                        readyGeneration = snapshot.generation
                    } else {
                        readySamples = 0
                    }
                    if (readySamples >= 3) {
                        captureViewModel.recording.start()?.join()
                        if (captureViewModel.recording.state.value.recording) {
                            delay(seconds * 1_000L)
                            captureViewModel.recording.stop()
                        }
                        return@launch
                    }
                    delay(100L)
                }
                android.util.Log.e("RawrVideo", "RAW stream not ready for auto recording")
            }
        }
        if (diagnosticCapturePending) {
            diagnosticCapturePending = false
            window.decorView.postDelayed({ captureViewModel.controller.dispatch(TriggerCapture) }, 750L)
        }
        if (highlightReplayPending) {
            highlightReplayPending = false
            lifecycleScope.launch {
                val input = highlightReplayInputPath
                if (input.isNullOrBlank()) return@launch
                // Consume the durable replay request only after the app has actually
                // entered replay handling. This makes cold-start delivery independent
                // of vendor intent-extra quirks while avoiding accidental replay on
                // ordinary subsequent launches.
                runCatching { java.io.File(filesDir, "highlight_replay_request.txt").delete() }
                repeat(200) {
                    val snapshot =
                        NativeCameraUiSnapshot.parse(
                            captureViewModel.previewCoordinator.cameraControlSnapshot()
                        )
                    if (snapshot != null &&
                        (diagnosticTargetCameraId == null || snapshot.cameraId == diagnosticTargetCameraId)
                    ) {
                        val result =
                            kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.IO) {
                                captureViewModel.previewCoordinator.runHighlightReplay(input)
                            }
                        android.util.Log.i("RawrCamReplay", result)
                        return@launch
                    }
                    delay(100L)
                }
                android.util.Log.e("RawrCamReplay", "HIGHLIGHT_REPLAY_FAIL camera_not_ready")
            }
        }
    }

    private fun openGalleryImage(uri: Uri) {
        val intent =
            Intent(Intent.ACTION_VIEW).apply {
                setDataAndType(uri, "image/jpeg")
                addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            }
        try {
            startActivity(intent)
        } catch (_: ActivityNotFoundException) {
            // No compatible gallery/photo viewer is installed; keep the camera usable.
        }
    }

    override fun onStop() {
        started = false
        startGeneration++
        captureViewModel.stopPreviewAfterRecording()
        window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        super.onStop()
    }

    private fun toggleVideoRecording() {
        val state = captureViewModel.recording.state.value
        if (state.recording || captureViewModel.controller.state.value.selfTimerRemainingMs != null ||
            checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED
        ) {
            captureViewModel.recording.toggle()
        } else {
            microphonePermissionLauncher.launch(Manifest.permission.RECORD_AUDIO)
        }
    }

    private companion object {
        const val EXTRA_CAMERA_ID = "camera_id"
        const val EXTRA_CAMERA_ROUTE = "camera_route"
        const val EXTRA_DIAGNOSTIC_MODE = "diagnostic_mode"
        const val EXTRA_CPU_RAW_COPY_PROBE_FRAMES = "cpu_raw_copy_probe_frames"
        const val EXTRA_RAW_STILL_CAPTURE_ONCE = "raw_still_capture_once"
        const val EXTRA_COLOR_MODE = "color_mode"
        const val EXTRA_HIGHLIGHT_REPLAY_ONCE = "highlight_replay_once"
        const val EXTRA_HIGHLIGHT_REPLAY_INPUT = "highlight_replay_input"
    }
}
