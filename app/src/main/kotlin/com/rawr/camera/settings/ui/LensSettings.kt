package com.rawr.camera.settings.ui

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Checkbox
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.rawr.camera.settings.architecture.OpenLensEditor
import com.rawr.camera.settings.architecture.SetLensProfiles
import com.rawr.camera.settings.architecture.SettingsDispatch
import com.rawr.camera.settings.model.LensProfile
import com.rawr.camera.settings.model.LENS_NAME_MAX_LENGTH
import com.rawr.camera.settings.model.validateLensProfile
import com.rawr.camera.settings.model.SettingsUiState
import com.rawr.camera.settings.model.effectiveLensProfiles
import com.rawr.camera.settings.model.moveLens
import com.rawr.camera.settings.model.removeLens
import com.rawr.camera.settings.model.setLensEnabled
import com.rawr.camera.ui.icons.Icons
import com.rawr.camera.ui.icons.rounded.Add
import com.rawr.camera.ui.icons.rounded.ContentCopy
import com.rawr.camera.ui.icons.rounded.DeleteOutline
import com.rawr.camera.ui.icons.rounded.DragHandle
import sh.calvin.reorderable.ReorderableColumn

@Composable
internal fun LensSettings(state: SettingsUiState, dispatch: SettingsDispatch) {
    val hardware = LocalLensHardware.current
    val lenses = state.values.effectiveLensProfiles(hardware.deviceDefaults)
    val usingDefaults = state.values.lensProfiles == null
    var deleteTarget by remember { mutableStateOf<LensProfile?>(null) }
    var copyTarget by remember { mutableStateOf<LensProfile?>(null) }
    var confirmReset by remember { mutableStateOf(false) }
    val haptics = LocalSettingsHaptics.current

    SettingsPageContainer {
        SettingsGroup(
            description =
                "Lenses on the capture screen, in this order. Drag to reorder, uncheck to hide, tap to edit." +
                    if (usingDefaults) " Showing this device's built-in lenses." else ""
        ) {
            if (lenses.isEmpty()) {
                SettingsRow(title = "No lenses", supportingText = "Add a lens to choose which camera to use.")
            }
            ReorderableColumn(
                list = lenses,
                onSettle = { from, to -> dispatch.invoke(SetLensProfiles(lenses.moveLens(from, to))) },
                onMove = { haptics.detent() }
            ) { index, lens, isDragging ->
                key(lens.name) {
                    ReorderableItem {
                        Surface(
                            color =
                                if (isDragging) MaterialTheme.colorScheme.surfaceContainerHighest
                                else MaterialTheme.colorScheme.surfaceContainerLow,
                            shadowElevation = if (isDragging) 6.dp else 0.dp
                        ) {
                            Column {
                                if (index > 0) SettingDivider()
                                LensRow(
                                    lens = lens,
                                    canDisable = lenses.count { it.enabled } > 1,
                                    dragHandle = {
                                        IconButton(onClick = {}, modifier = Modifier.draggableHandle()) {
                                            Icon(Icons.Rounded.DragHandle, contentDescription = "Reorder ${lens.name}")
                                        }
                                    },
                                    onEnabledChange = { enabled ->
                                        dispatch.invoke(SetLensProfiles(lenses.setLensEnabled(index, enabled)))
                                    },
                                    onOpen = { dispatch.invoke(OpenLensEditor(lens.name)) },
                                    onCopy = { copyTarget = lens },
                                    onDelete = { deleteTarget = lens }
                                )
                            }
                        }
                    }
                }
            }
        }
        SettingsGroup {
            SettingsRow(
                title = "Add lens",
                leadingIcon = Icons.Rounded.Add,
                testTag = SettingsTestTags.row("Lens", "add"),
                onClick = { dispatch.invoke(OpenLensEditor(null)) }
            )
            SettingDivider()
            SettingsRow(
                title = "Reset to device default",
                supportingText = "Replace this list with the built-in lenses for this phone.",
                enabled = !usingDefaults,
                onClick = { confirmReset = true }
            )
        }
    }

    copyTarget?.let { target ->
        key(target.name) {
            CopyLensDialog(
                target = target,
                lenses = lenses,
                onDismiss = { copyTarget = null },
                onCopy = { duplicate ->
                    dispatch.invoke(SetLensProfiles(lenses + duplicate))
                    copyTarget = null
                }
            )
        }
    }
    deleteTarget?.let { target ->
        val lastEnabled = target.enabled && lenses.count { it.enabled } <= 1
        AlertDialog(
            onDismissRequest = { deleteTarget = null },
            title = { Text("Delete lens ${target.name}?") },
            text = {
                Text(
                    if (lastEnabled) "This is the only enabled lens. Enable another lens first."
                    else "The lens and its vendor keys are removed."
                )
            },
            confirmButton = {
                TextButton(enabled = !lastEnabled, onClick = {
                    dispatch.invoke(SetLensProfiles(lenses.removeLens(target.name)))
                    deleteTarget = null
                }) { Text("Delete") }
            },
            dismissButton = { TextButton(onClick = { deleteTarget = null }) { Text("Cancel") } }
        )
    }
    if (confirmReset) {
        AlertDialog(
            onDismissRequest = { confirmReset = false },
            title = { Text("Reset lenses?") },
            text = { Text("Your lens list is replaced with this phone's built-in lenses.") },
            confirmButton = {
                TextButton(onClick = {
                    dispatch.invoke(SetLensProfiles(null))
                    confirmReset = false
                }) { Text("Reset") }
            },
            dismissButton = { TextButton(onClick = { confirmReset = false }) { Text("Cancel") } }
        )
    }
}

@Composable
private fun LensRow(
    lens: LensProfile,
    canDisable: Boolean,
    dragHandle: @Composable () -> Unit,
    onEnabledChange: (Boolean) -> Unit,
    onOpen: () -> Unit,
    onCopy: () -> Unit,
    onDelete: () -> Unit
) {
    Row(
        modifier =
            Modifier
                .fillMaxWidth()
                .heightIn(min = SettingsRowMinHeight)
                .testTag(SettingsTestTags.row("Lens", lens.name)),
        verticalAlignment = Alignment.CenterVertically
    ) {
        dragHandle()
        Checkbox(
            checked = lens.enabled,
            enabled = !lens.enabled || canDisable,
            onCheckedChange = onEnabledChange
        )
        Column(
            Modifier
                .weight(1f)
                .clickable(onClick = onOpen)
                .padding(vertical = SettingsRowVerticalPadding, horizontal = 8.dp),
            verticalArrangement = Arrangement.spacedBy(2.dp)
        ) {
            Text(
                lens.name,
                style = MaterialTheme.typography.bodyLarge,
                color =
                    if (lens.enabled) MaterialTheme.colorScheme.onSurface
                    else MaterialTheme.colorScheme.onSurface.copy(alpha = .5f)
            )
            Text(
                lensSummary(lens),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis
            )
        }
        IconButton(onClick = onCopy, modifier = Modifier.testTag(SettingsTestTags.row("Lens", "copy_${lens.name}"))) {
            Icon(Icons.Rounded.ContentCopy, contentDescription = "Copy ${lens.name}")
        }
        IconButton(onClick = onDelete) {
            Icon(Icons.Rounded.DeleteOutline, contentDescription = "Delete ${lens.name}")
        }
    }
}

@Composable
private fun CopyLensDialog(
    target: LensProfile,
    lenses: List<LensProfile>,
    onDismiss: () -> Unit,
    onCopy: (LensProfile) -> Unit
) {
    var name by remember { mutableStateOf("") }
    val duplicate = target.copy(name = name.trim())
    val problem = validateLensProfile(duplicate, lenses).firstOrNull()
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Copy lens ${target.name}") },
        text = {
            OutlinedTextField(
                value = name,
                onValueChange = { name = it },
                label = { Text("Name") },
                singleLine = true,
                isError = name.isNotEmpty() && problem != null,
                supportingText = {
                    Text(if (name.isNotEmpty() && problem != null) problem
                        else "Choose a unique name, up to $LENS_NAME_MAX_LENGTH characters.")
                },
                modifier = Modifier.fillMaxWidth().testTag(SettingsTestTags.row("Lens", "copy_name"))
            )
        },
        confirmButton = {
            TextButton(enabled = problem == null, onClick = { onCopy(duplicate) }) { Text("Copy") }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } }
    )
}

internal fun lensSummary(lens: LensProfile): String = buildList {
    add(cameraLabel(lens.cameraId, lens.physicalCameraId))
    add(lens.stream.label)
    add(if (lens.levels.isStatic) "Static levels" else "Dynamic levels")
    if (lens.cfaBlock > 1) add(cfaBlockLabel(lens.cfaBlock))
    lens.cfaOrder?.let { add(it) }
    val keys = lens.vendorKeys.count { it.enabled }
    if (keys > 0) add(if (keys == 1) "1 key" else "$keys keys")
}.joinToString(" · ")

internal fun cameraLabel(cameraId: String, physicalCameraId: String): String = when {
    cameraId.isBlank() -> "No camera"
    physicalCameraId.isNotEmpty() -> "Camera $physicalCameraId (in $cameraId)"
    else -> "Camera $cameraId"
}
