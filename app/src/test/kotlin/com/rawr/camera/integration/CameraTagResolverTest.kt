package com.rawr.camera.integration

import kotlin.test.Test
import kotlin.test.assertEquals
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34])
class CameraTagResolverTest {
    @Test fun cachesSuccessfulIdsPerCameraAndPreservesUnsignedVendorIds() {
        var calls = 0
        val resolver = CameraTagResolver { camera, _ ->
            calls++
            if (camera == "0") 0x80020001.toInt() else 0x90020001.toInt()
        }
        assertEquals(0x80020001L, resolver.resolve("0", "vendor.mode"))
        assertEquals(0x80020001L, resolver.resolve("0", "vendor.mode"))
        assertEquals(0x90020001L, resolver.resolve("1", "vendor.mode"))
        assertEquals(2, calls)
    }

    @Test fun retriesFailuresAndDoesNotReuseAnotherKeysId() {
        var calls = 0
        val resolver = CameraTagResolver { _, name ->
            calls++
            if (calls == 1) throw IllegalStateException("camera service unavailable")
            if (name == "vendor.mode") 42 else 43
        }
        assertEquals(-1L, resolver.resolve("0", "vendor.mode"))
        assertEquals(42L, resolver.resolve("0", "vendor.mode"))
        assertEquals(43L, resolver.resolve("0", "vendor.other"))
        assertEquals(3, calls)
    }
}
