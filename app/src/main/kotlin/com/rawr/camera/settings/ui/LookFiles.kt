package com.rawr.camera.settings.ui

import android.content.ActivityNotFoundException
import android.content.Context
import android.net.Uri
import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.platform.LocalContext
import com.rawr.camera.settings.architecture.ImportAgxLook
import com.rawr.camera.settings.architecture.ImportFilmPreset
import com.rawr.camera.settings.architecture.SettingsDispatch
import com.rawr.camera.settings.preferences.ImportedLook
import com.rawr.camera.settings.preferences.LookFileCodec
import com.rawr.camera.storage.PickFile
import com.rawr.camera.storage.launchPicker

/** Exports one look to a user-picked file, or imports a film or AgX look file. */
internal class LookFiles(
    private val context: Context,
    private val create: (String) -> Unit,
    private val pick: () -> Unit,
    private val stage: (String) -> Unit
) {
    fun export(name: String, text: String) {
        stage(text)
        try { create(LookFileCodec.fileName(name)) }
        catch (e: ActivityNotFoundException) { toast(context, "No file picker available. Enable the system Files app.") }
    }

    fun import() = pick()
}

@Composable
internal fun rememberLookFiles(dispatch: SettingsDispatch): LookFiles {
    val context = LocalContext.current
    var pending by rememberSaveable { mutableStateOf<String?>(null) }
    val creator = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument(LookFileCodec.MIME)) { uri ->
        val text = pending
        pending = null
        if (uri != null && text != null) write(context, uri, text)
    }
    val picker = rememberLauncherForActivityResult(PickFile()) { uri ->
        if (uri != null) read(context, uri)?.let { look ->
            when (look) {
                is ImportedLook.Film -> dispatch.invoke(ImportFilmPreset(look.name, look.look))
                is ImportedLook.Agx -> dispatch.invoke(ImportAgxLook(look.name, look.base, look.values))
            }
            toast(context, "Imported '${look.name}'")
        }
    }
    return remember(context, creator, picker) {
        LookFiles(context, creator::launch, { picker.launchPicker(context, arrayOf(LookFileCodec.MIME, "text/plain", "*/*")) },
            { pending = it })
    }
}

private fun write(context: Context, uri: Uri, text: String) {
    runCatching {
        requireNotNull(context.contentResolver.openOutputStream(uri, "wt")) { "Unable to open file" }
            .use { it.write(text.toByteArray()) }
    }.onSuccess { toast(context, "Look exported") }
        .onFailure { toast(context, it.message ?: "Unable to export look") }
}

private fun read(context: Context, uri: Uri): ImportedLook? = runCatching {
    val text = requireNotNull(context.contentResolver.openInputStream(uri)) { "Unable to open file" }
        .use { it.readBytes(MAX_LOOK_FILE_BYTES + 1) }
    require(text.size <= MAX_LOOK_FILE_BYTES) { "Not a rawr look file" }
    LookFileCodec.import(String(text))
}.onFailure { toast(context, it.message ?: "Unable to import look") }.getOrNull()

private fun java.io.InputStream.readBytes(limit: Int): ByteArray {
    val out = java.io.ByteArrayOutputStream()
    val buffer = ByteArray(8192)
    while (out.size() < limit) {
        val n = read(buffer, 0, minOf(buffer.size, limit - out.size()))
        if (n < 0) break
        out.write(buffer, 0, n)
    }
    return out.toByteArray()
}

private const val MAX_LOOK_FILE_BYTES = 64 * 1024

private fun toast(context: Context, text: String) = Toast.makeText(context, text, Toast.LENGTH_LONG).show()

/** Name prompt shared by the save and rename flows. */
@Composable
internal fun LookNameDialog(title: String, initial: String, confirm: String, onDismiss: () -> Unit, onConfirm: (String) -> Unit) {
    var name by remember { mutableStateOf(initial) }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(title) },
        text = { OutlinedTextField(value = name, onValueChange = { name = it }, singleLine = true, label = { Text("Name") }) },
        confirmButton = { TextButton(onClick = { onConfirm(name); onDismiss() }, enabled = name.isNotBlank()) { Text(confirm) } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } }
    )
}

/** Yes/no confirmation shared by the delete and overwrite flows. */
@Composable
internal fun LookConfirmDialog(title: String, text: String, confirm: String, onDismiss: () -> Unit, onConfirm: () -> Unit) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(title) },
        text = { Text(text) },
        confirmButton = { TextButton(onClick = { onConfirm(); onDismiss() }) { Text(confirm) } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } }
    )
}

/** One overflow button per saved look; rows stay narrow on phones. */
@Composable
internal fun LookActionsMenu(label: String, actions: List<Pair<String, () -> Unit>>) {
    var open by remember { mutableStateOf(false) }
    androidx.compose.foundation.layout.Box {
        TextButton(onClick = { open = true }) { Text(label) }
        androidx.compose.material3.DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            actions.forEach { (title, action) ->
                androidx.compose.material3.DropdownMenuItem(text = { Text(title) }, onClick = { open = false; action() })
            }
        }
    }
}
