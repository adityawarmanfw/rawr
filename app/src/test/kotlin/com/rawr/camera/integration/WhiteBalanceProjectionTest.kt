package com.rawr.camera.integration

import com.rawr.camera.model.WhiteBalanceMode
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

class WhiteBalanceProjectionTest {
    private fun snapshot(
        whiteBalanceMode: Int = 1,
        temperatureK: Int = 5200,
        tint: Int = 0,
        manualSupported: Boolean = false,
        awbModes: List<Int> = emptyList(),
        hasAutoWbGains: Boolean = false,
        autoWbGainR: Float = 1f,
        autoWbGainG: Float = 1f,
        autoWbGainB: Float = 1f,
        hasCct: Boolean = false,
        cctK: Int = 5200,
        cctTint: Int = 0
    ) = NativeCameraUiSnapshot(
        generation = 7,
        cameraId = "0",
        lensId = "main",
        sensitivityMin = 50,
        sensitivityMax = 3200,
        exposureTimeMinNs = 100000,
        exposureTimeMaxNs = 1000000000,
        evMinSteps = -6,
        evMaxSteps = 6,
        evStep = 1.0 / 3.0,
        manualExposureSupported = true,
        shutterPrioritySupported = false,
        isoPrioritySupported = false,
        tapAfSupported = false,
        manualFocusSupported = false,
        focusDistanceReadoutTrustworthy = false,
        minimumFocusDistance = 0f,
        hyperfocalDistance = 0f,
        exposureMode = 0,
        semanticExposureMode = 0,
        focusMode = 0,
        whiteBalanceMode = whiteBalanceMode,
        whiteBalanceTemperatureK = temperatureK,
        whiteBalanceTint = tint,
        manualWhiteBalanceSupported = manualSupported,
        supportedAwbModes = awbModes,
        hasAutoWbGains = hasAutoWbGains,
        autoWbGainR = autoWbGainR,
        autoWbGainG = autoWbGainG,
        autoWbGainB = autoWbGainB,
        hasAutoWbEstimate = hasCct,
        autoWbTemperatureK = cctK,
        autoWbTint = cctTint,
        requestedExposureTimeNs = 10000000,
        requestedSensitivity = 100,
        requestedEvSteps = 0,
        requestedManualFocusNormalized = 1f,
        appliedExposureTimeNs = null,
        appliedSensitivity = null,
        appliedEvSteps = null,
        afState = null,
        appliedFocusDistance = null
    )

    @Test
    fun snapshotCarriesWhiteBalanceFields() {
        val parsed =
            snapshot(
                whiteBalanceMode = 5,
                temperatureK = 3200,
                tint = 12,
                manualSupported = true,
                hasAutoWbGains = true,
                autoWbGainR = 1f,
                autoWbGainG = 1.2f,
                autoWbGainB = 1.8f
            )
        assertEquals(5, parsed.whiteBalanceMode)
        assertEquals(3200, parsed.whiteBalanceTemperatureK)
        assertEquals(12, parsed.whiteBalanceTint)
        assertTrue(parsed.manualWhiteBalanceSupported)
        assertTrue(parsed.hasAutoWbGains)
        assertEquals(1.2f, parsed.autoWbGainG)
    }

    @Test
    fun snapshotCarriesNativeEstimateFields() {
        val withCct = snapshot(hasCct = true, cctK = 8037, cctTint = 4)
        assertTrue(withCct.hasAutoWbEstimate)
        assertEquals(8037, withCct.autoWbTemperatureK)
        assertEquals(4, withCct.autoWbTint)

        val withoutCct = snapshot()
        assertFalse(withoutCct.hasAutoWbEstimate)
    }

    @Test
    fun snapshotDefaultsToNoNativeEstimate() {
        val parsed = snapshot()
        assertFalse(parsed.hasAutoWbEstimate)
    }

    @Test
    fun snapshotDefaultsToAutoOnly() {
        val parsed = snapshot()
        assertEquals(1, parsed.whiteBalanceMode)
        assertEquals(5200, parsed.whiteBalanceTemperatureK)
        assertEquals(0, parsed.whiteBalanceTint)
        assertFalse(parsed.manualWhiteBalanceSupported)
        assertTrue(parsed.supportedAwbModes.isEmpty())
    }

    @Test
    fun projectionMapsAwbModesAndManualFlag() {
        val caps =
            NativeCapabilityProjection.project(
                snapshot(manualSupported = true, awbModes = listOf(1, 5, 8, 42))
            ).capabilities
        assertTrue(caps.manualWhiteBalanceSupported)
        assertEquals(
            setOf(WhiteBalanceMode.Auto, WhiteBalanceMode.Daylight, WhiteBalanceMode.Shade),
            caps.supportedWhiteBalanceModes
        )
    }

    @Test
    fun projectionAlwaysOffersAuto() {
        val caps = NativeCapabilityProjection.project(snapshot()).capabilities
        assertFalse(caps.manualWhiteBalanceSupported)
        assertEquals(setOf(WhiteBalanceMode.Auto), caps.supportedWhiteBalanceModes)
    }
}
