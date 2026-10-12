package com.rawr.camera.settings

import android.Manifest
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Bundle
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.core.content.ContextCompat
import androidx.core.net.toUri
import com.rawr.camera.MainActivity
import com.rawr.camera.integration.InternalTraceNative
import com.rawr.camera.settings.architecture.ImportLutStage
import com.rawr.camera.settings.architecture.OpenSection
import com.rawr.camera.settings.architecture.SetChoice
import com.rawr.camera.settings.architecture.SetCustomGpuDriverInstalled
import com.rawr.camera.settings.architecture.SetSaveLocationTree
import com.rawr.camera.settings.model.ChoiceSelectorKind
import com.rawr.camera.settings.model.SettingsSection
import com.rawr.camera.settings.preferences.GpuDriverFileStore
import com.rawr.camera.settings.preferences.LutProfileFileStore
import com.rawr.camera.settings.ui.AndroidLensHardware
import com.rawr.camera.settings.ui.SettingsRoute
import com.rawr.camera.storage.PickFile
import com.rawr.camera.storage.launchPicker
import com.rawr.camera.ui.CaptureTheme
import java.io.File
import java.time.LocalDateTime
import java.time.format.DateTimeFormatter

class SettingsActivity : ComponentActivity() {
    private val viewModel: SettingsViewModel by viewModels()
    private var pendingLutTargetProfileId: String? = null
    private val notificationPermissionLauncher =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
            if (!granted) {
                Toast
                    .makeText(
                        this,
                        "Allow notifications to see ZSL background-save completion",
                        Toast.LENGTH_LONG
                    ).show()
            }
        }
    private val saveDirectoryPicker =
        registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
            if (uri != null) {
                val flags = Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                runCatching { contentResolver.takePersistableUriPermission(uri, flags) }
                    .onSuccess { viewModel.controller.dispatch(SetSaveLocationTree(uri.toString())) }
                    .onFailure {
                        Toast
                            .makeText(
                                this,
                                it.message ?: "Unable to use selected folder",
                                Toast.LENGTH_LONG
                            ).show()
                    }
            }
        }
    private val gpuDriverPicker =
        registerForActivityResult(PickFile()) { uri ->
            if (uri != null) {
                runCatching { GpuDriverFileStore(this).importZip(uri) }
                    .onSuccess { viewModel.controller.dispatch(SetCustomGpuDriverInstalled(it.displayName)) }
                    .onFailure {
                        Toast
                            .makeText(
                                this,
                                it.message ?: "Unable to import GPU driver",
                                Toast.LENGTH_LONG
                            ).show()
                    }
            }
        }
    private val traceDumpLauncher =
        registerForActivityResult(
            ActivityResultContracts.CreateDocument("text/plain")
        ) { uri ->
            if (uri != null) {
                val source = File(filesDir, INTERNAL_TRACE_FILE)
                runCatching {
                    require(source.isFile && source.length() > 0L) { "No internal trace has been captured yet" }
                    contentResolver.openOutputStream(uri, "wt")!!.use { output ->
                        source.inputStream().use { input -> input.copyTo(output) }
                    }
                }.onSuccess {
                    Toast.makeText(this, "Internal trace exported", Toast.LENGTH_SHORT).show()
                }.onFailure {
                    Toast.makeText(this, it.message ?: "Unable to export trace", Toast.LENGTH_LONG).show()
                }
            }
        }

    private val lutPicker =
        registerForActivityResult(PickFile()) { uri ->
            if (uri != null) {
                runCatching { LutProfileFileStore(this).importCube(uri) }
                    .onSuccess { viewModel.controller.dispatch(ImportLutStage(pendingLutTargetProfileId, it)) }
                    .onFailure { Toast.makeText(this, it.message ?: "Unable to import LUT", Toast.LENGTH_LONG).show() }
            }
            pendingLutTargetProfileId = null
        }

    private val lensHardware by lazy { AndroidLensHardware(this) }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        if (savedInstanceState == null) {
            runCatching { SettingsSection.valueOf(intent.getStringExtra(EXTRA_SECTION) ?: "") }
                .getOrNull()
                ?.let { viewModel.controller.dispatch(OpenSection(it)) }
        }
        setContent {
            CaptureTheme {
                SettingsRoute(
                    viewModel.controller,
                    onExitSettings = ::returnToCapture,
                    onImportLut = ::pickLut,
                    onImportGpuDriver = ::pickGpuDriver,
                    onPickSaveDirectory = ::pickSaveDirectory,
                    onDumpInternalTrace = ::dumpInternalTrace,
                    onClearInternalTrace = ::clearInternalTrace,
                    onExportDiagnosticsBundle = ::exportDiagnosticsBundle,
                    onRequestSaveNotificationPermission = ::requestSaveNotificationPermission,
                    lensHardware = lensHardware
                )
            }
        }
    }

    override fun onResume() {
        super.onResume()
        validateSaveLocationGrant()
    }

    /**
     * The system can revoke a persisted tree grant out from under us (Vivo
     * cleaner, OS update, reinstall). Detect that and fall back to the
     * MediaStore default so captures keep working; the user can re-pick.
     */
    private fun validateSaveLocationGrant() {
        val id = viewModel.controller.state.value.values.saveLocationId
        if (!id.startsWith("storage.tree:")) return
        val treeUri =
            runCatching {
                id.removePrefix("storage.tree:").toUri()
            }.getOrNull() ?: return
        val granted =
            contentResolver.persistedUriPermissions.any {
                it.uri == treeUri && it.isWritePermission
            }
        if (!granted) {
            viewModel.controller.dispatch(SetChoice(ChoiceSelectorKind.SaveLocation, "storage.dcim_camera"))
            Toast
                .makeText(
                    this,
                    "Save folder access was revoked, reset to DCIM/Camera. Tap Save Location to choose again.",
                    Toast.LENGTH_LONG
                ).show()
        }
    }

    private fun requestSaveNotificationPermission() {
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS) ==
            PackageManager.PERMISSION_GRANTED
        ) {
            return
        }
        notificationPermissionLauncher.launch(Manifest.permission.POST_NOTIFICATIONS)
    }

    private fun clearInternalTrace() {
        InternalTraceNative.clear()
        File(filesDir, INTERNAL_TRACE_FILE).delete()
        Toast.makeText(this, "Internal log cleared", Toast.LENGTH_SHORT).show()
    }

    private fun exportDiagnosticsBundle() {
        val stamp = LocalDateTime.now().format(DateTimeFormatter.ofPattern("yyyyMMdd_HHmmss"))
        val saveLocationId = viewModel.controller.state.value.values.saveLocationId
        Thread({
            val name =
                com.rawr.camera.storage
                    .ArtifactDiagnosticExporter(this)
                    .exportDiagnosticsBundle(saveLocationId, stamp)
            runOnUiThread {
                Toast
                    .makeText(
                        this,
                        if (name != null) "Diagnostics bundle saved: $name" else "No diagnostics files to export",
                        Toast.LENGTH_LONG
                    ).show()
            }
        }, "rawr-diagnostics-export").start()
    }

    private fun dumpInternalTrace() {
        val source = File(filesDir, INTERNAL_TRACE_FILE)
        if (!source.isFile || source.length() == 0L) {
            Toast.makeText(this, "No internal trace captured yet", Toast.LENGTH_SHORT).show()
            return
        }
        val stamp = LocalDateTime.now().format(DateTimeFormatter.ofPattern("yyyyMMdd_HHmmss"))
        traceDumpLauncher.launch("RAWR_internal_trace_$stamp.txt")
    }

    private fun pickSaveDirectory() {
        saveDirectoryPicker.launch(null)
    }

    private fun pickGpuDriver() {
        gpuDriverPicker.launchPicker(this, arrayOf("application/zip", "application/octet-stream", "*/*"))
    }

    private fun pickLut(targetProfileId: String?) {
        pendingLutTargetProfileId = targetProfileId
        lutPicker.launchPicker(this, arrayOf("application/octet-stream", "text/plain", "*/*"))
    }

    private fun returnToCapture() {
        startActivity(
            Intent(this, MainActivity::class.java).apply {
                addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_SINGLE_TOP)
            }
        )
        finish()
    }

    companion object {
        const val INTERNAL_TRACE_FILE = "rawrcam_internal_runtime_trace.txt"
        const val EXTRA_SECTION = "settings_section"

        fun openIntent(context: Context, section: SettingsSection? = null): Intent =
            Intent(context, SettingsActivity::class.java).apply {
                if (section != null) putExtra(EXTRA_SECTION, section.name)
            }
    }
}
