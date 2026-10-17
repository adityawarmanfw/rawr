package com.rawr.camera.settings.ui

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import com.rawr.camera.ui.icons.Icons
import com.rawr.camera.ui.icons.rounded.Add
import com.rawr.camera.ui.icons.rounded.ArrowDownward
import com.rawr.camera.ui.icons.rounded.ArrowUpward
import com.rawr.camera.ui.icons.rounded.Close
import com.rawr.camera.ui.icons.rounded.DeleteOutline
import com.rawr.camera.ui.icons.rounded.Edit
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.rawr.camera.settings.architecture.*
import com.rawr.camera.settings.model.*

@Composable
internal fun ImageToneSettings(
    state: SettingsUiState,
    dispatch: SettingsDispatch,
    onImportLut: (String?) -> Unit = {}
) {
    val tone = state.values.activeImageTone()
    val profileLabel = state.values.activeProfileLabel()
    LazyColumn(
        modifier = Modifier.fillMaxWidth().widthIn(max = SettingsContentWidth),
        contentPadding = PaddingValues(horizontal = 16.dp, vertical = 12.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        item {
            SettingsGroup(
                title = "Tone Mapping",
                description = if (state.values.isLogActive) "LOG recording bypasses tone adjustments." else "Turns scene light into the ${if (state.values.isVideo) "video" else "photo"}: contrast, highlight roll-off and colour. Runs after Dynamic Range."
            ) {
                if (state.values.isLogActive) {
                    SettingsRow(state.values.videoLogProfile.label)
                } else {
                    SettingsSelectionRow(
                        title = ColorRenderProfile.RawrBase.label,
                        selected = state.values.regularRenderProfile() == ColorRenderProfile.RawrBase,
                        testTag = SettingsTestTags.row("ImageTone", "profile_rawr_ntrl"),
                        onClick = { dispatch.invoke(SetColorRenderProfile(ColorRenderProfile.RawrBase)) }
                    )
                    SettingsSelectionRow(
                        title = "AgX",
                        selected = state.values.isAgxActive(),
                        testTag = SettingsTestTags.row("ImageTone", "profile_agx"),
                        onClick = { dispatch.invoke(SetAgxPreset(state.values.regularAgxPreset())) }
                    )
                    val display = if (state.values.isVideo) ColorRenderProfile.Rec709 else ColorRenderProfile.SRgb
                    SettingsSelectionRow(
                        title = display.label,
                        selected = state.values.regularRenderProfile() == display,
                        testTag = SettingsTestTags.row("ImageTone", "profile_${display.name}"),
                        onClick = { dispatch.invoke(SetColorRenderProfile(display)) }
                    )
                }
                if (!state.values.isLogActive) state.values.userLutProfiles.forEach { profile ->
                    SettingDivider()
                    // Tap (row or radio) only selects. Editing lives behind the
                    // pencil button, which selects first because the LUT editor
                    // always shows the selected profile.
                    ProfileSelectionRow(
                        title = profile.name,
                        selected = state.values.regularRenderProfile() == ColorRenderProfile.UserLut && state.values.regularLutProfileId() == profile.id,
                        value = "${profile.stages.size} LUT${if (profile.stages.size == 1) "" else "s"}",
                        testTag = SettingsTestTags.row("ImageTone", "profile_${SettingsTestTags.slug(profile.name)}"),
                        onSelect = { dispatch.invoke(SelectUserLutProfile(profile.id)) },
                        onEdit = {
                            dispatch.invoke(SelectUserLutProfile(profile.id))
                            dispatch.invoke(OpenSection(SettingsSection.LutProfile))
                        }
                    )
                }
                SettingDivider()
                if (!state.values.isLogActive) SettingsRow("Import LUT", leadingIcon = Icons.Rounded.Add, onClick = {
                    onImportLut(null)
                })
            }
        }
        if (state.values.isAgxActive()) {
            item(key = "agx_look") {
                SettingsGroup(title = "Look", description = "Variations of AgX; each keeps its own adjustments below.") {
                    AgxPreset.entries.forEachIndexed { index, preset ->
                        if (index > 0) SettingDivider()
                        SettingsSelectionRow(
                            title = preset.lookLabel,
                            selected = state.values.regularAgxPreset() == preset,
                            testTag = SettingsTestTags.row("ImageTone", "profile_agx_${preset.name}"),
                            onClick = { dispatch.invoke(SetAgxPreset(preset)) }
                        )
                    }
                }
            }
        }
        if (state.values.isAgxActive()) {
            item(key = "agx_saved_looks") { AgxSavedLooksGroup(state.values, dispatch) }
        }
        if (state.values.isAgxActive()) {
            val preset = state.values.regularAgxPreset()
            val values = state.values.activeAgxValues()
            AgxGroup.entries.forEach { group ->
                item(key = "agx_${group.name}") {
                    SettingsGroup(
                        title = group.title,
                        description = if (group == AgxGroup.Range)
                            "${preset.lookLabel} look: stored per look. AgX replaces the display tone controls." else null
                    ) {
                        if (group == AgxGroup.Range) {
                            ToneSlider(ImageToneSpecs.renderExposure, tone.renderExposure, dispatch)
                            SettingDivider()
                        }
                        AgxCatalog.group(group).forEachIndexed { index, spec ->
                            if (index > 0) SettingDivider()
                            StandaloneNumericSliderRow(
                                identity = "agx_${spec.parameter.name}",
                                label = spec.longLabel,
                                minimum = spec.minimum,
                                maximum = spec.maximum,
                                step = spec.step,
                                decimals = spec.valueDecimals,
                                value = values.valueOf(spec.parameter),
                                defaultValue = preset.defaults.valueOf(spec.parameter),
                                valueFormatter = { spec.format(it) },
                                dimDisabledText = false,
                                onValueChange = { dispatch.invoke(SetAgxValue(spec.parameter, it)) }
                            )
                        }
                    }
                }
            }
        }
        if (!state.values.isLogActive && !state.values.isAgxActive()) {
            item {
                SettingsGroup(
                    title = "Tone — $profileLabel",
                    description = "Stored per render profile. Edits affect only $profileLabel. " +
                        "Exposure applies to scene light, before everything."
                ) {
                    ToneSlider(ImageToneSpecs.renderExposure, tone.renderExposure, dispatch)
                }
            }
            item {
                SettingsGroup(
                    title = "Grade",
                    description = "Before the render, in ${gradeSpaceLabel(state.values)}."
                ) {
                    ToneSlider(ImageToneSpecs.highlights, tone.highlights, dispatch)
                    SettingDivider()
                    ToneSlider(ImageToneSpecs.shadows, tone.shadows, dispatch)
                    SettingDivider()
                    ToneSlider(ImageToneSpecs.midtones, tone.midtones, dispatch)
                    SettingDivider()
                    ToneSlider(ImageToneSpecs.contrast, tone.contrast, dispatch)
                    SettingDivider()
                    ToneSlider(ImageToneSpecs.saturation, tone.saturation, dispatch)
                    SettingDivider()
                    ToneSlider(ImageToneSpecs.vibrance, tone.vibrance, dispatch)
                }
            }
            item {
                SettingsGroup(
                    title = "Output",
                    description = "After the render. Sets where display white and black land."
                ) {
                    ToneSlider(ImageToneSpecs.whites, tone.whites, dispatch)
                    SettingDivider()
                    ToneSlider(ImageToneSpecs.blacks, tone.blacks, dispatch)
                }
            }
        }
        item { Spacer(Modifier.height(20.dp)) }
    }
}

/**
 * Single-choice profile option: the row and radio select, the trailing pencil
 * opens the editor. Kept separate from [SettingsSelectionRow] so selection
 * never navigates away by accident.
 */
@Composable
private fun ProfileSelectionRow(
    title: String,
    selected: Boolean,
    value: String? = null,
    testTag: String? = null,
    onSelect: () -> Unit,
    onEdit: () -> Unit
) {
    Row(
        modifier =
            Modifier
                .fillMaxWidth()
                .then(if (testTag != null) Modifier.testTag(testTag) else Modifier)
                .clickable(onClick = onSelect)
                .heightIn(min = SettingsRowMinHeight)
                .padding(horizontal = SettingsRowHorizontalPadding, vertical = SettingsRowVerticalPadding),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(title, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurface)
            if (value != null) {
                Text(
                    value,
                    style = MaterialTheme.typography.labelLarge,
                    color = MaterialTheme.colorScheme.primary,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis
                )
            }
        }
        SelectedCheck(selected)
        IconButton(onClick = onEdit) {
            Icon(Icons.Rounded.Edit, contentDescription = "Edit $title")
        }
    }
}

@Composable
internal fun LutProfileSettings(
    state: SettingsUiState,
    dispatch: SettingsDispatch,
    onImportLut: (String?) -> Unit = {}
) {
    val selected =
        state.values.regularLutProfileId()?.let { id ->
            state.values.userLutProfiles.firstOrNull {
                it.id ==
                    id
            }
        }
    var renaming by remember { mutableStateOf(false) }
    var deleting by remember { mutableStateOf(false) }

    if (selected == null) {
        SettingsPageContainer {
            SettingsGroup {
                SettingsRow("No LUT profile selected")
            }
        }
        return
    }

    LazyColumn(
        modifier = Modifier.fillMaxWidth().widthIn(max = SettingsContentWidth),
        contentPadding = PaddingValues(horizontal = 16.dp, vertical = 12.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        item {
            SettingsGroup(title = "Profile") {
                SettingsRow("Name", selected.name, onClick = { renaming = true })
            }
        }
        item {
            SettingsGroup(title = "LUT Chain") {
                selected.stages.forEachIndexed { index, stage ->
                    Column(Modifier.fillMaxWidth()) {
                        SettingsRow(
                            "Stage ${index + 1}",
                            stage.fileName
                        )
                        Row(
                            Modifier.fillMaxWidth().padding(horizontal = 8.dp, vertical = 4.dp),
                            horizontalArrangement = Arrangement.End
                        ) {
                            IconButton(enabled = index > 0, onClick = {
                                dispatch.invoke(MoveUserLutStage(selected.id, stage.id, -1))
                            }) { Icon(Icons.Rounded.ArrowUpward, "Move up") }
                            IconButton(enabled = index < selected.stages.lastIndex, onClick = {
                                dispatch.invoke(MoveUserLutStage(selected.id, stage.id, 1))
                            }) { Icon(Icons.Rounded.ArrowDownward, "Move down") }
                            IconButton(onClick = {
                                dispatch.invoke(RemoveUserLutStage(selected.id, stage.id))
                            }) { Icon(Icons.Rounded.Close, "Remove LUT") }
                        }
                    }
                    SettingDivider()
                }
                SettingsRow(
                    "Add LUT",
                    leadingIcon = Icons.Rounded.Add,
                    onClick = {
                        if (selected.stages.size <
                            8
                        ) {
                            onImportLut(selected.id)
                        }
                    }
                )
            }
        }
        item {
            SettingsGroup(title = "LUT Input") {
                EnumChoice(
                    "Gamut",
                    selected.inputGamut,
                    LutGamut.entries
                ) { dispatch.invoke(SetUserLutInputGamut(selected.id, it)) }
                SettingDivider()
                EnumChoice("Transfer", selected.inputTransfer, LutTransfer.entries) {
                    dispatch.invoke(SetUserLutInputTransfer(selected.id, it))
                }
            }
        }
        item {
            val outputEnabled = selected.afterLut == AfterLutAction.ConvertToJpegSrgb
            SettingsGroup(
                title = "LUT Output",
                description =
                    if (outputEnabled) {
                        "Describes what the final LUT already produces."
                    } else {
                        "Ignored when using the LUT output directly. Select “Convert to JPEG sRGB” to configure."
                    }
            ) {
                EnumChoice(
                    "Gamut",
                    selected.outputGamut,
                    LutGamut.entries,
                    enabled = outputEnabled
                ) { dispatch.invoke(SetUserLutOutputGamut(selected.id, it)) }
                SettingDivider()
                EnumChoice(
                    "Transfer",
                    selected.outputTransfer,
                    LutTransfer.entries,
                    enabled = outputEnabled
                ) {
                    dispatch.invoke(SetUserLutOutputTransfer(selected.id, it))
                }
            }
        }
        item {
            SettingsGroup(
                title = "After LUT",
                description = "“Use directly” ignores the LUT Output gamut/transfer above."
            ) {
                SettingsSelectionRow(
                    title = "Use directly",
                    selected = selected.afterLut == AfterLutAction.UseDirectly,
                    onClick = { dispatch.invoke(SetUserLutAfterAction(selected.id, AfterLutAction.UseDirectly)) }
                )
                SettingDivider()
                SettingsSelectionRow(
                    title = "Convert to JPEG sRGB",
                    selected = selected.afterLut == AfterLutAction.ConvertToJpegSrgb,
                    onClick = { dispatch.invoke(SetUserLutAfterAction(selected.id, AfterLutAction.ConvertToJpegSrgb)) }
                )
            }
        }
        item {
            OutlinedButton(
                onClick = { deleting = true },
                modifier = Modifier.fillMaxWidth().heightIn(min = 52.dp),
                colors = ButtonDefaults.outlinedButtonColors(contentColor = MaterialTheme.colorScheme.error),
                border = BorderStroke(1.dp, MaterialTheme.colorScheme.error.copy(alpha = .65f))
            ) {
                Icon(Icons.Rounded.DeleteOutline, contentDescription = null)
                Spacer(Modifier.width(8.dp))
                Text("Delete LUT profile")
            }
        }
        item { Spacer(Modifier.height(20.dp)) }
    }

    if (renaming) {
        var name by remember(selected.id) { mutableStateOf(selected.name) }
        AlertDialog(onDismissRequest = { renaming = false }, title = { Text("Rename LUT Profile") }, text = {
            OutlinedTextField(value = name, onValueChange = {
                name =
                    it
            }, singleLine = true, label = { Text("Name") })
        }, confirmButton = {
            TextButton(onClick = {
                dispatch.invoke(RenameUserLutProfile(selected.id, name))
                renaming =
                    false
            }) { Text("Save") }
        }, dismissButton = { TextButton(onClick = { renaming = false }) { Text("Cancel") } })
    }
    if (deleting) {
        AlertDialog(onDismissRequest = {
            deleting = false
        }, title = {
            Text("Delete LUT Profile?")
        }, text = {
            Text(
                "Delete ‘${selected.name}’? LUT files that are no longer referenced by another profile will also be removed."
            )
        }, confirmButton = {
            TextButton(onClick = {
                dispatch.invoke(DeleteUserLutProfile(selected.id))
                deleting =
                    false
                dispatch.invoke(NavigateBack)
            }) { Text("Delete") }
        }, dismissButton = {
            TextButton(onClick = {
                deleting =
                    false
            }) { Text("Cancel") }
        })
    }
}

@Composable
private fun <T> EnumChoice(
    title: String,
    selected: T,
    values: List<T>,
    enabled: Boolean = true,
    onSelect: (T) -> Unit
) where T : Enum<T> {
    DropdownChoiceRow(
        title = title,
        selected = selected,
        values = values,
        label = {
            when (it) {
                is LutGamut -> it.label
                is LutTransfer -> it.label
                else -> it.name
            }
        },
        enabled = enabled,
        onSelect = onSelect
    )
}

/**
 * The encoding the Grade sliders run in: a log LUT's own input, DaVinci
 * Intermediate for linear and display-referred (look) LUT inputs, else the
 * render's own.
 */
private fun gradeSpaceLabel(values: SettingsValues): String =
    values.selectedLutProfile()?.inputTransfer?.let { it.takeIf(::isLogTransfer)?.label ?: "DaVinci Intermediate" }
        ?: when (values.regularRenderProfile()) {
            ColorRenderProfile.SRgb -> "sRGB"
            ColorRenderProfile.Rec709 -> "Rec.709"
            else -> "DaVinci Intermediate"
        }

private fun isLogTransfer(transfer: LutTransfer): Boolean = when (transfer) {
    LutTransfer.DaVinciIntermediate, LutTransfer.LogC3, LutTransfer.SLog3, LutTransfer.VLog, LutTransfer.FLog2C -> true
    else -> false
}

@Composable
private fun ToneSlider(spec: NumericSettingSpec, value: Float, dispatch: SettingsDispatch) {
    NumericSliderRow(spec, value) { dispatch.invoke(SetNumericValue(spec.parameter, it)) }
}
