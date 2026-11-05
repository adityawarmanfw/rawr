package com.rawr.camera.settings.ui

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.CustomAccessibilityAction
import androidx.compose.ui.semantics.customActions
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import com.rawr.camera.settings.model.LensImage
import com.rawr.camera.ui.icons.Icons
import com.rawr.camera.ui.icons.rounded.DeleteOutline
import com.rawr.camera.ui.icons.rounded.DragHandle
import sh.calvin.reorderable.ReorderableColumn

/** One summary on the settings page; profile selection and management share a compact picker. */
@Composable
internal fun LensDcpProfiles(
    image: LensImage,
    importing: Boolean,
    error: String?,
    onSelect: (String?) -> Unit,
    onRemove: (String) -> Unit,
    onImport: () -> Unit,
    onMove: (Int, Int) -> Unit = { _, _ -> },
    onSort: (Boolean) -> Unit = {}
) {
    var open by remember { mutableStateOf(false) }
    val haptics = LocalSettingsHaptics.current
    SettingsGroup(title = "Colour") {
        SettingsRow(
            title = if (importing) "Importing profile…" else "DCP profile",
            value = image.dcp?.name ?: "Camera default",
            supportingText = "Applies the profile’s colour and tone before your selected look; embeds it in DNG files",
            enabled = !importing,
            testTag = lensEditorTag("dcp"),
            onClick = { open = true }
        )
        error?.let {
            Text(it, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall,
                modifier = Modifier.padding(horizontal = SettingsRowHorizontalPadding, vertical = 12.dp))
        }
    }
    if (open) {
        AlertDialog(
            modifier = Modifier.testTag(lensEditorTag("dcpPicker")),
            onDismissRequest = { open = false },
            title = {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text("DCP profiles", modifier = Modifier.weight(1f))
                    Box {
                        var sorting by remember { mutableStateOf(false) }
                        TextButton(onClick = { sorting = true }, enabled = image.availableDcps.size > 1,
                            modifier = Modifier.testTag(lensEditorTag("dcpSort"))) { Text("Sort") }
                        DropdownMenu(expanded = sorting, onDismissRequest = { sorting = false }) {
                            listOf(false to "Name (A–Z)", true to "Name (Z–A)").forEach { (descending, label) ->
                                DropdownMenuItem(text = { Text(label) }, onClick = { sorting = false; onSort(descending) },
                                    modifier = Modifier.testTag(lensEditorTag(if (descending) "dcpSortDescending" else "dcpSortAscending")))
                            }
                        }
                    }
                }
            },
            text = {
                Column(Modifier.fillMaxWidth().heightIn(max = 360.dp)
                    .verticalScroll(rememberScrollState()).selectableGroup()) {
                    DcpChoice("Camera default", image.dcp == null, lensEditorTag("dcpDefault"),
                        onSelect = { onSelect(null) })
                    ReorderableColumn(
                        list = image.availableDcps,
                        onSettle = onMove,
                        onMove = { haptics.detent() }
                    ) { index, profile, _ ->
                        key(profile.file) {
                            ReorderableItem {
                                DcpChoice(profile.name, image.dcp?.file == profile.file, lensEditorTag("dcp_${profile.file}"),
                                    onSelect = { onSelect(profile.file) },
                                    modifier = Modifier.semantics {
                                        customActions = buildList {
                                            if (index > 0) add(CustomAccessibilityAction("Move up") { onMove(index, index - 1); true })
                                            if (index < image.availableDcps.lastIndex)
                                                add(CustomAccessibilityAction("Move down") { onMove(index, index + 1); true })
                                        }
                                    },
                                    trailing = {
                                        IconButton(
                                            onClick = { onRemove(profile.file) },
                                            modifier = Modifier.size(40.dp).testTag(lensEditorTag("removeDcp_${profile.file}"))
                                        ) {
                                            Icon(Icons.Rounded.DeleteOutline, contentDescription = "Remove ${profile.name}")
                                        }
                                        IconButton(onClick = {}, modifier = Modifier.size(40.dp).draggableHandle()
                                            .testTag(lensEditorTag("moveDcp_${profile.file}"))) {
                                            Icon(Icons.Rounded.DragHandle, contentDescription = "Reorder ${profile.name}")
                                        }
                                    })
                            }
                        }
                    }
                }
            },
            confirmButton = {
                TextButton(onClick = { open = false }, modifier = Modifier.testTag(lensEditorTag("dcpDone"))) {
                    Text("Done")
                }
            },
            dismissButton = {
                TextButton(onClick = { open = false; onImport() },
                    modifier = Modifier.testTag(lensEditorTag("dcpImport"))) { Text("Import DCP") }
            }
        )
    }
}

@Composable
private fun DcpChoice(
    name: String,
    selected: Boolean,
    tag: String,
    onSelect: () -> Unit,
    modifier: Modifier = Modifier,
    trailing: @Composable () -> Unit = {}
) {
    Row(
        modifier.fillMaxWidth().heightIn(min = 56.dp).testTag(tag)
            .selectable(selected = selected, role = Role.RadioButton, onClick = onSelect),
        verticalAlignment = Alignment.CenterVertically
    ) {
        RadioButton(selected = selected, onClick = null)
        Text(name, style = MaterialTheme.typography.bodyMedium,
            modifier = Modifier.weight(1f).padding(start = 12.dp, top = 8.dp, bottom = 8.dp))
        trailing()
    }
}
