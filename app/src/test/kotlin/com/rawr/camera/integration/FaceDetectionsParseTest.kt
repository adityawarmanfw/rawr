package com.rawr.camera.integration

import com.rawr.camera.model.FaceDetection
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue

class FaceDetectionsParseTest {
    @Test
    fun eyeTargetAndFallbackParse() {
        val eye = NativeCameraUiSnapshot.parseFaces("[[0.1,0.2,0.3,0.4,80,1,0.2,0.3]]").single()
        assertTrue(eye.afTarget)
        assertEquals(.2f, eye.eyeX)
        assertEquals(.3f, eye.eyeY)
        for (tail in listOf("1,-1,-1", "1,NaN,0.3", "0,0.2,0.3", "1,0.2,2")) {
            val face = NativeCameraUiSnapshot.parseFaces("[[0.1,0.2,0.3,0.4,80,$tail]]").single()
            assertEquals(null, face.eyeX)
            assertEquals(null, face.eyeY)
        }
    }

    @Test
    fun emptyArrayParsesToEmpty() {
        assertEquals(emptyList(), NativeCameraUiSnapshot.parseFaces("[]"))
    }

    @Test
    fun singleFaceRoundTrips() {
        assertEquals(
            listOf(FaceDetection(.1f, .2f, .3f, .4f, 80)),
            NativeCameraUiSnapshot.parseFaces("[[0.1000,0.2000,0.3000,0.4000,80]]")
        )
    }

    @Test
    fun multipleFacesKeepOrder() {
        val parsed = NativeCameraUiSnapshot.parseFaces("[[0.1,0.2,0.3,0.4,90],[0.5,0.5,0.2,0.2,60]]")
        assertEquals(2, parsed.size)
        assertEquals(90, parsed[0].score)
        assertEquals(60, parsed[1].score)
    }

    @Test
    fun malformedInputParsesToEmpty() {
        assertTrue(NativeCameraUiSnapshot.parseFaces("").isEmpty())
        assertTrue(NativeCameraUiSnapshot.parseFaces("not json").isEmpty())
        assertTrue(NativeCameraUiSnapshot.parseFaces("[,,]").isEmpty())
    }

    @Test
    fun malformedEntriesAreSkipped() {
        // Second entry has a non-positive width; first survives.
        val parsed =
            NativeCameraUiSnapshot.parseFaces("[[0.1,0.2,0.3,0.4,70],[0.5,0.5,-0.2,0.2,60],[0.1,0.1]]")
        assertEquals(listOf(FaceDetection(.1f, .2f, .3f, .4f, 70)), parsed)
    }
}
