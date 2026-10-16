package com.rawr.camera.settings.ui

import androidx.compose.foundation.layout.Row
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import com.rawr.camera.settings.architecture.*
import com.rawr.camera.settings.model.*
import com.rawr.camera.settings.preferences.LookFileCodec

/** Saved AgX looks: save the current controls, apply, rename, overwrite, delete, export, import. */
@Composable
internal fun AgxSavedLooksGroup(values: SettingsValues, dispatch: SettingsDispatch) {
    val files = rememberLookFiles(dispatch)
    var saving by remember { mutableStateOf(false) }
    var renaming by remember { mutableStateOf<AgxLook?>(null) }
    var deleting by remember { mutableStateOf<AgxLook?>(null) }
    var updating by remember { mutableStateOf<AgxLook?>(null) }
    val current = values.matchingAgxLook()
    SettingsGroup(
        title = "Saved looks",
        description = "A saved look keeps its base look and every AgX control. Applying one replaces that base look's adjustments."
    ) {
        values.agxLooks.forEach { look ->
            SettingsRow(
                title = look.name,
                value = look.base.lookLabel,
                onClick = { dispatch.invoke(ApplyAgxLook(look.id)) },
                trailing = {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        LookActionsMenu("Edit", listOfNotNull(
                            ("Update with current" to { updating = look }).takeIf { look.id != current?.id },
                            "Rename" to { renaming = look },
                            "Export" to { files.export(look.name, LookFileCodec.exportAgx(look)) },
                            "Delete" to { deleting = look }
                        ))
                        RadioButton(selected = look.id == current?.id, onClick = null)
                    }
                }
            )
            SettingDivider()
        }
        SettingsRow(title = "Save current look as new", onClick = { saving = true })
        SettingDivider()
        SettingsRow(title = "Import look file", onClick = { files.import() })
    }
    if (saving) LookNameDialog("Save AgX look", "", "Save", { saving = false }) { dispatch.invoke(SaveAgxLook(it)) }
    renaming?.let { look ->
        LookNameDialog("Rename look", look.name, "Save", { renaming = null }) { dispatch.invoke(RenameAgxLook(look.id, it)) }
    }
    deleting?.let { look ->
        LookConfirmDialog("Delete look?", "Delete '${look.name}'? The current adjustments are kept.", "Delete",
            { deleting = null }) { dispatch.invoke(DeleteAgxLook(look.id)) }
    }
    updating?.let { look ->
        LookConfirmDialog("Update look?", "Overwrite '${look.name}' with the current AgX look?", "Update",
            { updating = null }) { dispatch.invoke(UpdateAgxLook(look.id)) }
    }
}
