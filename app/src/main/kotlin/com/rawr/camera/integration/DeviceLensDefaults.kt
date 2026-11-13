package com.rawr.camera.integration

import com.rawr.camera.settings.model.LensProfile
import com.rawr.camera.settings.preferences.LensProfileCodec
import com.rawr.camera.settings.preferences.BundledDcpProfiles
import org.json.JSONObject

data class DeviceCameraProfile(
    val id: String,
    val name: String,
    val description: String,
    val isDefault: Boolean,
    val lenses: List<LensProfile>
)

/** This device's built-in lenses (native built-in camera profile); the Lens settings start from these. */
object DeviceLensDefaults {
    val profiles: List<DeviceCameraProfile> by lazy {
        NativePreviewEngine().deviceCameraProfiles().toList().chunked(5).map { record ->
            DeviceCameraProfile(record[0], record[1], record[2], record[3].toBoolean(),
                BundledDcpProfiles.defaults(record[0], LensProfileCodec.decode(record[4]).orEmpty()))
        }
    }

    val lenses: List<LensProfile> by lazy {
        val raw = NativePreviewEngine().builtInCameraProfile()
        BundledDcpProfiles.defaults(JSONObject(raw).optString("id"), LensProfileCodec.decode(raw).orEmpty())
    }
}
