package com.rawr.camera.settings.ui

import androidx.compose.foundation.layout.*
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import com.rawr.camera.settings.architecture.*
import com.rawr.camera.settings.model.*

@Composable
internal fun FilmSimSettings(state: SettingsUiState, dispatch: SettingsDispatch) {
    val look = state.values.filmSimLook
    SettingsPageContainer {
        SettingsGroup(
            description = "Analog film simulation. Renders the preview through a film negative and print-paper model instead of the standard tonemap."
        ) {
            SettingsSwitchRow(
                title = "Film Simulation",
                checked = state.values.filmSimEnabled
            ) { dispatch.invoke(SetFilmSimEnabled(it)) }
        }
        // Preset first: pick a starting point, then refine below. Edits
        // mark the origin preset Modified (Revert restores it); null is Custom.
        SettingsGroup(title = "Preset") {
            val base = state.values.selectedFilmPreset()
            val modified = state.values.isFilmPresetModified()
            val statusValue = when {
                base == null -> "Custom look"
                modified -> "${base.name} • Modified"
                else -> base.name
            }
            SettingsRow(
                title = "Current preset",
                value = statusValue,
                onClick = null,
                trailing = if (modified) {
                    {
                        androidx.compose.material3.TextButton(onClick = { dispatch.invoke(RevertFilmPreset) }) {
                            androidx.compose.material3.Text("Revert")
                        }
                    }
                } else {
                    null
                }
            )
            SettingDivider()
            FilmSimPresetChips(
                factory = FilmFactoryPresets.all,
                user = state.values.filmPresets,
                selectedId = state.values.selectedFilmPresetId,
                onSelect = { dispatch.invoke(SelectFilmPreset(it)) }
            )
            SettingDivider()
            SettingsRow(
                title = "Save & manage",
                value = filmHubPresetsSummary(state),
                onClick = { dispatch.invoke(OpenFilmSimSubPage(FilmSimSection.Presets)) }
            )
        }
        SettingsGroup {
            FilmSimHubRow("Film", filmHubFilmSummary(look)) {
                dispatch.invoke(OpenFilmSimSubPage(FilmSimSection.Film))
            }
            SettingDivider()
            ToggleSubmenuRow(
                title = "DIR Couplers",
                value = dirCouplersSummary(look),
                checked = look.dirCouplersAmount > 0f,
                onOpen = { dispatch.invoke(OpenFilmSimSubPage(FilmSimSection.DirCouplers)) },
                onCheckedChange = {
                    dispatch.invoke(
                        SetFilmSimNumericValue(
                            FilmSimNumericParameter.DirCouplersAmount,
                            if (it) 1f else 0f
                        )
                    )
                }
            )
            SettingDivider()
            FilmSimHubRow("Print", filmHubPrintSummary(look)) {
                dispatch.invoke(OpenFilmSimSubPage(FilmSimSection.Print))
            }
            SettingDivider()
            FilmSimHubRow("Filters", filmHubFiltersSummary(look)) {
                dispatch.invoke(OpenFilmSimSubPage(FilmSimSection.Filters))
            }
            SettingDivider()
            FilmSimHubRow("Diffusion", filmHubDiffusionSummary(look)) {
                dispatch.invoke(OpenFilmSimSubPage(FilmSimSection.Diffusion))
            }
            SettingDivider()
            ToggleSubmenuRow(
                title = "Grain",
                value = filmHubGrainSummary(look),
                checked = look.grainEnabled,
                onOpen = { dispatch.invoke(OpenFilmSimSubPage(FilmSimSection.Grain)) },
                onCheckedChange = { dispatch.invoke(SetFilmSimFlag(FilmSimFlag.GrainEnabled, it)) }
            )
            SettingDivider()
            ToggleSubmenuRow(
                title = "Halation",
                value = filmHubHalationSummary(look),
                checked = look.halationEnabled,
                onOpen = { dispatch.invoke(OpenFilmSimSubPage(FilmSimSection.Halation)) },
                onCheckedChange = { dispatch.invoke(SetFilmSimFlag(FilmSimFlag.HalationEnabled, it)) }
            )
            SettingDivider()
            ToggleSubmenuRow(
                title = "Scanner",
                value = filmHubScannerSummary(look),
                checked = look.scannerEnabled,
                onOpen = { dispatch.invoke(OpenFilmSimSubPage(FilmSimSection.Scanner)) },
                onCheckedChange = { dispatch.invoke(SetFilmSimFlag(FilmSimFlag.ScannerEnabled, it)) }
            )
            SettingDivider()
            FilmSimHubRow("Output", filmHubOutputSummary(look)) {
                dispatch.invoke(OpenFilmSimSubPage(FilmSimSection.Output))
            }
        }
    }
}

@Composable
private fun FilmSimHubRow(title: String, value: String, onClick: () -> Unit) {
    SettingsRow(title = title, value = value, onClick = onClick)
}

private fun filmHubFilmSummary(look: FilmSimLook): String {
    val stock = FilmStocks.films.getOrElse(look.film) { "?" }
    val workflow = FilmStocks.processes.getOrElse(look.process) { "?" }
    return "$stock · $workflow"
}

private fun filmHubPrintSummary(look: FilmSimLook): String {
    val paper = FilmStocks.papers.getOrElse(look.paper) { "?" }
    return "$paper · ${formatSignedEv(look.printExposureEv)}"
}

private fun filmHubFiltersSummary(look: FilmSimLook): String {
    val parts = mutableListOf<String>()
    if (look.cameraUvFilterEnabled) parts.add("UV")
    if (look.cameraIrFilterEnabled) parts.add("IR")
    if (parts.isEmpty()) return "Off"
    return "On · " + parts.joinToString(" + ")
}

private fun filmHubGrainSummary(look: FilmSimLook): String {
    if (!look.grainEnabled) return "Off"
    var summary = "On · ${formatAmount(look.grainAmount)}"
    if (look.grainAnimate) summary += " · animated"
    return summary
}

private fun filmHubHalationSummary(look: FilmSimLook): String {    if (!look.halationEnabled) return "Off"
    val parts = mutableListOf<String>()
    if (look.scatterAmount > 0) parts.add("scatter")
    if (look.halationAmount > 0) parts.add("halation")
    if (look.halationBoostEv > 0) parts.add("boost")
    if (parts.isEmpty()) return "On"
    return "On · " + parts.joinToString(" + ")
}

private fun filmHubScannerSummary(look: FilmSimLook): String {
    if (!look.scannerEnabled) return "Off"
    return "On"
}

private fun filmHubOutputSummary(look: FilmSimLook): String =
    FilmStocks.colorSpaces.getOrElse(look.outputColorSpace) { "?" }

private fun filmPresetSummary(look: FilmSimLook): String {
    val stock = FilmStocks.films.getOrElse(look.film) { "?" }
    val paper = FilmStocks.papers.getOrElse(look.paper) { "?" }
    val workflow = FilmStocks.processes.getOrElse(look.process) { "?" }
    return "$stock · $paper · $workflow"
}

private fun filmHubPresetsSummary(state: SettingsUiState): String {
    val base = state.values.selectedFilmPreset()
    val modified = state.values.isFilmPresetModified()
    val counts = if (state.values.filmPresets.isEmpty()) {
        "${FilmFactoryPresets.all.size} factory"
    } else {
        "${FilmFactoryPresets.all.size} factory · ${state.values.filmPresets.size} saved"
    }
    return when {
        base == null -> "Custom · $counts"
        modified -> "${base.name} • Modified · $counts"
        else -> "${base.name} · $counts"
    }
}

private fun dirCouplersSummary(look: FilmSimLook): String {
    if (look.dirCouplersAmount <= 0f) return "Off"
    return "On · ${formatAmount(look.dirCouplersAmount)}"
}

private fun filmHubDiffusionSummary(look: FilmSimLook): String {
    val parts = mutableListOf<String>()
    if (look.cameraDiffusionEnabled) {
        parts.add(
            "Camera · " + FilmStocks.diffusionFamilies.getOrElse(look.cameraDiffusionFamily) { "?" }
        )
    }
    if (look.printDiffusionEnabled) {
        parts.add(
            "Print · " + FilmStocks.diffusionFamilies.getOrElse(look.printDiffusionFamily) { "?" }
        )
    }
    if (parts.isEmpty()) return "Off"
    return "On · " + parts.joinToString(" + ")
}

private fun formatSignedEv(value: Float): String {
    val rounded = kotlin.math.round(value * 10) / 10.0
    return (if (rounded >= 0) "+" else "") + rounded + " EV"
}

private fun formatAmount(value: Float): String {
    val rounded = kotlin.math.round(value * 100) / 100.0
    return rounded.toString()
}

@Composable
internal fun FilmSimPresetGroup(state: SettingsUiState, dispatch: SettingsDispatch) {
    var saving by androidx.compose.runtime.remember { androidx.compose.runtime.mutableStateOf(false) }
    var saveName by androidx.compose.runtime.remember { androidx.compose.runtime.mutableStateOf("") }
    var renameTarget by androidx.compose.runtime.remember { androidx.compose.runtime.mutableStateOf<FilmPreset?>(null) }
    var renameName by androidx.compose.runtime.remember { androidx.compose.runtime.mutableStateOf("") }
    var deleteTarget by androidx.compose.runtime.remember { androidx.compose.runtime.mutableStateOf<FilmPreset?>(null) }
    var updateTarget by androidx.compose.runtime.remember { androidx.compose.runtime.mutableStateOf<FilmPreset?>(null) }
    val userPresets = state.values.filmPresets
    val base = state.values.selectedFilmPreset()
    val modified = state.values.isFilmPresetModified()
    val baseIsUser = base != null && state.values.isUserFilmPreset(base.id)

    SettingsGroup(title = "Current") {
        SettingsRow(
            title = base?.name ?: "Custom look",
            value = when {
                base == null -> "Custom"
                modified -> "Modified"
                else -> "Saved"
            },
            onClick = null
        )
        if (modified && base != null) {
            SettingDivider()
            SettingsRow(
                title = "Revert to '${base.name}'",
                onClick = { dispatch.invoke(RevertFilmPreset) }
            )
        }
        if (modified && base != null && baseIsUser) {
            SettingDivider()
            SettingsRow(
                title = "Update '${base.name}'",
                onClick = { updateTarget = base }
            )
        }
        SettingDivider()
        SettingsRow(
            title = "Save current look as new",
            onClick = {
                saveName = ""
                saving = true
            }
        )
    }

    SettingsGroup(title = "Factory") {
        FilmFactoryPresets.all.forEachIndexed { index, preset ->
            if (index > 0) SettingDivider()
            val isSelected = state.values.selectedFilmPresetId == preset.id
            SettingsRow(
                title = preset.name,
                onClick = { dispatch.invoke(SelectFilmPreset(preset.id)) },
                trailing = {
                    androidx.compose.material3.RadioButton(selected = isSelected, onClick = null)
                }
            )
        }
    }

    SettingsGroup(title = "Yours") {
        if (userPresets.isEmpty()) {
            SettingsRow(
                title = "No saved presets",
                onClick = null
            )
        } else {
            userPresets.forEachIndexed { index, preset ->
                if (index > 0) SettingDivider()
                val isSelected = state.values.selectedFilmPresetId == preset.id
                SettingsRow(
                    title = preset.name,
                    onClick = { dispatch.invoke(SelectFilmPreset(preset.id)) },
                    trailing = {
                        androidx.compose.foundation.layout.Row(
                            verticalAlignment = androidx.compose.ui.Alignment.CenterVertically
                        ) {
                            androidx.compose.material3.TextButton(onClick = {
                                renameName = preset.name
                                renameTarget = preset
                            }) { androidx.compose.material3.Text("Rename") }
                            androidx.compose.material3.TextButton(onClick = {
                                deleteTarget = preset
                            }) { androidx.compose.material3.Text("Delete") }
                            androidx.compose.material3.RadioButton(selected = isSelected, onClick = null)
                        }
                    }
                )
            }
        }
    }

    if (saving) {
        androidx.compose.material3.AlertDialog(
            onDismissRequest = { saving = false },
            title = { androidx.compose.material3.Text("Save film preset") },
            text = {
                androidx.compose.material3.OutlinedTextField(
                    value = saveName,
                    onValueChange = { saveName = it },
                    singleLine = true,
                    label = { androidx.compose.material3.Text("Name") }
                )
            },
            confirmButton = {
                androidx.compose.material3.TextButton(onClick = {
                    dispatch.invoke(SaveFilmPreset(saveName))
                    saving = false
                }) { androidx.compose.material3.Text("Save") }
            },
            dismissButton = {
                androidx.compose.material3.TextButton(onClick = { saving = false }) {
                    androidx.compose.material3.Text("Cancel")
                }
            }
        )
    }
    renameTarget?.let { target ->
        androidx.compose.material3.AlertDialog(
            onDismissRequest = { renameTarget = null },
            title = { androidx.compose.material3.Text("Rename preset") },
            text = {
                androidx.compose.material3.OutlinedTextField(
                    value = renameName,
                    onValueChange = { renameName = it },
                    singleLine = true,
                    label = { androidx.compose.material3.Text("Name") }
                )
            },
            confirmButton = {
                androidx.compose.material3.TextButton(onClick = {
                    dispatch.invoke(RenameFilmPreset(target.id, renameName))
                    renameTarget = null
                }) { androidx.compose.material3.Text("Save") }
            },
            dismissButton = {
                androidx.compose.material3.TextButton(onClick = { renameTarget = null }) {
                    androidx.compose.material3.Text("Cancel")
                }
            }
        )
    }
    deleteTarget?.let { target ->
        androidx.compose.material3.AlertDialog(
            onDismissRequest = { deleteTarget = null },
            title = { androidx.compose.material3.Text("Delete preset?") },
            text = { androidx.compose.material3.Text("Delete '${target.name}'? The current look is kept.") },
            confirmButton = {
                androidx.compose.material3.TextButton(onClick = {
                    dispatch.invoke(DeleteFilmPreset(target.id))
                    deleteTarget = null
                }) { androidx.compose.material3.Text("Delete") }
            },
            dismissButton = {
                androidx.compose.material3.TextButton(onClick = { deleteTarget = null }) {
                    androidx.compose.material3.Text("Cancel")
                }
            }
        )
    }
    updateTarget?.let { target ->
        androidx.compose.material3.AlertDialog(
            onDismissRequest = { updateTarget = null },
            title = { androidx.compose.material3.Text("Update preset?") },
            text = { androidx.compose.material3.Text("Overwrite '${target.name}' with the current look?") },
            confirmButton = {
                androidx.compose.material3.TextButton(onClick = {
                    dispatch.invoke(UpdateFilmPreset(target.id))
                    updateTarget = null
                }) { androidx.compose.material3.Text("Update") }
            },
            dismissButton = {
                androidx.compose.material3.TextButton(onClick = { updateTarget = null }) {
                    androidx.compose.material3.Text("Cancel")
                }
            }
        )
    }
}
