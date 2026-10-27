package com.rawr.camera.settings

import android.os.Bundle
import com.rawr.camera.settings.preferences.BackupSection

/** Selection survives recreation while the external folder picker is open. */
internal class BackupExportSelection {
    var sections: Set<BackupSection>? = null

    fun take(): Set<BackupSection>? = sections.also { sections = null }

    fun save(state: Bundle) {
        sections?.let { state.putStringArrayList(KEY, ArrayList(it.map(BackupSection::id))) }
    }

    fun restore(state: Bundle?) {
        sections = state?.getStringArrayList(KEY)?.mapNotNull(BackupSection::fromId)?.toSet()
    }

    private companion object {
        const val KEY = "backupExportSections"
    }
}
