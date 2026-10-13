package com.rawr.camera.renderer

import com.rawr.camera.settings.architecture.OpenSection
import com.rawr.camera.settings.architecture.PersistentSettingsController
import com.rawr.camera.settings.architecture.SettingsController
import com.rawr.camera.settings.model.SettingsCatalog
import com.rawr.camera.settings.model.SettingsSection
import com.rawr.camera.settings.model.SettingsValues
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow

/** Job-local recipe state. No preference writer or shared preset editor is attached. */
internal class RendererEditorSession(
    initial: SettingsValues,
    private val onDraftChanged: (SettingsValues) -> Unit
) {
    private val mutableApplied = MutableStateFlow(initial)
    val applied = mutableApplied.asStateFlow()
    private val mutableController = MutableStateFlow<SettingsController>(createController(initial))
    val controller = mutableController.asStateFlow()

    private fun createController(initial: SettingsValues): SettingsController = PersistentSettingsController(
        SettingsCatalog.initialState().copy(values = initial),
        defaults = initial,
        onValuesChanged = ::publish
    ).apply { dispatch(OpenSection(SettingsSection.ImageTone)) }

    private fun publish(values: SettingsValues) {
        mutableApplied.value = values
        onDraftChanged(values)
    }

}
