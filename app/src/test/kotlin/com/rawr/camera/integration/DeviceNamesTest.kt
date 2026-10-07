package com.rawr.camera.integration

import org.junit.Assert.assertEquals
import org.junit.Test

class DeviceNamesTest {
    @Test fun usesRequestedPropertyPriority() {
        val properties = mapOf("ro.vivo.market.name" to " X300 Ultra ",
            "ro.vendor.oplus.market.name" to "Other name", "ro.product.vendor.model" to "V2562")
        assertEquals("X300 Ultra", DeviceNames.marketingModel("V2562", properties::get))
    }

    @Test fun skipsMissingBlankAndPlaceholderValues() {
        val properties = mapOf("ro.vivo.market.name" to " ", "ro.vendor.oplus.market.name" to "NULL",
            "ro.product.marketname" to "unknown", "ro.config.marketing_name" to "Phone")
        assertEquals("Phone", DeviceNames.marketingModel("Code", properties::get))
    }

    @Test fun vivoReleaseNamePrecedesVendorModel() {
        val properties = mapOf("ro.vivo.product.release.name" to "X300 Ultra", "ro.product.vendor.model" to "V2562")
        assertEquals("X300 Ultra", DeviceNames.marketingModel("V2562", properties::get))
    }

    @Test fun supportsEachPropertyAndFallsBackToModel() {
        for (key in listOf("ro.vivo.market.name", "ro.vendor.oplus.market.name", "ro.product.marketname",
            "ro.config.marketing_name", "ro.vendor.product.display", "ro.config.devicename", "ro.product.vendor.model")) {
            assertEquals("Phone", DeviceNames.marketingModel("Code") { if (it == key) "Phone" else null })
        }
        assertEquals("V2514", DeviceNames.marketingModel("V2514"))
    }
}
