package com.rawr.camera.integration

import com.rawr.camera.settings.model.RawStreamFormat
import com.rawr.camera.settings.model.VendorKeyType
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNull
import kotlin.test.assertTrue

class CameraInventoryParserTest {
    private val camerasJson = """
        {"cameras":[
          {"id":"0","parentId":"","enumerated":true,"logical":true,"physicalIds":["4","3"],"facing":1,
           "focalLengths":[6.0],"sensorSizeMm":[9.8,7.4],"equivalentFocalMm":24.0,
           "rawStreams":[{"format":"RAW_SENSOR","width":4080,"height":3064,"supported":true},
                         {"format":"RAW12","width":4080,"height":3064,"supported":false}],
           "black":[64,64,64,64],"white":1023,"sessionKeys":[65536]},
          {"id":"4","parentId":"0","enumerated":false,"logical":false,"physicalIds":[],"facing":1,
           "focalLengths":[2.2],"sensorSizeMm":[6.4,4.8],"equivalentFocalMm":14.0,"rawStreams":[],
           "black":[],"white":0,"sessionKeys":[]},
          {"id":"1","parentId":"","enumerated":true,"logical":false,"physicalIds":[],"facing":0,
           "focalLengths":[3.0],"sensorSizeMm":[],"equivalentFocalMm":0,"rawStreams":[],
           "black":[],"white":0,"sessionKeys":[]},
          {"id":"5","parentId":"","enumerated":false,"logical":false,"physicalIds":[],"facing":1,
           "focalLengths":[15.0],"sensorSizeMm":[],"equivalentFocalMm":85.0,
           "rawStreams":[{"format":"RAW10","width":4000,"height":3000,"supported":true}],
           "black":[],"white":0,"sessionKeys":[]}
        ],"error":""}
    """.trimIndent()

    @Test
    fun camerasParseWithPhysicalSubCameras() {
        val inventory = CameraInventoryParser.parseCameras(camerasJson)
        assertNull(inventory.error)
        assertEquals(4, inventory.cameras.size)
        val logical = inventory.find("0", "")!!
        assertTrue(logical.logical && logical.enumerated)
        assertEquals(listOf("4", "3"), logical.physicalIds)
        assertEquals(RawStreamFormat.Raw16, logical.rawStreams[0].format)
        assertNull(logical.rawStreams[1].format)
        assertEquals(listOf(64, 64, 64, 64), logical.blackLevels)
        val physical = inventory.find("0", "4")!!
        assertEquals("0", physical.openId)
        assertEquals("4", physical.physicalId)
        assertNull(inventory.find("4", ""))
    }

    @Test
    fun groupsByFacingBackFirstSortedByFocalLength() {
        val groups = CameraInventoryParser.parseCameras(camerasJson).grouped()
        assertEquals(listOf(CameraFacing.Back, CameraFacing.Front), groups.map { it.first })
        assertEquals(listOf("4", "0", "5"), groups[0].second.map { it.id })
    }

    @Test
    fun fpsRangesParseAsPairsAndListFixedRates() {
        val json = """
            {"cameras":[{"id":"0","parentId":"","enumerated":true,"logical":false,"physicalIds":[],"facing":1,
             "focalLengths":[],"equivalentFocalMm":0,"rawStreams":[],"black":[],"white":0,
             "fpsRanges":[15,30,30,30,60,60,24,24,30,30,0,10,20,15,7],"sessionKeys":[]}],"error":""}
        """.trimIndent()
        val camera = CameraInventoryParser.parseCameras(json).cameras.single()
        // Invalid pairs (a zero minimum, max below min) and the odd trailing value are dropped.
        assertEquals(listOf(15..30, 30..30, 60..60, 24..24, 30..30), camera.fpsRanges)
        assertEquals(listOf(24, 30, 60), camera.fixedFrameRates)
        // Older probes without the field parse with no ranges.
        assertEquals(emptyList(), CameraInventoryParser.parseCameras(camerasJson).cameras.first().fpsRanges)
    }

    @Test
    fun malformedJsonReportsError() {
        val inventory = CameraInventoryParser.parseCameras("{")
        assertTrue(inventory.cameras.isEmpty())
        assertTrue(inventory.error!!.startsWith("Camera probe failed"))
    }

    @Test
    fun keysMergeNamesTypesAndSessionMembership() {
        val json = """
            {"tags":[{"tag":2147614720,"type":"int32","count":1,"values":[0]},
                     {"tag":65536,"type":"byte","count":1,"values":[1]},
                     {"tag":3000,"type":"rational","count":1,"values":[0.5]}],
             "names":[{"name":"vivo.control.forceSensorMode","tag":2147614720},
                      {"name":"android.control.mode","tag":65536},
                      {"name":"com.vendor.missing","tag":null}],
             "sessionKeys":[2147614720],"error":""}
        """.trimIndent()
        val catalog = CameraInventoryParser.parseKeys(
            json,
            listOf("vivo.control.forceSensorMode", "android.control.mode", "com.vendor.missing"),
            setOf("android.control.mode")
        )
        assertNull(catalog.error)
        assertEquals(
            listOf("android.control.mode", "com.vendor.missing", "vivo.control.forceSensorMode", null),
            catalog.keys.map { it.name }
        )
        val force = catalog.find("vivo.control.forceSensorMode")!!
        assertEquals(VendorKeyType.Int32, force.type)
        assertTrue(force.sessionKey)
        assertEquals(listOf(0.0), force.defaults)
        assertTrue(catalog.find("android.control.mode")!!.sessionKey)
        assertNull(catalog.find("com.vendor.missing")!!.type)
        val unnamed = catalog.find("0xbb8")!!
        assertNull(unnamed.type)
        assertEquals("rational", unnamed.unsupportedType)
        assertEquals("0xbb8", unnamed.reference)
        assertEquals(listOf("vivo.control.forceSensorMode"), catalog.search("forcesensor").map { it.name })
        assertEquals(1, catalog.search("0x8002").size)
    }

    @Test
    fun keyProbeErrorIsSurfaced() {
        val catalog = CameraInventoryParser.parseKeys("""{"error":"Camera 3 could not be opened"}""", emptyList(), emptySet())
        assertEquals("Camera 3 could not be opened", catalog.error)
    }
}
