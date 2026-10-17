package com.rawr.camera.renderer

import androidx.compose.runtime.Composable
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

/** Editor-side saved AgX looks: apply, save, update, rename, delete, export, import. */
@Composable
internal fun RendererAgxLookControls(v: SettingsValues, dispatch: SettingsDispatch) {
    val files = rememberLookFiles(dispatch)
    var dialog by remember { mutableStateOf<String?>(null) }
    var target by remember { mutableStateOf<AgxLook?>(null) }
    val current = v.matchingAgxLook()
    if (v.agxLooks.isNotEmpty()) {
        EditorChoice("Saved look", current?.name ?: "None", v.agxLooks.map { it.name }) {
            dispatch.invoke(ApplyAgxLook(v.agxLooks[it].id))
        }
    }
    val actions = buildList<Pair<String, () -> Unit>> {
        add("Save as new look" to { dialog = "save" })
        v.agxLooks.filter { it.id != current?.id }.forEach { look ->
            add("Update '${look.name}'" to { target = look; dialog = "update" })
        }
        if (current != null) {
            add("Rename '${current.name}'" to { target = current; dialog = "rename" })
            add("Delete '${current.name}'" to { target = current; dialog = "delete" })
        }
        add("Export current look" to {
            val name = current?.name ?: "AgX ${v.regularAgxPreset().lookLabel}"
            files.export(name, LookFileCodec.exportAgx(AgxLook("", name, v.regularAgxPreset(), v.activeAgxValues())))
        })
        add("Import look file" to { files.import() })
    }
    EditorChoice("Manage looks", "…", actions.map { it.first }) { actions[it].second() }
    val look = target
    when (dialog) {
        "save" -> LookNameDialog("Save AgX look", "", "Save", { dialog = null }) { dispatch.invoke(SaveAgxLook(it)) }
        "rename" -> if (look != null) LookNameDialog("Rename look", look.name, "Save", { dialog = null }) {
            dispatch.invoke(RenameAgxLook(look.id, it))
        }
        "update" -> if (look != null) LookConfirmDialog("Update look?", "Overwrite '${look.name}' with the current AgX look?",
            "Update", { dialog = null }) { dispatch.invoke(UpdateAgxLook(look.id)) }
        "delete" -> if (look != null) LookConfirmDialog("Delete look?", "Delete '${look.name}'? The current adjustments are kept.",
            "Delete", { dialog = null }) { dispatch.invoke(DeleteAgxLook(look.id)) }
    }
}
