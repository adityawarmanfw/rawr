package com.rawr.camera.integration

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNotNull

class ProfileErrorSnapshotTest {
    // The smallest control state the parser accepts, plus the field under test.
    private fun parse(extra: String): NativeCameraUiSnapshot? = NativeCameraUiSnapshot.parse(
        """{"generation":1,"cameraId":"2","lensId":"L1","sensitivityMin":50,"sensitivityMax":6400,""" +
            """"exposureTimeMinNs":1000,"exposureTimeMaxNs":1000000000$extra}"""
    )

    @Test
    fun aRefusedProfileReachesTheSnapshot() {
        val refused = assertNotNull(parse(""","profileError":"invalid \"opMode\" for lens X""""))
        assertEquals("invalid \"opMode\" for lens X", refused.profileError)
    }

    @Test
    fun noErrorMeansAnEmptyString() {
        assertEquals("", assertNotNull(parse("")).profileError)
        assertEquals("", assertNotNull(parse(""","profileError":""""")).profileError)
    }
}
