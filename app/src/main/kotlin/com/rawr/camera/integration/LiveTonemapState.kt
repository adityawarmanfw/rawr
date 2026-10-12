package com.rawr.camera.integration

import com.rawr.camera.model.ImageToneState
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/**
 * Process-local live rendering state.
 *
 * Persistence and realtime application are deliberately separate: Settings publishes here
 * synchronously, while DataStore writes happen asynchronously.  Camera/native code consumes
 * this state but does not own Settings persistence.
 */
object LiveTonemapState {
    private val mutable = MutableStateFlow<ImageToneState?>(null)
    val values: StateFlow<ImageToneState?> = mutable.asStateFlow()

    fun publish(value: ImageToneState) {
        mutable.value = value
    }

    /** Active AgX preset's controls; applied even while another profile is active. */
    private val mutableAgx = MutableStateFlow<com.rawr.camera.settings.model.AgxValues?>(null)
    val agx: StateFlow<com.rawr.camera.settings.model.AgxValues?> = mutableAgx.asStateFlow()

    fun publishAgx(value: com.rawr.camera.settings.model.AgxValues) {
        mutableAgx.value = value
    }
}
