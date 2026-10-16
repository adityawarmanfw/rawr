package com.rawr.camera.settings.model

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNull
import kotlin.test.assertTrue

class LensProfilesTest {
    private val lens = LensProfile(name = "35", cameraId = "3")

    @Test
    fun validLensHasNoProblems() {
        assertEquals(emptyList(), validateLensProfile(lens, emptyList()))
    }

    @Test
    fun nameIsRequiredShortAndUnique() {
        assertEquals(listOf("Name is required"), validateLensProfile(lens.copy(name = " "), emptyList()))
        assertEquals(listOf("Name must be at most 4 characters"), validateLensProfile(lens.copy(name = "12345"), emptyList()))
        assertEquals(
            listOf("Another lens is named Tele"),
            validateLensProfile(lens.copy(name = "Tele"), listOf(lens.copy(name = "tele")))
        )
    }

    @Test
    fun staticLevelsNeedWhiteAboveBlack() {
        val bad = lens.copy(levels = LensLevels(isStatic = true, blackRggb = listOf(64f, 64f, 64f, 64f), white = 64f))
        assertEquals(listOf("White level must be above the black levels"), validateLensProfile(bad, emptyList()))
        val dynamic = lens.copy(levels = LensLevels(isStatic = false, white = 0f))
        assertEquals(emptyList(), validateLensProfile(dynamic, emptyList()))
    }

    @Test
    fun vendorKeysCheckValuesAgainstType() {
        assertNull(validateVendorKey(VendorKey("a", values = listOf(31.0))))
        assertEquals("a: enter at least one value", validateVendorKey(VendorKey("a")))
        assertEquals("a: Int32 needs whole numbers", validateVendorKey(VendorKey("a", values = listOf(1.5))))
        assertEquals(
            "a: value out of Byte range",
            validateVendorKey(VendorKey("a", type = VendorKeyType.Byte, values = listOf(256.0)))
        )
        assertNull(validateVendorKey(VendorKey("a", type = VendorKeyType.Float, values = listOf(1.5))))
        val badKey = lens.copy(vendorKeys = listOf(VendorKey("")))
        assertEquals(listOf("Key name is required"), validateLensProfile(badKey, emptyList()))
    }

    @Test
    fun vendorKeyNamesTrimEdgesButRejectPastedTypeAndValue() {
        assertNull(validateVendorKey(VendorKey(" \tvendor.mode\n", values = listOf(7.0))))
        for (name in listOf("vendor.mode\nInt32\n7", "vendor. mode", "vendor.\tmode")) {
            assertEquals(
                "Key name cannot contain whitespace; enter type and value in their separate fields",
                validateVendorKey(VendorKey(name, values = listOf(7.0)))
            )
        }
    }

    @Test
    fun listNeedsAnEnabledLens() {
        val problems = validateLensProfiles(listOf(lens.copy(enabled = false)))
        assertTrue("At least one lens must be enabled" in problems)
    }

    @Test
    fun valuesParseAndFormat() {
        assertEquals(listOf(1.0, 2.0, 3.0), parseVendorKeyValues("1, 2 3"))
        assertNull(parseVendorKeyValues("1, x"))
        assertEquals(emptyList(), parseVendorKeyValues(" "))
        assertEquals("1, 2", formatVendorKeyValues(listOf(1.0, 2.0), VendorKeyType.Int32))
        assertEquals("1.5", formatVendorKeyValues(listOf(1.5), VendorKeyType.Float))
    }
}
