package com.rawr.camera.settings.model

import com.rawr.camera.BuildConfig

/**
 * Features that are not ready for release. Each flag is on in debug builds only; release builds
 * hide the feature's UI and decode any saved or imported selection of it to the default.
 */
object FeatureFlags {
    val hdrPlusBracketed: Boolean = BuildConfig.DEBUG
}
