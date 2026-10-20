package com.rawr.camera.settings.preferences

import com.rawr.camera.settings.model.LensLevels
import com.rawr.camera.settings.model.LensProfile
import com.rawr.camera.settings.model.RawStreamChoice
import com.rawr.camera.settings.model.RawStreamFormat
import com.rawr.camera.settings.model.VendorKey
import com.rawr.camera.settings.model.VendorKeyScope
import com.rawr.camera.settings.model.VendorKeyType
import org.json.JSONObject
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue
import kotlin.test.assertNull

class LensProfileCodecTest {
    private val force = VendorKey("vivo.control.forceSensorMode", VendorKeyScope.Session, VendorKeyType.Int32, listOf(31.0))
    private val lenses =
        listOf(
            LensProfile(
                name = "35",
                cameraId = "3",
                stream = RawStreamChoice(RawStreamFormat.Raw16, 4080, 3064),
                levels = LensLevels(true, List(4) { 1024f }, 8712f),
                vendorKeys = listOf(force.copy(values = listOf(17.0)))
            ),
            LensProfile(
                name = "Tl|\"",
                cameraId = "2",
                enabled = false,
                physicalCameraId = "5",
                stream = RawStreamChoice(RawStreamFormat.Raw10),
                levels = LensLevels(true, listOf(64f, 65f, 66f, 67f), 1023f),
                vendorKeys =
                    listOf(
                        force,
                        VendorKey("0x80020001", VendorKeyScope.Request, VendorKeyType.Float, listOf(1.5, 2.0), enabled = false)
                    )
            )
        )

    @Test
    fun keepsSeamlessLensFieldsThroughARoundTrip() {
        // vivo 50/200 MP: binned viewfinder + capture stream, still-only keys, burst limit.
        val seamless =
            LensProfile(
                name = "F85",
                cameraId = "5",
                stream = RawStreamChoice(RawStreamFormat.Raw16, 4080, 3072),
                vendorKeys = listOf(VendorKey("com.vivo.current_ui_module", VendorKeyScope.Still, VendorKeyType.Int32, listOf(74.0))),
                vendorGate = "vivoFullSize",
                captureStream = RawStreamChoice(RawStreamFormat.Raw16, 16320, 12288),
                captureBurst = 3,
                captureBurstSequential = true
            )
        val json = LensProfileCodec.encode(listOf(seamless))
        assertEquals(seamless, requireNotNull(LensProfileCodec.decode(json)).single())
        val lens = JSONObject(json).getJSONArray("lenses").getJSONObject(0)
        assertEquals("still", lens.getJSONArray("keys").getJSONObject(0).getString("scope"))
        assertEquals(16320, lens.getJSONObject("captureStream").getInt("width"))
        assertEquals(3, lens.getInt("captureBurst"))
        assertTrue(lens.getBoolean("captureBurstSequential"))
    }

    @Test
    fun keepsFullSizeLensFieldsThroughARoundTrip() {
        val full =
            LensProfile(
                name = "F85",
                cameraId = "5",
                stream = RawStreamChoice(RawStreamFormat.Raw16, 8160, 6144, unlisted = true),
                levels = LensLevels(true, List(4) { 64f }, 1023f),
                vendorKeys = listOf(force.copy(values = listOf(14.0)), force.copy(scope = VendorKeyScope.Request, values = listOf(14.0))),
                readerUsage = "gpuDataBuffer",
                vendorGate = "vivoFullSize"
            )
        val decoded = requireNotNull(LensProfileCodec.decode(LensProfileCodec.encode(listOf(full)))).single()
        assertEquals(full, decoded)
        // Absent fields stay off, so binned lenses keep the app's reader choice and no gate.
        val plain = requireNotNull(LensProfileCodec.decode("""{"lenses":[{"id":"L","cameraId":"3"}]}""")).single()
        assertEquals(false, plain.stream.unlisted)
        assertNull(plain.readerUsage)
        assertEquals("", plain.vendorGate)
        val json = JSONObject(LensProfileCodec.encode(listOf(plain))).getJSONArray("lenses").getJSONObject(0)
        assertEquals(false, json.has("readerUsage") || json.has("vendorGate") || json.getJSONObject("stream").has("unlisted"))
    }

    @Test
    fun trimsLoadedKeyNamesWithoutDiscardingMalformedEntries() {
        val json = """{"lenses":[{"id":"DCG","cameraId":"0","keys":[
            {"tag":" \tvendor.mode\n","values":[7]},
            {"tag":"vendor.mode\nInt32\n7","values":[7]}]}]}"""
        val keys = requireNotNull(LensProfileCodec.decode(json)).single().vendorKeys
        assertEquals("vendor.mode", keys[0].tag)
        // Retain the bad entry so the editor can show its validation error and repair it.
        assertEquals("vendor.mode\nInt32\n7", keys[1].tag)
    }

    @Test
    fun roundTripKeepsEveryField() {
        assertEquals(lenses, LensProfileCodec.decode(LensProfileCodec.encode(lenses)))
    }

    @Test
    fun cameraPayloadCarriesOnlyEnabledLensesInOrder() {
        val json = JSONObject(LensProfileCodec.encode(lenses, enabledOnly = true))
        assertEquals("user", json.getString("id"))
        assertEquals(1, json.getJSONArray("lenses").length())
        assertEquals("35", json.getJSONArray("lenses").getJSONObject(0).getString("id"))
    }

    // Shape written by the native serializeCameraProfile (built-in profiles).
    @Test
    fun decodesNativeBuiltInProfile() {
        val native =
            """{"id":"v2562","lenses":[{"id":"170","cameraId":"5","physicalCameraId":"",""" +
                """"stream":{"format":"RAW16","width":4080,"height":3072},""" +
                """"levels":{"static":true,"black":[64,64,64,64],"white":1023},""" +
                """"keys":[{"tag":"vivo.control.forceSensorMode","type":"int32","scope":"session","values":[31],"enabled":true}]}]}"""
        val decoded = LensProfileCodec.decode(native)!!.single()
        assertEquals(
            LensProfile(
                name = "170",
                cameraId = "5",
                stream = RawStreamChoice(RawStreamFormat.Raw16, 4080, 3072),
                levels = LensLevels(true, List(4) { 64f }, 1023f),
                vendorKeys = listOf(force)
            ),
            decoded
        )
    }

    // Early builds stored DCG keys/levels in a separate block.
    @Test
    fun legacyDcgBlockFoldsIntoLensKeysAndLevels() {
        val legacy =
            """{"lenses":[{"id":"35","cameraId":"3","levels":{"static":false,"black":[0,0,0,0],"white":0},"keys":[],""" +
                """"dcg":{"keys":[{"tag":"vivo.control.forceSensorMode","type":"int32","scope":"session","values":[17]}],""" +
                """"levels":{"static":true,"black":[1024,1024,1024,1024],"white":8712}}}]}"""
        val lens = LensProfileCodec.decode(legacy)!!.single()
        assertEquals(listOf(force.copy(values = listOf(17.0))), lens.vendorKeys)
        assertEquals(LensLevels(true, List(4) { 1024f }, 8712f), lens.levels)
    }

    @Test
    fun unreadableInputIsLenient() {
        assertNull(LensProfileCodec.decode(null))
        assertNull(LensProfileCodec.decode("not json"))
        val partial = """{"lenses":[{"id":"ok","cameraId":"0","keys":[{"no_tag":1},{"tag":"a","values":[1]}]},{"cameraId":"1"}]}"""
        val decoded = LensProfileCodec.decode(partial)!!
        assertEquals(listOf("ok"), decoded.map { it.name })
        assertEquals(listOf("a"), decoded.single().vendorKeys.map { it.tag })
    }
}
