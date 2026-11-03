package com.rawr.camera.integration

import com.rawr.camera.settings.model.LensProfile
import com.rawr.camera.settings.preferences.LensProfileCodec
import com.rawr.camera.settings.preferences.BundledDcpProfiles
import org.json.JSONObject

/** This device's built-in lenses (native built-in camera profile); the Lens settings start from these. */
object DeviceLensDefaults {
    val lenses: List<LensProfile> by lazy {
        val raw = NativePreviewEngine().builtInCameraProfile()
        BundledDcpProfiles.defaults(JSONObject(raw).optString("id"), LensProfileCodec.decode(raw).orEmpty())
    }
}
