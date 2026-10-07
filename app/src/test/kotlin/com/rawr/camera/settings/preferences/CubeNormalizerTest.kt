package com.rawr.camera.settings.preferences

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assert.assertThrows
import org.junit.Test

class CubeNormalizerTest {
    private fun table(size: Int) = List(size * size * size) { "0.0 0.5 1.0" }
    private fun normalize(vararg headers: String) =
        CubeNormalizer.normalize((listOf("LUT_3D_SIZE 2") + headers + table(2)).asSequence())

    @Test fun acceptsSupportedSizesAndHarmlessMetadata() {
        for (size in listOf(2, 16, 17, 33, 65)) {
            val out = CubeNormalizer.normalize((listOf("TITLE \"x\"", "# comment", "LUT_3D_SIZE $size") + table(size)).asSequence())
            assertEquals("LUT_3D_SIZE $size", out.first())
            assertEquals(3 + size * size * size, out.size)
        }
    }

    @Test fun preservesNonDefaultRangeAndMatchingDomains() {
        val out = normalize("LUT_3D_INPUT_RANGE -1 2", "DOMAIN_MIN -1 -1 -1")
        assertEquals("DOMAIN_MIN -1.0 -1.0 -1.0", out[1])
        assertEquals("DOMAIN_MAX 2.0 2.0 2.0", out[2])
        val perChannel = normalize("DOMAIN_MIN -1 -2 -3", "DOMAIN_MAX 2 3 4")
        assertEquals("DOMAIN_MIN -1.0 -2.0 -3.0", perChannel[1])
        assertEquals("DOMAIN_MAX 2.0 3.0 4.0", perChannel[2])
    }

    @Test fun normalizesSignedAndScientificRowsWithComments() {
        val out = CubeNormalizer.normalize((listOf("LUT_3D_SIZE 2 # size") + List(8) { "+0.0 .5 1e0 # row" }).asSequence())
        assertTrue(out.drop(3).all { it == "0.0 0.5 1.0" })
    }

    @Test fun rejectsUnsupportedAndConflictingHeaders() {
        for (headers in listOf(
            listOf("LUT_IN_VIDEO_RANGE 0 1"), listOf("UNKNOWN_SETTING 1"),
            listOf("LUT_1D_SIZE 16"), listOf("LUT_3D_SIZE 2"),
            listOf("DOMAIN_MIN 0 0"), listOf("DOMAIN_MAX NaN 1 1"),
            listOf("DOMAIN_MIN 1 0 0"), listOf("LUT_3D_INPUT_RANGE 2 -1"),
            listOf("LUT_3D_INPUT_RANGE -1 2", "DOMAIN_MAX 1 1 1"),
            listOf("DOMAIN_MIN 0 0 0", "DOMAIN_MIN 0 0 0")
        )) assertThrows(headers.toString(), IllegalArgumentException::class.java) { normalize(*headers.toTypedArray()) }
    }

    @Test fun rejectsMalformedNumbersAndRowCounts() {
        for (row in listOf("0 nope 1", "0 NaN 1", "0 1e999 1", "0 1", "0 1 2 3", "0x0 0 0")) {
            assertThrows(row, IllegalArgumentException::class.java) {
                CubeNormalizer.normalize((listOf("LUT_3D_SIZE 2") + List(8) { row }).asSequence())
            }
        }
        for (lines in listOf(listOf("LUT_3D_SIZE 1"), listOf("LUT_3D_SIZE 66"), table(2),
            listOf("LUT_3D_SIZE 2") + table(2).dropLast(1), listOf("LUT_3D_SIZE 2") + table(2) + table(2))) {
            assertThrows(IllegalArgumentException::class.java) { CubeNormalizer.normalize(lines.asSequence()) }
        }
    }
}
