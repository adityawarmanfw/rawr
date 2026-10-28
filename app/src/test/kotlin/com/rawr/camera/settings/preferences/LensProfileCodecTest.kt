package com.rawr.camera.settings.preferences

import com.rawr.camera.settings.model.LensMirror
import com.rawr.camera.settings.model.LensOutputSize
import com.rawr.camera.settings.model.LensLevels
import com.rawr.camera.settings.model.LensDcp
import com.rawr.camera.settings.model.LensProfile
import com.rawr.camera.settings.model.RawStreamChoice
import com.rawr.camera.settings.model.RawStreamFormat
import com.rawr.camera.settings.model.VendorKey
import com.rawr.camera.settings.model.VendorKeyScope
import com.rawr.camera.settings.model.VendorKeyType
import com.rawr.camera.settings.model.needsJavaBackend
import com.rawr.camera.settings.model.validateLensProfile
import org.json.JSONObject
import kotlin.test.assertFailsWith
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue
import kotlin.test.assertNull

class LensProfileCodecTest {
    @Test
    fun companionOptOutSurvivesBackupsAndRejectsConflictingRecipes() {
        val lens = LensProfile("T50", "4", disableCompanion = true,
            captureStream = RawStreamChoice(RawStreamFormat.Raw16, 8160, 6144))
        val encoded = LensProfileCodec.encode(listOf(lens))
        assertEquals(listOf(lens), LensProfileCodec.decode(encoded))
        assertEquals(listOf(lens), LensProfileCodec.decodeBackup(encoded))
        val plain = LensProfile("T12", "4")
        assertTrue(!JSONObject(LensProfileCodec.encode(listOf(plain)))
            .getJSONArray("lenses").getJSONObject(0).has("disableCompanion"))
        assertTrue(!requireNotNull(LensProfileCodec.decode("""{"lenses":[{"id":"T12","cameraId":"4"}]}""")).single().disableCompanion)
        for (field in listOf(
            """"disableCompanion":"true"""",
            """"disableCompanion":1""",
            """"disableCompanion":null""",
            """"disableCompanion":true,"companion":{"width":640,"height":480}"""
        )) {
            assertFailsWith<IllegalArgumentException> {
                LensProfileCodec.decodeBackup("""{"lenses":[{"id":"T50","cameraId":"4",$field}]}""")
            }
        }
        assertFailsWith<IllegalArgumentException> {
            LensProfileCodec.encode(listOf(lens.copy(companion = LensOutputSize(640, 480))))
        }
    }

    @Test
    fun sessionPolicyFieldsRoundTripAndDefaultToOff() {
        val key = VendorKey("vendor.mode", VendorKeyScope.SessionAndRequest, VendorKeyType.Int32, listOf(3.0))
        val lens = LensProfile(
            "ISZ", "2",
            vendorKeys = listOf(key.copy(scope = VendorKeyScope.Request)),
            sessionParams = "none",
            requestDefaults = "minimal",
            vendorGate = "g",
            gateHold = "closed",
            quirks = listOf("noTapAf", "requireFpsRange")
        )
        val encoded = LensProfileCodec.encode(listOf(lens))
        assertEquals(listOf(lens), LensProfileCodec.decode(encoded))
        assertEquals(listOf(lens), LensProfileCodec.decodeBackup(encoded))
        val both = LensProfile("ISZ", "2", vendorKeys = listOf(key))
        val roundTrip = requireNotNull(LensProfileCodec.decodeBackup(LensProfileCodec.encode(listOf(both)))).single()
        assertEquals(VendorKeyScope.SessionAndRequest, roundTrip.vendorKeys.single().scope)
        val json = JSONObject(LensProfileCodec.encode(listOf(LensProfile("T12", "4")))).getJSONArray("lenses").getJSONObject(0)
        for (field in listOf("session", "request", "gateHold", "quirks")) assertTrue(!json.has(field), field)
        val plain = requireNotNull(LensProfileCodec.decode("""{"lenses":[{"id":"T12","cameraId":"4"}]}""")).single()
        assertEquals("cadence", plain.sessionParams)
        assertEquals("rawr", plain.requestDefaults)
        assertNull(plain.gateHold)
        assertEquals(emptyList(), plain.quirks)
        for (field in listOf(
            """"session":{"params":"sometimes"}""",
            """"request":{"defaults":"bare"}""",
            """"gateHold":"forever"""",
            """"quirks":["noTapAF"]""",
            """"quirks":[3]"""
        )) {
            assertFailsWith<IllegalArgumentException> {
                LensProfileCodec.decodeBackup("""{"lenses":[{"id":"T50","cameraId":"4",$field}]}""")
            }
        }
    }

    @Test
    fun companionAuxAndReaderTuningRoundTripAndDefaultToOff() {
        val tuned = LensProfile(
            "T50", "4",
            companionFormat = "private",
            companionTargeted = false,
            companionUsage = "0x100",
            companionMaxImages = 4,
            companionPhysical = false,
            companionFallback = "nearest",
            companionDetach = true,
            auxJpeg = LensOutputSize(8192, 6144),
            auxJpegUsage = "0x3",
            rawMaxImages = 12,
            readerProbe = true,
            readerDemote = true
        )
        val encoded = LensProfileCodec.encode(listOf(tuned))
        assertEquals(listOf(tuned), LensProfileCodec.decode(encoded))
        assertEquals(listOf(tuned), LensProfileCodec.decodeBackup(encoded))
        val companion = JSONObject(encoded).getJSONArray("lenses").getJSONObject(0).getJSONObject("companion")
        assertTrue(!companion.has("width"), "an automatic companion writes no size")
        val sized = LensProfile("T51", "4", companion = LensOutputSize(640, 480))
        assertEquals(listOf(sized), LensProfileCodec.decodeBackup(LensProfileCodec.encode(listOf(sized))))
        val plain = JSONObject(LensProfileCodec.encode(listOf(LensProfile("T12", "4")))).getJSONArray("lenses").getJSONObject(0)
        for (field in listOf("companion", "auxJpeg", "reader")) assertTrue(!plain.has(field), field)
        for (field in listOf(
            """"companion":{"fallback":"sideways"}""",
            """"companion":{"maxImages":1}""",
            """"companion":{"width":640}""",
            """"companion":{"usage":"cpu"}""",
            """"companion":{"physical":"no"}""",
            """"auxJpeg":{"width":640,"height":480,"usage":"cpu"}""",
            """"auxJpeg":{"usage":"0x3"}""",
            """"reader":{"maxImages":5}""",
            """"reader":{"probe":"yes"}"""
        )) {
            assertFailsWith<IllegalArgumentException>(field) {
                LensProfileCodec.decodeBackup("""{"lenses":[{"id":"T50","cameraId":"4",$field}]}""")
            }
        }
    }

    @Test
    fun tuningValidationRejectsConflictsAndBadValues() {
        val ok = LensProfile("A", "2")
        assertTrue(validateLensProfile(ok, emptyList()).isEmpty())
        assertTrue(validateLensProfile(ok.copy(companionFallback = "nearest"), emptyList()).isEmpty())
        assertTrue(validateLensProfile(
            ok.copy(companion = LensOutputSize(640, 480), companionFallback = "nearest"), emptyList()
        ).any { it.contains("automatic companion size") })
        assertTrue(validateLensProfile(ok.copy(companionFallback = "x"), emptyList()).any { it.contains("fallback") })
        assertTrue(validateLensProfile(ok.copy(companionMaxImages = 1), emptyList()).any { it.contains("2 to 16") })
        assertTrue(validateLensProfile(ok.copy(rawMaxImages = 5), emptyList()).any { it.contains("6 to 16") })
        assertTrue(validateLensProfile(ok.copy(rawMaxImages = 16, companionMaxImages = 16), emptyList()).isEmpty())
        assertTrue(validateLensProfile(ok.copy(readerUsage = "cpu"), emptyList()).any { it.contains("usage") })
        assertTrue(validateLensProfile(ok.copy(readerUsage = "0x100003", captureReaderUsage = "gpuDataBuffer"), emptyList()).isEmpty())
    }

    @Test
    fun backendOpModeAndStreamUseCasesRoundTripAndDefaultToOff() {
        val lens = LensProfile(
            "M50", "2",
            stream = RawStreamChoice(RawStreamFormat.Raw16, 8192, 6144, unlisted = true),
            captureStream = RawStreamChoice(RawStreamFormat.Raw16, 8192, 6144),
            companion = LensOutputSize(640, 480),
            auxJpeg = LensOutputSize(8192, 6144),
            backend = "java",
            opMode = 32769,
            rawStreamUseCase = 524547,
            captureStreamUseCase = 6,
            companionStreamUseCase = 3,
            auxJpegStreamUseCase = 2
        )
        val encoded = LensProfileCodec.encode(listOf(lens))
        assertEquals(listOf(lens), LensProfileCodec.decode(encoded))
        assertEquals(listOf(lens), LensProfileCodec.decodeBackup(encoded))
        val json = JSONObject(encoded).getJSONArray("lenses").getJSONObject(0)
        assertEquals(32769, json.getJSONObject("session").getInt("opMode"))
        assertEquals(524547, json.getJSONObject("stream").getInt("useCase"))
        assertTrue(lens.needsJavaBackend())
        val plain = JSONObject(LensProfileCodec.encode(listOf(LensProfile("T12", "4")))).getJSONArray("lenses").getJSONObject(0)
        for (field in listOf("backend", "session", "quirks")) assertTrue(!plain.has(field), field)
        assertTrue(!plain.getJSONObject("stream").has("useCase"))
        assertTrue(!LensProfile("T12", "4").needsJavaBackend())
        for (field in listOf(
            """"backend":"gpu"""",
            """"session":{"opMode":1}""",
            """"session":{"opMode":2}""",
            """"session":{"opMode":-3}""",
            """"stream":{"useCase":-1}""",
            """"companion":{"width":640,"height":480,"useCase":1.5}"""
        )) {
            assertFailsWith<IllegalArgumentException>(field) {
                LensProfileCodec.decodeBackup("""{"lenses":[{"id":"T50","cameraId":"4",$field}]}""")
            }
        }
        val legacy = """{"lenses":[{"id":"T50","cameraId":"4","streamUseCase":524547}]}"""
        assertFailsWith<IllegalArgumentException> { LensProfileCodec.decodeBackup(legacy) }
        assertEquals(emptyList(), LensProfileCodec.decode(legacy))  // the lenient reader drops the lens
    }

    @Test
    fun javaOnlyOptionsConflictWithAnExplicitNdkBackend() {
        val ok = LensProfile("A", "2")
        assertTrue(validateLensProfile(ok.copy(opMode = 32769), emptyList()).isEmpty())
        assertTrue(validateLensProfile(ok.copy(backend = "ndk"), emptyList()).isEmpty())
        for (lens in listOf(
            ok.copy(backend = "ndk", opMode = 32769),
            ok.copy(backend = "ndk", rawStreamUseCase = 6),
            ok.copy(backend = "ndk", auxJpegStreamUseCase = 2)
        )) {
            assertTrue(validateLensProfile(lens, emptyList()).any { it.contains("needs the Java backend") }, lens.toString())
        }
        assertTrue(validateLensProfile(ok.copy(backend = "java", opMode = 32769), emptyList()).isEmpty())
        assertTrue(validateLensProfile(ok.copy(opMode = 1), emptyList()).any { it.contains("Operating mode") })
        assertTrue(validateLensProfile(ok.copy(opMode = 2), emptyList()).any { it.contains("Operating mode") })
        assertTrue(validateLensProfile(ok.copy(backend = "gpu"), emptyList()).any { it.contains("Unknown backend") })
        assertTrue(validateLensProfile(ok.copy(captureStreamUseCase = -1), emptyList()).any { it.contains("negative") })
    }

    @Test
    fun sessionParamsNoneRejectsSessionKeysAndGateHoldNeedsAGate() {
        val request = VendorKey("vendor.mode", VendorKeyScope.Request, VendorKeyType.Int32, listOf(3.0))
        val none = LensProfile("ISZ", "2", sessionParams = "none", vendorKeys = listOf(request))
        assertTrue(validateLensProfile(none, emptyList()).isEmpty())
        for (scope in listOf(VendorKeyScope.Session, VendorKeyScope.SessionAndRequest)) {
            val problems = validateLensProfile(none.copy(vendorKeys = listOf(request.copy(scope = scope))), emptyList())
            assertTrue(problems.any { it.contains("cannot carry the session key") }, problems.toString())
        }
        assertTrue(validateLensProfile(
            none.copy(vendorKeys = listOf(request.copy(scope = VendorKeyScope.Session, enabled = false))), emptyList()
        ).isEmpty())
        assertTrue(validateLensProfile(LensProfile("A", "2", gateHold = "closed"), emptyList()).any { it.contains("needs a vendor gate") })
        assertTrue(validateLensProfile(LensProfile("A", "2", vendorGate = "g", gateHold = "closed"), emptyList()).isEmpty())
        assertTrue(validateLensProfile(LensProfile("A", "2", quirks = listOf("bogus")), emptyList()).any { it.contains("Unknown quirk") })
    }

    @Test
    fun fullSizeRecipeFieldsRoundTripAndDefaultToOff() {
        val lens = LensProfile(
            "T50", "4",
            stream = RawStreamChoice(RawStreamFormat.Raw16, 8160, 6144, unlisted = true),
            readerUsage = "0x100003",
            captureReaderUsage = "0x100003",
            stillTemplate = true,
            fixedFps = 24,
            companion = LensOutputSize(640, 480),
            companionFormat = "private",
            companionTargeted = false,
            startupDelayMs = 500,
            viewfinderLevels = LensLevels(isStatic = true, blackRggb = listOf(512f, 512f, 512f, 512f), white = 4095f),
            captureLevels = LensLevels(),
            stopRepeatingForStill = true,
            auxJpeg = LensOutputSize(8160, 6144)
        )
        val encoded = LensProfileCodec.encode(listOf(lens))
        assertEquals(listOf(lens), LensProfileCodec.decode(encoded))
        val json = JSONObject(encoded).getJSONArray("lenses").getJSONObject(0)
        assertEquals("still", json.getString("template"))
        assertEquals(24, json.getInt("fps"))
        assertEquals(480, json.getJSONObject("companion").getInt("height"))

        val preview = lens.copy(stillTemplate = false)
        assertEquals(listOf(preview), LensProfileCodec.decode(LensProfileCodec.encode(listOf(preview))))
        val plain = requireNotNull(LensProfileCodec.decode("""{"lenses":[{"id":"a","cameraId":"0"}]}""")).single()
        assertTrue(plain.stillTemplate == null && plain.fixedFps == 0)
        assertNull(plain.companion)
        assertNull(plain.auxJpeg)
        assertNull(plain.captureReaderUsage)
    }

    @Test
    fun mirrorRoundTripsForEveryLensAndDefaultsToNone() {
        val lenses = LensMirror.entries.map { LensProfile(it.wireName, "0", mirror = it) }
        val encoded = LensProfileCodec.encode(lenses)
        assertEquals(lenses, LensProfileCodec.decode(encoded))
        val json = JSONObject(encoded).getJSONArray("lenses")
        assertTrue(!json.getJSONObject(0).has("mirror"))
        for (i in 1 until json.length()) {
            assertEquals(lenses[i].mirror.wireName, json.getJSONObject(i).getString("mirror"))
        }
        for (value in listOf("", "none", "unknown")) {
            val input = """{"lenses":[{"id":"a","cameraId":"0","mirror":"$value"}]}"""
            assertEquals(LensMirror.None, requireNotNull(LensProfileCodec.decode(input)).single().mirror)
        }
    }

    @Test
    fun dcpRoundTripKeepsPortableReferenceAndResolvesNativePath() {
        val dcp = LensDcp("0123456789abcdef0123456789abcdef.dcp", "Daylight \"profile\"")
        val lens = LensProfile("35", "3", dcp = dcp)
        val stored = LensProfileCodec.encode(listOf(lens))
        assertEquals(listOf(lens), LensProfileCodec.decode(stored))
        assertTrue(!JSONObject(stored).getJSONArray("lenses").getJSONObject(0).getJSONObject("dcp").has("path"))
        val dir = java.io.File("profiles").absoluteFile
        val native = LensProfileCodec.encode(listOf(lens), dcpDir = dir)
        assertEquals(java.io.File(dir, dcp.file).absolutePath,
            JSONObject(native).getJSONArray("lenses").getJSONObject(0).getJSONObject("dcp").getString("path"))
        assertEquals(listOf(lens), LensProfileCodec.decode(native))
    }

    @Test
    fun unsafeDcpReferenceIsDroppedWithoutLosingLens() {
        for (file in listOf("../outside.dcp", "/outside.dcp", "file.dcp", "")) {
            val json = JSONObject().put("lenses", org.json.JSONArray().put(
                JSONObject().put("id", "35").put("cameraId", "3")
                    .put("dcp", JSONObject().put("file", file).put("path", "/untrusted/profile.dcp"))))
            val lens = requireNotNull(LensProfileCodec.decode(json.toString())).single()
            assertEquals("35", lens.name)
            assertNull(lens.dcp)
        }
    }

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
                vendorKeys =
                    listOf(
                        VendorKey("com.vivo.current_ui_module", VendorKeyScope.Still, VendorKeyType.Int32, listOf(74.0)),
                        VendorKey("com.vivo.current_ui_module", VendorKeyScope.Decision, VendorKeyType.Int32, listOf(74.0))
                    ),
                vendorGate = "vivoFullSize",
                captureStream = RawStreamChoice(RawStreamFormat.Raw16, 16320, 12288),
                captureBurst = 3,
                captureBurstSequential = true,
                cfaBlock = 4,
                cfaOrder = "GRBG",
                cfaLive = "fast",
                cfaPhoto = "standard"
            )
        val json = LensProfileCodec.encode(listOf(seamless))
        assertEquals(seamless, requireNotNull(LensProfileCodec.decode(json)).single())
        val lens = JSONObject(json).getJSONArray("lenses").getJSONObject(0)
        assertEquals("still", lens.getJSONArray("keys").getJSONObject(0).getString("scope"))
        assertEquals("decision", lens.getJSONArray("keys").getJSONObject(1).getString("scope"))
        assertEquals(16320, lens.getJSONObject("captureStream").getInt("width"))
        assertEquals(3, lens.getInt("captureBurst"))
        assertTrue(lens.getBoolean("captureBurstSequential"))
        assertEquals(4, lens.getJSONObject("cfa").getInt("block"))
        assertEquals("GRBG", lens.getJSONObject("cfa").getString("order"))
        assertEquals("fast", lens.getJSONObject("cfa").getString("live"))
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

    @Test fun backupRejectsMalformedNestedFieldsInsteadOfUsingDefaults() {
        val fields = listOf(
            """"dcp":{"file":"bad.dcp"}""",
            """"dcp":"not an object" """,
            """"keys":[{"values":[1]}]""",
            """"keys":[{"tag":"vendor.mode","type":"unknown","values":[1]}]""",
            """"keys":[{"tag":"vendor.mode","scope":"unknown","values":[1]}]""",
            """"keys":[{"tag":"vendor.mode","values":[]} ]""",
            """"keys":[{"tag":"vendor.mode","type":"byte","values":[256]}]""",
            """"keys":[{"tag":"vendor.mode","values":["1"]}]""",
            """"keys":"not an array" """,
            """"dcg":{"keys":[{"values":[1]}]}""",
            """"levels":{"black":[1,2,3]}""",
            """"stream":{"format":"unknown"}""",
            """"captureStream":{"width":0,"height":10}""",
            """"exposure":{"isoMax":-1}""",
            """"cfa":{"block":3}"""
        )
        for (field in fields) {
            assertFailsWith<IllegalArgumentException>(field) {
                LensProfileCodec.decodeBackup("""{"lenses":[{"id":"W","cameraId":"0",$field}]}""")
            }
        }
    }

    @Test fun strictBackupDecoderPreservesValidLegacyDcgAndDefaults() {
        val raw = """{"lenses":[{"id":"W","cameraId":"0","dcg":{"keys":[{"tag":"vendor.mode","values":[1]}],"levels":{"static":true,"black":[64],"white":1023}}}]}"""
        assertEquals(LensProfileCodec.decode(raw), LensProfileCodec.decodeBackup(raw))
        assertEquals(listOf(LensProfile("W", "0")),
            LensProfileCodec.decodeBackup("""{"lenses":[{"id":"W","cameraId":"0"}]}"""))
        assertEquals(lenses, LensProfileCodec.decodeBackup(LensProfileCodec.encode(lenses)))
    }

    @Test fun builtInLensExportsPassStrictValidation() {
        val directory = java.io.File("src/main/cpp/camera/profiles")
        val sources = directory.listFiles()!!.filter { it.extension == "json" }
        assertTrue(sources.isNotEmpty())
        sources.forEach { source ->
            val lenses = requireNotNull(LensProfileCodec.decode(source.readText()))
            assertEquals(lenses, LensProfileCodec.decodeBackup(LensProfileCodec.encode(lenses)), source.name)
        }
    }
}
