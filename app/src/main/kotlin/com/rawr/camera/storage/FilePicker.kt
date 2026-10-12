package com.rawr.camera.storage

import android.app.Activity
import android.app.AlertDialog
import android.content.ActivityNotFoundException
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.util.Log
import android.widget.Toast
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.contract.ActivityResultContract

/**
 * Opens a single file for a one-shot import. Every caller copies the file
 * straight away, so no persistable grant is needed.
 *
 * ACTION_OPEN_DOCUMENT is only served by the system DocumentsUI; some vivo
 * builds disable or replace it, and launch() then throws. Those phones' own
 * file managers do serve ACTION_GET_CONTENT, so that is the fallback, sent
 * to an explicit handler (see [launchPicker]).
 */
class PickFile : ActivityResultContract<PickFile.Request, Uri?>() {
    class Request(val mimeTypes: Array<String>, val handler: ComponentName? = null)

    override fun createIntent(context: Context, input: Request): Intent =
        getContentIntent(input.mimeTypes, input.handler != null).setComponent(input.handler)

    // Vendor selectors (vivo's File Manager) may return the pick as ClipData only.
    override fun parseResult(resultCode: Int, intent: Intent?): Uri? {
        if (resultCode != Activity.RESULT_OK) return null
        val uri = intent?.data ?: intent?.clipData?.takeIf { it.itemCount > 0 }?.getItemAt(0)?.uri
        if (uri == null) Log.w(TAG, "Picker returned OK without a file: $intent")
        return uri
    }
}

private fun getContentIntent(mimeTypes: Array<String>, getContent: Boolean): Intent =
    Intent(if (getContent) Intent.ACTION_GET_CONTENT else Intent.ACTION_OPEN_DOCUMENT)
        .addCategory(Intent.CATEGORY_OPENABLE)
        .setType(if (mimeTypes.size == 1) mimeTypes[0] else "*/*")
        .putExtra(Intent.EXTRA_MIME_TYPES, mimeTypes)

/**
 * Launches the document picker; without one, lets the user pick a GET_CONTENT
 * handler. The fallback cannot use the system chooser: its trampoline returns
 * a result at once, and the launcher then drops the real pick. The photo
 * picker is left out: it forwards non-media types to the missing DocumentsUI
 * and relaunches itself until the task closes.
 */
fun ActivityResultLauncher<PickFile.Request>.launchPicker(context: Context, mimeTypes: Array<String>) {
    try {
        launch(PickFile.Request(mimeTypes))
        return
    } catch (e: ActivityNotFoundException) {
        Log.w(TAG, "No activity for OPEN_DOCUMENT: ${e.message}")
    }
    val pm = context.packageManager
    val handlers = pm.queryIntentActivities(getContentIntent(mimeTypes, getContent = true), 0)
        .filter { info -> PHOTO_PICKER_PACKAGES.none { info.activityInfo.packageName.contains(it) } }
    Log.w(TAG, "OPEN_DOCUMENT unavailable; GET_CONTENT handlers=${handlers.map { it.activityInfo.packageName }}")
    val launchHandler = { index: Int ->
        val info = handlers[index].activityInfo
        try {
            launch(PickFile.Request(mimeTypes, ComponentName(info.packageName, info.name)))
        } catch (e: ActivityNotFoundException) {
            Log.w(TAG, "GET_CONTENT handler ${info.packageName} failed: ${e.message}")
        }
    }
    when (handlers.size) {
        0 -> {
            Log.e(TAG, "FILE_PICKER_UNAVAILABLE mimeTypes=${mimeTypes.joinToString()}")
            Toast.makeText(context, "No file picker available. Enable the system Files app.", Toast.LENGTH_LONG).show()
        }
        1 -> launchHandler(0)
        else -> AlertDialog.Builder(context)
            .setTitle("Select file with")
            .setItems(handlers.map { it.loadLabel(pm) }.toTypedArray()) { _, which -> launchHandler(which) }
            .show()
    }
}

private const val TAG = "RawrFilePicker"

private val PHOTO_PICKER_PACKAGES = listOf("photopicker", "providers.media")
