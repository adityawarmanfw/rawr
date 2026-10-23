package com.rawr.camera.renderer

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Row
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.unit.dp
import com.rawr.camera.ui.CaptureColors
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import com.rawr.camera.settings.architecture.*
import com.rawr.camera.settings.model.*
import com.rawr.camera.settings.preferences.LookFileCodec
import com.rawr.camera.settings.ui.LookConfirmDialog
import com.rawr.camera.settings.ui.LookNameDialog
import com.rawr.camera.settings.ui.rememberLookFiles

/** Editor-side film preset library; saves land in the shared library (RendererViewModel syncs them). */
@Composable
internal fun RendererFilmPresetActions(v: SettingsValues, dispatch: SettingsDispatch) {
    val files = rememberLookFiles(dispatch)
    var dialog by remember { mutableStateOf<String?>(null) }
    val base = v.selectedFilmPreset()
    val user = base?.takeIf { v.isUserFilmPreset(it.id) }
    val actions = buildList<Pair<String, () -> Unit>> {
        add("Save as new preset" to { dialog = "save" })
        if (user != null && v.isFilmPresetModified()) add("Update '${user.name}'" to { dialog = "update" })
        if (user != null) add("Rename '${user.name}'" to { dialog = "rename" })
        if (user != null) add("Delete '${user.name}'" to { dialog = "delete" })
        add("Export current look" to {
            val name = base?.name ?: "Film look"
            files.export(name, LookFileCodec.exportFilm(FilmPreset("", name, v.filmSimLook)))
        })
        add("Import look file" to { files.import() })
    }
    EditorChoice("Manage presets", "…", actions.map { it.first }) { actions[it].second() }
    when (dialog) {
        "save" -> LookNameDialog("Save film preset", "", "Save", { dialog = null }) { dispatch.invoke(SaveFilmPreset(it)) }
        "rename" -> user?.let { p ->
            LookNameDialog("Rename preset", p.name, "Save", { dialog = null }) { dispatch.invoke(RenameFilmPreset(p.id, it)) }
        }
        "update" -> user?.let { p ->
            LookConfirmDialog("Update preset?", "Overwrite '${p.name}' with the current look?", "Update",
                { dialog = null }) { dispatch.invoke(UpdateFilmPreset(p.id)) }
        }
        "delete" -> user?.let { p ->
            LookConfirmDialog("Delete preset?", "Delete '${p.name}'? The current look is kept.", "Delete",
                { dialog = null }) { dispatch.invoke(DeleteFilmPreset(p.id)) }
        }
    }
}

/**
 * Editor-side AgX looks: one Look list (built-in then saved), a selected saved look
 * that reads "Modified" once its controls drift, and quick Save / Update / Revert.
 * The selection is editor-local; it starts from the saved look matching the values.
 */
@Composable
internal fun RendererAgxLookControls(v: SettingsValues, dispatch: SettingsDispatch) {
    val files = rememberLookFiles(dispatch)
    var dialog by remember { mutableStateOf<String?>(null) }
    var picked by rememberSaveable { mutableStateOf<String?>(null) }
    val matching = v.matchingAgxLook()
    val selected = v.agxLooks.firstOrNull { it.id == picked } ?: matching
    val modified = selected != null && matching?.id != selected.id
    val builtIn = AgxPreset.entries
    EditorChoice("Look", selected?.let { if (modified) "${it.name} · Modified" else it.name } ?: v.regularAgxPreset().lookLabel,
        builtIn.map { it.lookLabel } + v.agxLooks.map { it.name }) {
        if (it < builtIn.size) {
            picked = null
            dispatch.invoke(SetAgxPreset(builtIn[it]))
        } else {
            val look = v.agxLooks[it - builtIn.size]
            picked = look.id
            dispatch.invoke(ApplyAgxLook(look.id))
        }
    }
    Row(horizontalArrangement = Arrangement.spacedBy(4.dp), verticalAlignment = Alignment.CenterVertically) {
        @Composable fun action(label: String, onClick: () -> Unit) =
            TextButton(onClick = onClick) { Text(label, style = EditorHeadingStyle, color = CaptureColors.AccentSoft) }
        action("Save as new") { dialog = "save" }
        if (selected != null && modified) {
            action("Update") { dialog = "update" }
            action("Revert") { dispatch.invoke(ApplyAgxLook(selected.id)) }
        }
        val extra = buildList<Pair<String, () -> Unit>> {
            if (selected != null) {
                add("Rename '${selected.name}'" to { dialog = "rename" })
                add("Delete '${selected.name}'" to { dialog = "delete" })
            }
            add("Export current look" to {
                val name = selected?.name ?: "AgX ${v.regularAgxPreset().lookLabel}"
                files.export(name, LookFileCodec.exportAgx(AgxLook("", name, v.regularAgxPreset(), v.activeAgxValues())))
            })
            add("Import look file" to { files.import() })
        }
        EditorChoice("More", "…", extra.map { it.first }) { extra[it].second() }
    }
    when (dialog) {
        "save" -> LookNameDialog("Save AgX look", "", "Save", { dialog = null }) { dispatch.invoke(SaveAgxLook(it)); picked = null }
        "rename" -> if (selected != null) LookNameDialog("Rename look", selected.name, "Save", { dialog = null }) {
            dispatch.invoke(RenameAgxLook(selected.id, it))
        }
        "update" -> if (selected != null) LookConfirmDialog("Update look?", "Overwrite '${selected.name}' with the current AgX look?",
            "Update", { dialog = null }) { dispatch.invoke(UpdateAgxLook(selected.id)) }
        "delete" -> if (selected != null) LookConfirmDialog("Delete look?", "Delete '${selected.name}'? The current adjustments are kept.",
            "Delete", { dialog = null }) { picked = null; dispatch.invoke(DeleteAgxLook(selected.id)) }
    }
}
