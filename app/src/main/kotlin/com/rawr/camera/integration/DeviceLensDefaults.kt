package com.rawr.camera.integration

import com.rawr.camera.settings.model.LensProfile
import com.rawr.camera.settings.preferences.LensProfileCodec

/** This device's built-in lenses (native built-in camera profile); the Lens settings start from these. */
object DeviceLensDefaults {
    val lenses: List<LensProfile> by lazy {
        LensProfileCodec.decode(NativePreviewEngine().builtInCameraProfile()).orEmpty()
    }
}
