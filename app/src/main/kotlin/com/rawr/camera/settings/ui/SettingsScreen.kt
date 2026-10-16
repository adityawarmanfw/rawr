package com.rawr.camera.settings.ui

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.layout.*
import com.rawr.camera.ui.icons.Icons
import com.rawr.camera.ui.icons.automirrored.rounded.ArrowBack
import com.rawr.camera.ui.icons.rounded.*
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.rawr.camera.settings.architecture.*
import com.rawr.camera.settings.model.*

@Composable
fun SettingsRoute(
    controller: SettingsController,
    onExitSettings: () -> Unit,
    onImportLut: (String?) -> Unit = {},
    onImportGpuDriver: () -> Unit = {},
    onPickSaveDirectory: () -> Unit = {},
    onDumpInternalTrace: () -> Unit = {},
    onClearInternalTrace: () -> Unit = {},
    onExportDiagnosticsBundle: () -> Unit = {},
    onExportBackup: () -> Unit = {},
    onImportBackup: () -> Unit = {},
    onRequestSaveNotificationPermission: () -> Unit = {},
    lensHardware: LensHardware = LensHardware.None
) {
    val state by controller.state.collectAsStateWithLifecycle()
    val haptics = rememberSettingsHaptics()
    CompositionLocalProvider(LocalSettingsHaptics provides haptics, LocalLensHardware provides lensHardware) {
        SettingsScreen(
            state = state,
            dispatch = controller::dispatch,
            onExitSettings = onExitSettings,
            onImportLut = onImportLut,
            onImportGpuDriver = onImportGpuDriver,
            onPickSaveDirectory = onPickSaveDirectory,
            onDumpInternalTrace = onDumpInternalTrace,
            onClearInternalTrace = onClearInternalTrace,
            onExportDiagnosticsBundle = onExportDiagnosticsBundle,
            onExportBackup = onExportBackup,
            onImportBackup = onImportBackup,
            onRequestSaveNotificationPermission = onRequestSaveNotificationPermission
        )
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SettingsScreen(
    state: SettingsUiState,
    dispatch: SettingsDispatch,
    modifier: Modifier = Modifier,
    handleBack: Boolean = true,
    embedded: Boolean = false,
    onExitSettings: () -> Unit = {},
    onImportLut: (String?) -> Unit = {},
    onImportGpuDriver: () -> Unit = {},
    onPickSaveDirectory: () -> Unit = {},
    onDumpInternalTrace: () -> Unit = {},
    onClearInternalTrace: () -> Unit = {},
    onExportDiagnosticsBundle: () -> Unit = {},
    onExportBackup: () -> Unit = {},
    onImportBackup: () -> Unit = {},
    onRequestSaveNotificationPermission: () -> Unit = {}
) {
    val navigateUp = {
        if (state.presentation.destination == SettingsDestination.Home) {
            onExitSettings()
        } else {
            dispatch.invoke(NavigateBack)
        }
    }

    // One hierarchy for toolbar Up and Android system/predictive Back.
    BackHandler(enabled = handleBack && state.presentation.pendingReset == null, onBack = navigateUp)

    Scaffold(
        modifier = modifier.fillMaxSize(),
        containerColor = MaterialTheme.colorScheme.background,
        contentWindowInsets = if (embedded) WindowInsets(0) else ScaffoldDefaults.contentWindowInsets,
        topBar = {
            TopAppBar(
                title = { Text(destinationTitle(state.presentation.destination)) },
                windowInsets = if (embedded) WindowInsets(0) else TopAppBarDefaults.windowInsets,
                navigationIcon = {
                    IconButton(
                        onClick = navigateUp,
                        modifier = Modifier.testTag(SettingsTestTags.TOPBAR_BACK)
                    ) {
                        Icon(Icons.AutoMirrored.Rounded.ArrowBack, contentDescription = "Back")
                    }
                },
                colors =
                    TopAppBarDefaults.topAppBarColors(
                        containerColor = MaterialTheme.colorScheme.background,
                        scrolledContainerColor = MaterialTheme.colorScheme.surface
                    )
            )
        }
    ) { padding ->
        // Each destination keeps its own scroll offset (in-memory accessory
        // state), so Back returns to the previous scroll position.
        val scrollMemory = androidx.compose.runtime.remember { SettingsScrollMemory() }
        Box(
            Modifier.fillMaxSize().padding(padding).testTag(settingsDestinationKey(state.presentation.destination)),
            contentAlignment = Alignment.TopCenter
        ) {
            CompositionLocalProvider(
                LocalSettingsScrollSession provides
                    SettingsScrollSession(settingsDestinationKey(state.presentation.destination), scrollMemory)
            ) {
            when (val destination = state.presentation.destination) {
                SettingsDestination.Home -> {
                    SettingsHome(dispatch)
                }

                is SettingsDestination.Section -> {
                    SettingsSectionContent(
                        state,
                        destination.section,
                        dispatch,
                        onImportLut,
                        onImportGpuDriver,
                        onPickSaveDirectory,
                        onDumpInternalTrace,
                        onClearInternalTrace,
                        onExportDiagnosticsBundle,
                        onExportBackup,
                        onImportBackup,
                        onRequestSaveNotificationPermission
                    )
                }

                is SettingsDestination.ChoiceSelector -> {
                    ChoiceSelectorScreen(state, destination.kind, dispatch)
                }

                is SettingsDestination.FilmSimSubPage -> {
                    FilmSimSubPageScreen(state, destination.section, dispatch)
                }

                is SettingsDestination.FilmSimDetailPage -> {
                    FilmSimDetailScreen(state, destination.detail, dispatch)
                }

                is SettingsDestination.LensEditor -> {
                    LensEditorScreen(state, destination.lensName, dispatch)
                }
            }
            }
        }
    }

    state.presentation.pendingReset?.let { target -> ResetConfirmation(target, dispatch) }
}

private fun settingsDestinationKey(destination: SettingsDestination): String = when (destination) {
    SettingsDestination.Home -> "home"
    is SettingsDestination.Section -> "section:${destination.section.name}"
    is SettingsDestination.ChoiceSelector -> "selector:${destination.kind.name}"
    is SettingsDestination.FilmSimSubPage -> "filmsim:${destination.section.name}"
    is SettingsDestination.FilmSimDetailPage -> "filmdetail:${destination.detail.name}"
    is SettingsDestination.LensEditor -> "lens:${destination.lensName.orEmpty()}"
}

private fun destinationTitle(destination: SettingsDestination): String = when (destination) {
    SettingsDestination.Home -> "Settings"
    is SettingsDestination.Section -> destination.section.title
    is SettingsDestination.ChoiceSelector -> selectorTitle(destination.kind)
    is SettingsDestination.FilmSimSubPage -> destination.section.title
    is SettingsDestination.FilmSimDetailPage -> destination.detail.title
    is SettingsDestination.LensEditor -> destination.lensName?.let { "Lens $it" } ?: "New lens"
}

@Composable
private fun SettingsHome(dispatch: SettingsDispatch) {
    val entries =
        listOf(
            SettingsHomeEntry(SettingsSection.Image, Icons.Rounded.Tune),
            SettingsHomeEntry(SettingsSection.Capture, Icons.Rounded.CameraAlt),
            SettingsHomeEntry(SettingsSection.DisplayControls, Icons.Rounded.MonitorHeart),
            SettingsHomeEntry(SettingsSection.Dng, Icons.Rounded.Description),
            SettingsHomeEntry(SettingsSection.Jpeg, Icons.Rounded.Image),
            SettingsHomeEntry(SettingsSection.VideoEncoder, Icons.Rounded.Videocam),
            SettingsHomeEntry(SettingsSection.Storage, Icons.Rounded.Folder),
            SettingsHomeEntry(SettingsSection.Experimental, Icons.Rounded.Science),
            SettingsHomeEntry(SettingsSection.Debug, Icons.Rounded.BugReport),
            SettingsHomeEntry(SettingsSection.About, Icons.Rounded.Info)
        )
    SettingsPageContainer(testTag = SettingsTestTags.SCREEN_HOME) {
        SettingsGroup {
            entries.forEachIndexed { index, entry ->
                SettingsRow(
                    title = entry.section.title,
                    leadingIcon = entry.icon,
                    testTag = SettingsTestTags.homeEntry(entry.section.name),
                    onClick = { dispatch.invoke(OpenSection(entry.section)) }
                )
                if (index != entries.lastIndex) SettingDivider()
            }
        }
    }
}

private data class SettingsHomeEntry(val section: SettingsSection, val icon: ImageVector)

@Composable
private fun ResetConfirmation(target: ResetTarget, dispatch: SettingsDispatch) {
    AlertDialog(
        onDismissRequest = { dispatch.invoke(DismissReset) },
        title = { Text(resetTitle(target)) },
        text = { Text("Reset to defaults?") },
        confirmButton = { TextButton(onClick = { dispatch.invoke(ConfirmReset(target)) }) { Text("Reset") } },
        dismissButton = { TextButton(onClick = { dispatch.invoke(DismissReset) }) { Text("Cancel") } }
    )
}

private fun resetTitle(target: ResetTarget): String = when (target) {
    ResetTarget.ExposureTonality -> "Reset Exposure & Tonality?"
    ResetTarget.Color -> "Reset Color?"
    ResetTarget.Output -> "Reset Output?"
    ResetTarget.AllImageTone -> "Reset all Image / Tone?"
}
