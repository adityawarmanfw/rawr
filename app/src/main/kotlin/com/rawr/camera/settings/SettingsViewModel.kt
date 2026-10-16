package com.rawr.camera.settings

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.rawr.camera.settings.architecture.PersistentSettingsController
import com.rawr.camera.settings.architecture.RestorePersistentSettings
import com.rawr.camera.settings.architecture.SettingsController
import com.rawr.camera.settings.model.SettingsCatalog
import com.rawr.camera.settings.preferences.SettingsBackupStore
import com.rawr.camera.settings.preferences.SettingsPreferencesStore
import kotlinx.coroutines.launch

class SettingsViewModel(application: Application) : AndroidViewModel(application) {
    private val initialState = SettingsCatalog.initialState()
    val editor = SettingsPreferencesStore.editor(application, initialState.values)
    val backupStore = SettingsBackupStore(application, initialState.values)
    val controller: SettingsController = PersistentSettingsController(
        initialState = initialState,
        defaults = initialState.values,
        valuesEditor = editor
    )

    init {
        viewModelScope.launch {
            editor.values.collect { values ->
                if (values != controller.state.value.values) controller.dispatch(RestorePersistentSettings(values))
            }
        }
    }
}
