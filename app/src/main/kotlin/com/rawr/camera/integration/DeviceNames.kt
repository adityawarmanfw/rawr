package com.rawr.camera.integration

import android.os.Build

/** Best-effort display name; never use this for device matching or RAW identity. */
internal object DeviceNames {
    private val properties = listOf(
        "ro.vivo.market.name",
        "ro.vendor.oplus.market.name",
        "ro.product.marketname",
        "ro.config.marketing_name",
        "ro.vendor.product.display",
        "ro.config.devicename",
        "ro.vivo.product.release.name",
        "ro.product.vendor.model"
    )

    fun marketingModel(model: String, readProperty: (String) -> String? = { null }): String =
        properties.firstNotNullOfOrNull { key ->
            readProperty(key)?.trim()?.takeIf { it.isNotEmpty() && !it.equals("unknown", true) && !it.equals("null", true) }
        } ?: model

    fun marketingModel(): String = marketingModel(Build.MODEL.orEmpty(), SystemPropertiesUtil::get)
}

/** Reads through the native property API, without reflection into hidden Java APIs. */
internal object SystemPropertiesUtil {
    init { System.loadLibrary("rawrcam_native") }
    private external fun read(key: String): String
    fun get(key: String): String? = read(key).trim().takeIf { it.isNotEmpty() }
}
