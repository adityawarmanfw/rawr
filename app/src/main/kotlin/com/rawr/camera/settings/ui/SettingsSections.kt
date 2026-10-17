package com.rawr.camera.settings.ui

import androidx.compose.foundation.layout.*
import androidx.compose.runtime.Composable
import com.rawr.camera.settings.architecture.*
import com.rawr.camera.settings.model.*

@Composable
internal fun SettingsSectionContent(
    state: SettingsUiState,
    section: SettingsSection,
    dispatch: SettingsDispatch,
    onImportLut: (String?) -> Unit = {},
    onImportGpuDriver: () -> Unit = {},
    onPickSaveDirectory: () -> Unit = {},
    onDumpInternalTrace: () -> Unit = {},
    onClearInternalTrace: () -> Unit = {},
    onExportDiagnosticsBundle: () -> Unit = {},
    onRequestSaveNotificationPermission: () -> Unit = {}
) {
    when (section) {
        SettingsSection.Capture -> {
            CaptureSettings(state, dispatch)
        }

        SettingsSection.Lens -> {
            LensSettings(state, dispatch)
        }

        SettingsSection.Exposure -> {
            ExposureSettings(state, dispatch)
        }

        SettingsSection.HighlightReconstruction -> {
            HighlightReconstructionSettings(state, dispatch)
        }

        SettingsSection.Ois -> {
            OisSettings(state, dispatch)
        }

        SettingsSection.Image -> {
            ImageSettings(state, dispatch)
        }

        SettingsSection.ImageTone -> {
            ImageToneSettings(state, dispatch, onImportLut)
        }

        SettingsSection.LutProfile -> {
            LutProfileSettings(state, dispatch, onImportLut)
        }

        SettingsSection.Demosaic -> {
            DemosaicSettings(state, dispatch)
        }

        SettingsSection.Defringe -> {
            DefringeSettings(state, dispatch)
        }

        SettingsSection.Denoise -> {
            DenoiseSettings(state, dispatch)
        }

        SettingsSection.LensShading -> {
            LensShadingSettings(state, dispatch)
        }

        SettingsSection.Jpeg -> {
            JpegSettings(state, dispatch)
        }

        SettingsSection.Dng -> {
            DngSettings(state, dispatch)
        }

        SettingsSection.DisplayControls -> {
            DisplayControlsSettings(state, dispatch)
        }

        SettingsSection.Monitoring -> {
            MonitoringSettings(state, dispatch)
        }

        SettingsSection.ControlStyle -> {
            ControlStyleSettings(state, dispatch)
        }

        SettingsSection.Storage -> {
            StorageSettings(state, onPickSaveDirectory)
        }

        SettingsSection.VideoEncoder -> {
            VideoEncoderSettings(state, dispatch)
        }

        SettingsSection.Experimental -> {
            ExperimentalSettings(state, dispatch)
        }

        SettingsSection.GpuDriver -> {
            GpuDriverSettings(state, dispatch, onImportGpuDriver)
        }

        SettingsSection.ZeroCopy -> {
            ZeroCopySettings(state, dispatch)
        }

        SettingsSection.Multiframe -> {
            MultiframeSettings(state, dispatch)
        }

        SettingsSection.FilmSim -> {
            FilmSimSettings(state, dispatch)
        }

        SettingsSection.DynamicRange -> {
            DynamicRangeSettings(state, dispatch)
        }

        SettingsSection.Debug -> {
            DebugSettings(state, dispatch, onRequestSaveNotificationPermission)
        }

        SettingsSection.InternalLogging -> {
            InternalLoggingSettings(
                state,
                dispatch,
                onDumpInternalTrace,
                onClearInternalTrace,
                onExportDiagnosticsBundle
            )
        }

        SettingsSection.About -> {
            AboutSettings()
        }

        SettingsSection.CameraDevice -> {
            CaptureSettings(state, dispatch)
        }
    }
}
