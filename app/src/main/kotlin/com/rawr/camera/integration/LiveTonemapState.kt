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

    /** Active AgX preset and controls; applied even while another profile is active. */
    data class Agx(
        val preset: com.rawr.camera.settings.model.AgxPreset,
        val values: com.rawr.camera.settings.model.AgxValues
    )

    private val mutableAgx = MutableStateFlow<Agx?>(null)
    val agx: StateFlow<Agx?> = mutableAgx.asStateFlow()

    fun publishAgx(value: Agx) {
        mutableAgx.value = value
    }
}
