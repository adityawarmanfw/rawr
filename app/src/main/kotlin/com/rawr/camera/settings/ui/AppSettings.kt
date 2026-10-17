package com.rawr.camera.settings.ui

import androidx.compose.foundation.layout.*
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.core.net.toUri
import com.rawr.camera.BuildConfig
import com.rawr.camera.settings.architecture.*
import com.rawr.camera.settings.model.*

@Composable
internal fun AboutSettings(onExportBackup: () -> Unit = {}, onImportBackup: () -> Unit = {}) {
    var showLicenses by remember { mutableStateOf(false) }
    SettingsPageContainer(testTag = SettingsTestTags.sectionRoot("About")) {
        SettingsGroup {
            SettingsRow(
                "Rawr",
                "Version ${BuildConfig.VERSION_NAME}",
                testTag = SettingsTestTags.row("About", "app_version")
            )
            SettingDivider()
            SettingsRow(
                "Renderer",
                "RAWR NTRL + user-imported LUT profiles",
                testTag = SettingsTestTags.row("About", "renderer")
            )
            SettingDivider()
            SettingsRow(
                "License",
                "GNU GPL v3.0 only",
                testTag = SettingsTestTags.row("About", "license")
            )
            SettingDivider()
            SettingsRow(
                "Open-source licenses",
                "Third-party notices and full license texts",
                testTag = SettingsTestTags.row("About", "licenses"),
                onClick = { showLicenses = true }
            )
        }
        SettingsGroup(
            title = "Backup",
            description = "Settings, lens profiles and, if chosen, LUT profiles as a zip. Save location and GPU driver stay on this phone."
        ) {
            SettingsRow(
                "Export Backup",
                "Zip",
                testTag = SettingsTestTags.row("About", "export_backup"),
                onClick = onExportBackup
            )
            SettingDivider()
            SettingsRow(
                "Import Backup",
                testTag = SettingsTestTags.row("About", "import_backup"),
                onClick = onImportBackup
            )
        }
    }
    if (showLicenses) {
        LicensesDialog(onDismiss = { showLicenses = false })
    }
}

@Composable
internal fun StorageSettings(state: SettingsUiState, onPickSaveDirectory: () -> Unit) {
    val id = state.values.saveLocationId
    val saveLocation =
        if (id.startsWith("storage.tree:")) {
            val uri = id.removePrefix("storage.tree:").toUri()
            android.net.Uri
                .decode(uri.lastPathSegment ?: "Selected folder")
                .substringAfterLast(':')
                .ifBlank { "Selected folder" }
        } else {
            state.capabilities.saveLocationChoices
                .firstOrNull { it.id == id }
                ?.label ?: "Unavailable"
        }
    SettingsPageContainer(testTag = SettingsTestTags.sectionRoot("Storage")) {
        SettingsGroup(
            description = "Photos and videos use this location. Choose any folder exposed by Android's system directory picker. Rawr keeps access after reboot."
        ) {
            SettingsRow(
                "Save Location",
                saveLocation,
                testTag = SettingsTestTags.row("Storage", "save_location"),
                onClick = onPickSaveDirectory
            )
        }
    }
}
