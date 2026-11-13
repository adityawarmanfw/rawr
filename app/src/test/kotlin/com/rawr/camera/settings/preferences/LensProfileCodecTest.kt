package com.rawr.camera.settings.preferences

import com.rawr.camera.settings.model.AuxJpegSettings
import com.rawr.camera.settings.model.PhotoStreamSettings
import com.rawr.camera.settings.model.CompanionMode
import com.rawr.camera.settings.model.LensSensorLayout
import com.rawr.camera.settings.model.LensDcp
import com.rawr.camera.settings.model.LensLevels
import com.rawr.camera.settings.model.LensMirror
import com.rawr.camera.settings.model.LensOutputSize
import com.rawr.camera.settings.model.LensProfile
import com.rawr.camera.settings.model.RawStreamChoice
import com.rawr.camera.settings.model.RawStreamFormat
import com.rawr.camera.settings.model.BufferSettings
import com.rawr.camera.settings.model.VendorKey
import com.rawr.camera.settings.model.VendorKeyScope
import com.rawr.camera.settings.model.VendorKeyType
import com.rawr.camera.settings.model.StillSequence
import com.rawr.camera.settings.model.needsJavaCameraApi
import com.rawr.camera.settings.model.stillSequence
import com.rawr.camera.settings.model.validateLensProfile
import org.json.JSONObject
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertNull
import kotlin.test.assertTrue

class LensProfileCodecTest {
    private fun lensJson(encoded: String): JSONObject = JSONObject(encoded).getJSONArray("lenses").getJSONObject(0)
    private fun roundTrips(vararg lens: LensProfile) {
        val encoded = LensProfileCodec.encode(lens.toList())
        assertEquals(lens.toList(), LensProfileCodec.decode(encoded))
        assertEquals(lens.toList(), LensProfileCodec.decodeBackup(encoded))
    }
    private fun rejectedV1(field: String) = assertFailsWith<IllegalArgumentException>(field) {
        LensProfileCodec.decodeBackup("""{"lenses":[{"id":"T50","cameraId":"4",$field}]}""")
    }
    private fun rejectedV2(field: String) = assertFailsWith<IllegalArgumentException>(field) {
        LensProfileCodec.decodeBackup("""{"schema":2,"lenses":[{"id":"T50","camera":{"id":"4"},$field}]}""")
    }
    private val photo8160 = PhotoStreamSettings(RawStreamChoice(RawStreamFormat.Raw16, 8160, 6144))

    @Test
    fun writesSchemaTwoGroupedBySection() {
        val root = JSONObject(LensProfileCodec.encode(listOf(LensProfile("T12", "4", "2"))))
        assertEquals(2, root.getInt("schema"))
        val lens = root.getJSONArray("lenses").getJSONObject(0)
        assertEquals("4", lens.getJSONObject("camera").getString("id"))
        assertEquals("2", lens.getJSONObject("camera").getString("physical"))
        assertTrue(lens.getJSONObject("streams").has("raw"))
        assertTrue(lens.getJSONObject("image").has("levels"))
        for (legacy in listOf("cameraId", "stream", "levels", "readerUsage", "vendorGate", "disableCompanion"))
            assertTrue(!lens.has(legacy), legacy)
        // Defaults write nothing.
        for (field in listOf("session", "request")) assertTrue(!lens.has(field), field)
        for (field in listOf("photo", "companion", "auxJpeg")) assertTrue(!lens.getJSONObject("streams").has(field), field)
    }

    @Test
    fun schemaOneAndSchemaTwoDecodeToTheSameLens() {
        val v1 = """{"lenses":[{"id":"T","cameraId":"0","physicalCameraId":"4",
            "stream":{"format":"RAW10","width":4096,"height":3072,"highResLive":true,"useCase":1},
            "readerUsage":"gpuDataBuffer","reader":{"maxImages":8,"probe":true},
            "captureStream":{"format":"RAW16","width":8192,"height":6144,"useCase":2},
            "captureReaderUsage":"0x3","captureBurst":3,"captureBurstSequential":true,
            "companion":{"width":640,"height":480,"format":"private","targeted":false,"maxImages":4,"useCase":4},
            "auxJpeg":{"width":4096,"height":3072,"usage":"none","useCase":5},
            "backend":"java","session":{"params":"full","opMode":32769},"template":"still","fps":30,"startupDelayMs":200,
            "quirks":["noTapAf"],"request":{"defaults":"minimal"},
            "keys":[{"tag":"x.y","type":"int32","scope":"sessionAndRequest","values":[1],"enabled":true}],
            "levels":{"static":true,"black":[64,64,64,64],"white":1023},
            "cfa":{"block":2,"order":"GRBG","live":"fast","photo":"standard"},"mirror":"horizontal",
            "exposure":{"extended":true,"isoMax":12800,"shutterMaxNs":2000000000}}]}"""
        val fromV1 = LensProfileCodec.decodeBackup(v1)
        assertEquals(fromV1, LensProfileCodec.decodeBackup(LensProfileCodec.encode(fromV1)))
        val lens = fromV1.single()
        assertEquals(CompanionMode.Fixed, lens.streams.companion.mode)
        assertEquals(3, lens.streams.photo?.mergeFrameLimit)
        assertEquals("0x3", lens.streams.photo?.bufferUsage)
        assertEquals(BufferSettings("gpuDataBuffer", 8, testFirst = true), lens.streams.raw.buffers)
        assertEquals(true, lens.session.stillTemplate)
        assertEquals(LensSensorLayout(2, "GRBG", "fast", "standard"), lens.image.sensorLayout)
        // An unknown schema is refused rather than read as something it isn't.
        assertNull(LensProfileCodec.decode("""{"schema":3,"lenses":[{"id":"T","camera":{"id":"0"}}]}"""))
        assertFailsWith<IllegalArgumentException> {
            LensProfileCodec.decodeBackup("""{"schema":3,"lenses":[{"id":"T","camera":{"id":"0"}}]}""")
        }
    }

    @Test
    fun companionModesRoundTripAndRejectConflicts() {
        val off = LensProfile("T50", "4").copy(
            streams = LensProfile("T50", "4").streams.copy(photo = photo8160)
        ).companion { copy(mode = CompanionMode.Off) }
        roundTrips(off)
        assertEquals("off", lensJson(LensProfileCodec.encode(listOf(off))).getJSONObject("streams").getJSONObject("companion").getString("mode"))
        assertEquals(CompanionMode.Auto,
            requireNotNull(LensProfileCodec.decode("""{"lenses":[{"id":"T12","cameraId":"4"}]}""")).single().streams.companion.mode)
        // Schema 1 opt-out: a boolean, and never together with a required size.
        assertEquals(CompanionMode.Off, LensProfileCodec.decodeBackup(
            """{"lenses":[{"id":"T50","cameraId":"4","disableCompanion":true}]}""").single().streams.companion.mode)
        for (field in listOf(
            """"disableCompanion":"true"""",
            """"disableCompanion":1""",
            """"disableCompanion":null""",
            """"disableCompanion":true,"companion":{"width":640,"height":480}"""
        )) rejectedV1(field)
        rejectedV2(""""streams":{"companion":{"mode":"sometimes"}}""")
        rejectedV2(""""streams":{"companion":{"mode":"fixed"}}""")
        rejectedV2(""""streams":{"companion":{"mode":"auto","width":640,"height":480}}""")
    }

    @Test
    fun sessionPolicyFieldsRoundTripAndDefaultToOff() {
        val key = VendorKey("vendor.mode", VendorKeyScope.SessionAndRequest, VendorKeyType.Int32, listOf(3.0))
        val lens = LensProfile("ISZ", "2").copy(vendorKeys = listOf(key.copy(scope = VendorKeyScope.Request)))
            .session { copy(params = "none", gate = "g", gateHold = "closed", workarounds = listOf("noTapFocus", "alwaysSendFrameRate")) }
            .let { it.copy(request = it.request.copy(defaults = "minimal")) }
        roundTrips(lens)
        val both = LensProfile("ISZ", "2").copy(vendorKeys = listOf(key))
        assertEquals(VendorKeyScope.SessionAndRequest,
            LensProfileCodec.decodeBackup(LensProfileCodec.encode(listOf(both))).single().vendorKeys.single().scope)
        val plain = requireNotNull(LensProfileCodec.decode("""{"lenses":[{"id":"T12","cameraId":"4"}]}""")).single()
        assertEquals("cadence", plain.session.params)
        assertEquals("rawr", plain.request.defaults)
        assertNull(plain.session.gateHold)
        assertEquals(emptyList(), plain.session.workarounds)
        for (field in listOf(
            """"session":{"params":"sometimes"}""",
            """"request":{"defaults":"bare"}""",
            """"gateHold":"forever"""",
            """"quirks":["noTapAF"]""",
            """"quirks":[3]"""
        )) rejectedV1(field)
        rejectedV2(""""session":{"gate":{"hold":"forever"}}""")
    }

    @Test
    fun companionAuxAndReaderTuningRoundTripAndDefaultToOff() {
        val tuned = LensProfile("T50", "4")
            .companion {
                copy(format = "private", targeted = false, bufferUsage = "0x100", bufferCount = 4, physical = false,
                    fallback = "nearest", detach = true)
            }
            .streams { copy(auxJpeg = AuxJpegSettings(LensOutputSize(8192, 6144), bufferUsage = "0x3")) }
            .raw { copy(buffers = BufferSettings(count = 12, testFirst = true, fallBack = true)) }
        roundTrips(tuned)
        val companion = lensJson(LensProfileCodec.encode(listOf(tuned))).getJSONObject("streams").getJSONObject("companion")
        assertTrue(!companion.has("width"), "an automatic companion writes no size")
        roundTrips(LensProfile("T51", "4").companion { copy(mode = CompanionMode.Fixed, size = LensOutputSize(640, 480)) })
        for (field in listOf(
            """"companion":{"fallback":"sideways"}""",
            """"companion":{"maxImages":1}""",
            """"companion":{"width":640}""",
            """"companion":{"usage":"cpu"}""",
            """"companion":{"physical":"no"}""",
            """"auxJpeg":{"width":640,"height":480,"usage":"cpu"}""",
            """"auxJpeg":{"usage":"0x3"}""",
            """"reader":{"maxImages":1}""",
            """"reader":{"probe":"yes"}"""
        )) rejectedV1(field)
    }

    @Test
    fun tuningValidationRejectsConflictsAndBadValues() {
        val ok = LensProfile("A", "2")
        assertTrue(validateLensProfile(ok, emptyList()).isEmpty())
        assertTrue(validateLensProfile(ok.companion { copy(fallback = "nearest") }, emptyList()).isEmpty())
        assertTrue(validateLensProfile(
            ok.companion { copy(mode = CompanionMode.Fixed, fallback = "nearest") }, emptyList()
        ).any { it.contains("automatic companion size") })
        assertTrue(validateLensProfile(ok.companion { copy(fallback = "x") }, emptyList()).any { it.contains("fallback") })
        assertTrue(validateLensProfile(ok.companion { copy(bufferCount = 1) }, emptyList()).any { it.contains("2 to 16") })
        assertTrue(validateLensProfile(ok.raw { copy(buffers = buffers.copy(count = 1)) }, emptyList()).any { it.contains("2 to 16") })
        assertTrue(validateLensProfile(ok.raw { copy(buffers = buffers.copy(count = 4)) }, emptyList()).isEmpty())
        assertTrue(validateLensProfile(
            ok.raw { copy(buffers = buffers.copy(count = 16)) }.companion { copy(bufferCount = 16) }, emptyList()).isEmpty())
        assertTrue(validateLensProfile(ok.raw { copy(buffers = buffers.copy(usage = "cpu")) }, emptyList()).any { it.contains("usage") })
        assertTrue(validateLensProfile(
            ok.raw { copy(buffers = buffers.copy(usage = "0x100003")) }
                .streams { copy(photo = photo8160.copy(bufferUsage = "gpuDataBuffer")) }, emptyList()).isEmpty())
        assertTrue(validateLensProfile(
            ok.companion { copy(mode = CompanionMode.Fixed, size = LensOutputSize(0, 480)) }, emptyList()
        ).any { it.contains("Companion width") })
    }

    @Test
    fun backendOpModeAndStreamUseCasesRoundTripAndDefaultToOff() {
        val lens = LensProfile("M50", "2")
            .raw { copy(choice = RawStreamChoice(RawStreamFormat.Raw16, 8192, 6144, highResLive = true), useCase = 524547) }
            .streams {
                copy(
                    photo = PhotoStreamSettings(RawStreamChoice(RawStreamFormat.Raw16, 8192, 6144), useCase = 6),
                    auxJpeg = AuxJpegSettings(LensOutputSize(8192, 6144), useCase = 2)
                )
            }
            .companion { copy(mode = CompanionMode.Fixed, size = LensOutputSize(640, 480), useCase = 3) }
            .session { copy(cameraApi = "java", opMode = 32769) }
        roundTrips(lens)
        val json = lensJson(LensProfileCodec.encode(listOf(lens)))
        assertEquals(32769, json.getJSONObject("session").getInt("opMode"))
        assertEquals(524547, json.getJSONObject("streams").getJSONObject("raw").getInt("useCase"))
        assertTrue(lens.needsJavaCameraApi())
        val plain = lensJson(LensProfileCodec.encode(listOf(LensProfile("T12", "4"))))
        assertTrue(!plain.has("session"))
        assertTrue(!plain.getJSONObject("streams").getJSONObject("raw").has("useCase"))
        assertTrue(!LensProfile("T12", "4").needsJavaCameraApi())
        for (field in listOf(
            """"backend":"gpu"""",
            """"session":{"opMode":1}""",
            """"session":{"opMode":2}""",
            """"session":{"opMode":-3}""",
            """"stream":{"useCase":-1}""",
            """"companion":{"width":640,"height":480,"useCase":1.5}"""
        )) rejectedV1(field)
        val legacy = """{"lenses":[{"id":"T50","cameraId":"4","streamUseCase":524547}]}"""
        assertFailsWith<IllegalArgumentException> { LensProfileCodec.decodeBackup(legacy) }
        assertNull(LensProfileCodec.decode(legacy))  // the lenient reader drops the lens; nothing left means defaults
    }

    @Test
    fun javaOnlyOptionsConflictWithAnExplicitNativeCameraApi() {
        val ok = LensProfile("A", "2")
        assertTrue(validateLensProfile(ok.session { copy(opMode = 32769) }, emptyList()).isEmpty())
        assertTrue(validateLensProfile(ok.session { copy(cameraApi = "native") }, emptyList()).isEmpty())
        for (lens in listOf(
            ok.session { copy(cameraApi = "native", opMode = 32769) },
            ok.session { copy(cameraApi = "native") }.raw { copy(useCase = 6) },
            ok.session { copy(cameraApi = "native") }.streams { copy(auxJpeg = AuxJpegSettings(LensOutputSize(640, 480), useCase = 2)) }
        )) {
            assertTrue(validateLensProfile(lens, emptyList()).any { it.contains("needs Java Camera2") }, lens.toString())
        }
        assertTrue(validateLensProfile(ok.session { copy(cameraApi = "java", opMode = 32769) }, emptyList()).isEmpty())
        assertTrue(validateLensProfile(ok.session { copy(opMode = 1) }, emptyList()).any { it.contains("Operating mode") })
        assertTrue(validateLensProfile(ok.session { copy(opMode = 2) }, emptyList()).any { it.contains("Operating mode") })
        assertTrue(validateLensProfile(ok.session { copy(cameraApi = "gpu") }, emptyList()).any { it.contains("Unknown camera API") })
        assertTrue(validateLensProfile(
            ok.streams { copy(photo = photo8160.copy(useCase = -1)) }, emptyList()).any { it.contains("negative") })
        // Stopping the viewfinder for photos stays on the native API, whether Java is explicit or derived; the vivo gate runs on either API.
        assertTrue(validateLensProfile(ok.session { copy(cameraApi = "java", gate = "vivoFullSize") }, emptyList())
            .none { it.contains("vendor gate") })
        assertTrue(validateLensProfile(ok.session { copy(gate = "vivoFullSize") }.raw { copy(useCase = 1) }, emptyList())
            .none { it.contains("vendor gate") })
        assertTrue(validateLensProfile(ok.session { copy(cameraApi = "java", stopViewfinderForPhotos = true) }, emptyList())
            .any { it.contains("stop the viewfinder stream") })
        assertTrue(validateLensProfile(ok.session { copy(gate = "vivoFullSize") }, emptyList()).none { it.contains("Java backend") })
    }

    @Test
    fun sessionParamsNoneRejectsSessionKeysAndGateHoldNeedsAGate() {
        val request = VendorKey("vendor.mode", VendorKeyScope.Request, VendorKeyType.Int32, listOf(3.0))
        val none = LensProfile("ISZ", "2").session { copy(params = "none") }.copy(vendorKeys = listOf(request))
        assertTrue(validateLensProfile(none, emptyList()).isEmpty())
        for (scope in listOf(VendorKeyScope.Session, VendorKeyScope.SessionAndRequest)) {
            val problems = validateLensProfile(none.copy(vendorKeys = listOf(request.copy(scope = scope))), emptyList())
            assertTrue(problems.any { it.contains("cannot carry the session key") }, problems.toString())
        }
        assertTrue(validateLensProfile(
            none.copy(vendorKeys = listOf(request.copy(scope = VendorKeyScope.Session, enabled = false))), emptyList()
        ).isEmpty())
        val a = LensProfile("A", "2")
        assertTrue(validateLensProfile(a.session { copy(gateHold = "closed") }, emptyList()).any { it.contains("needs a vendor gate") })
        assertTrue(validateLensProfile(a.session { copy(gate = "g", gateHold = "closed") }, emptyList()).isEmpty())
        assertTrue(validateLensProfile(a.session { copy(workarounds = listOf("bogus")) }, emptyList()).any { it.contains("Unknown workaround") })
    }

    @Test
    fun fullSizeRecipeFieldsRoundTripAndDefaultToOff() {
        val lens = LensProfile("T50", "4")
            .raw {
                copy(
                    choice = RawStreamChoice(RawStreamFormat.Raw16, 8160, 6144, highResLive = true),
                    buffers = BufferSettings(usage = "0x100003"),
                    levels = LensLevels(isStatic = true, blackRggb = listOf(512f, 512f, 512f, 512f), white = 4095f)
                )
            }
            .streams { copy(photo = photo8160.copy(bufferUsage = "0x100003", levels = LensLevels()),
                auxJpeg = AuxJpegSettings(LensOutputSize(8160, 6144))) }
            .companion { copy(mode = CompanionMode.Fixed, size = LensOutputSize(640, 480), format = "private", targeted = false) }
            .session { copy(stillTemplate = true, frameRate = 24, startupDelayMs = 500, stopViewfinderForPhotos = true) }
        roundTrips(lens)
        val json = lensJson(LensProfileCodec.encode(listOf(lens)))
        assertEquals("still", json.getJSONObject("session").getString("template"))
        assertEquals(24, json.getJSONObject("session").getInt("frameRate"))
        assertEquals(480, json.getJSONObject("streams").getJSONObject("companion").getInt("height"))
        val preview = lens.session { copy(stillTemplate = false) }
        assertEquals(listOf(preview), LensProfileCodec.decode(LensProfileCodec.encode(listOf(preview))))
        val plain = requireNotNull(LensProfileCodec.decode("""{"lenses":[{"id":"a","cameraId":"0"}]}""")).single()
        assertTrue(plain.session.stillTemplate == null && plain.session.frameRate == 0)
        assertEquals(CompanionMode.Auto, plain.streams.companion.mode)
        assertNull(plain.streams.auxJpeg)
        assertNull(plain.streams.photo)
    }

    @Test
    fun schemaOneCaptureFieldsWithoutACaptureStreamAreDropped() {
        // They only ever applied to the capture stream.
        val lens = LensProfileCodec.decodeBackup(
            """{"lenses":[{"id":"a","cameraId":"0","captureReaderUsage":"0x3","captureBurst":3,"captureLevels":{"static":false}}]}"""
        ).single()
        assertNull(lens.streams.photo)
        assertEquals(listOf(lens), LensProfileCodec.decodeBackup(LensProfileCodec.encode(listOf(lens))))
    }

    @Test
    fun mirrorRoundTripsForEveryLensAndDefaultsToNone() {
        val lenses = LensMirror.entries.map { LensProfile(it.wireName, "0").image { copy(mirror = it) } }
        val encoded = LensProfileCodec.encode(lenses)
        assertEquals(lenses, LensProfileCodec.decode(encoded))
        val json = JSONObject(encoded).getJSONArray("lenses")
        assertTrue(!json.getJSONObject(0).getJSONObject("image").has("mirror"))
        for (i in 1 until json.length()) {
            assertEquals(lenses[i].image.mirror.wireName, json.getJSONObject(i).getJSONObject("image").getString("mirror"))
        }
        for (value in listOf("", "none", "unknown")) {
            val input = """{"lenses":[{"id":"a","cameraId":"0","mirror":"$value"}]}"""
            assertEquals(LensMirror.None, requireNotNull(LensProfileCodec.decode(input)).single().image.mirror)
        }
    }

    @Test
    fun dcpRoundTripKeepsPortableReferenceAndResolvesNativePath() {
        val dcp = LensDcp("0123456789abcdef0123456789abcdef.dcp", "Daylight \"profile\"")
        val lens = LensProfile("35", "3").image { copy(dcp = dcp) }
        val stored = LensProfileCodec.encode(listOf(lens))
        assertEquals(listOf(lens), LensProfileCodec.decode(stored))
        assertTrue(!lensJson(stored).getJSONObject("image").getJSONObject("dcp").has("path"))
        val dir = java.io.File("profiles").absoluteFile
        val native = LensProfileCodec.encode(listOf(lens), dcpDir = dir)
        assertEquals(java.io.File(dir, dcp.file).absolutePath,
            lensJson(native).getJSONObject("image").getJSONObject("dcp").getString("path"))
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
            assertNull(lens.image.dcp)
        }
    }

    private val force = VendorKey("vivo.control.forceSensorMode", VendorKeyScope.Session, VendorKeyType.Int32, listOf(31.0))
    private val lenses =
        listOf(
            LensProfile("35", "3")
                .raw { copy(choice = RawStreamChoice(RawStreamFormat.Raw16, 4080, 3064)) }
                .image { copy(levels = LensLevels(true, List(4) { 1024f }, 8712f)) }
                .copy(vendorKeys = listOf(force.copy(values = listOf(17.0)))),
            LensProfile("Tl|\"", "2", "5")
                .copy(enabled = false)
                .raw { copy(choice = RawStreamChoice(RawStreamFormat.Raw10)) }
                .image { copy(levels = LensLevels(true, listOf(64f, 65f, 66f, 67f), 1023f)) }
                .copy(vendorKeys = listOf(force,
                    VendorKey("0x80020001", VendorKeyScope.Request, VendorKeyType.Float, listOf(1.5, 2.0), enabled = false)))
        )

    @Test
    fun keepsSeamlessLensFieldsThroughARoundTrip() {
        // vivo 50/200 MP: binned viewfinder + capture stream, still-only keys, burst limit.
        val seamless = LensProfile("F85", "5")
            .raw { copy(choice = RawStreamChoice(RawStreamFormat.Raw16, 4080, 3072)) }
            .copy(vendorKeys = listOf(
                VendorKey("com.vivo.current_ui_module", VendorKeyScope.Still, VendorKeyType.Int32, listOf(74.0)),
                VendorKey("com.vivo.current_ui_module", VendorKeyScope.Decision, VendorKeyType.Int32, listOf(74.0))
            ))
            .session { copy(gate = "vivoFullSize") }
            .streams { copy(photo = PhotoStreamSettings(RawStreamChoice(RawStreamFormat.Raw16, 16320, 12288),
                mergeFrameLimit = 3, mergeFramesOneByOne = true)) }
            .image { copy(sensorLayout = LensSensorLayout(4, "GRBG", "fast", "standard")) }
        val json = LensProfileCodec.encode(listOf(seamless))
        assertEquals(seamless, requireNotNull(LensProfileCodec.decode(json)).single())
        val lens = lensJson(json)
        assertEquals("still", lens.getJSONArray("vendorKeys").getJSONObject(0).getString("scope"))
        assertEquals("decision", lens.getJSONArray("vendorKeys").getJSONObject(1).getString("scope"))
        val photo = lens.getJSONObject("streams").getJSONObject("photo")
        assertEquals(16320, photo.getInt("width"))
        assertEquals(3, photo.getInt("mergeFrameLimit"))
        assertTrue(photo.getBoolean("mergeFramesOneByOne"))
        assertEquals("vivoFullSize", lens.getJSONObject("session").getJSONObject("gate").getString("name"))
        val layout = lens.getJSONObject("image").getJSONObject("sensorLayout")
        assertEquals(4, layout.getInt("block"))
        assertEquals("GRBG", layout.getString("colourOrder"))
        assertEquals("fast", layout.getString("liveRemosaic"))
    }

    @Test
    fun keepsPhotoOnlyQuadLayoutThroughARoundTrip() {
        val lens = LensProfile("H10x", "5")
            .streams { copy(photo = PhotoStreamSettings(RawStreamChoice(RawStreamFormat.Raw10, 8192, 6144),
                includePreview = false, bufferCount = 2)) }
            .image { copy(sensorLayout = LensSensorLayout(photoBlock = 2)) }
        val json = LensProfileCodec.encode(listOf(lens))
        assertEquals(lens, requireNotNull(LensProfileCodec.decode(json)).single())
        val layout = lensJson(json).getJSONObject("image").getJSONObject("sensorLayout")
        assertEquals(1, layout.getInt("block"))
        assertEquals(2, layout.getInt("photoBlock"))
        val photo = lensJson(json).getJSONObject("streams").getJSONObject("photo")
        assertEquals(false, photo.getBoolean("includePreview"))
        assertEquals(2, photo.getJSONObject("buffers").getInt("count"))
    }

    @Test
    fun keepsFullSizeLensFieldsThroughARoundTrip() {
        val full = LensProfile("F85", "5")
            .raw { copy(choice = RawStreamChoice(RawStreamFormat.Raw16, 8160, 6144, highResLive = true),
                buffers = BufferSettings(usage = "gpuDataBuffer")) }
            .image { copy(levels = LensLevels(true, List(4) { 64f }, 1023f)) }
            .copy(vendorKeys = listOf(force.copy(values = listOf(14.0)), force.copy(scope = VendorKeyScope.Request, values = listOf(14.0))))
            .session { copy(gate = "vivoFullSize") }
        assertEquals(full, requireNotNull(LensProfileCodec.decode(LensProfileCodec.encode(listOf(full)))).single())
        // Absent fields stay off, so binned lenses keep the app's reader choice and no gate.
        val plain = requireNotNull(LensProfileCodec.decode("""{"lenses":[{"id":"L","cameraId":"3"}]}""")).single()
        assertEquals(false, plain.streams.raw.choice.highResLive)
        assertNull(plain.streams.raw.buffers.usage)
        assertEquals("", plain.session.gate)
        val raw = lensJson(LensProfileCodec.encode(listOf(plain))).getJSONObject("streams").getJSONObject("raw")
        assertEquals(false, raw.has("buffers") || raw.has("highResLive"))
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
            """{"id":"v2562","schema":2,"lenses":[{"id":"170","camera":{"id":"5","physical":""},""" +
                """"streams":{"raw":{"format":"RAW16","width":4080,"height":3072}},""" +
                """"vendorKeys":[{"tag":"vivo.control.forceSensorMode","type":"int32","scope":"session","values":[31],"enabled":true}],""" +
                """"image":{"levels":{"static":true,"black":[64,64,64,64],"white":1023},""" +
                """"exposure":{"extended":false,"isoMax":0,"shutterMaxNs":0}}}]}"""
        assertEquals(
            LensProfile("170", "5")
                .raw { copy(choice = RawStreamChoice(RawStreamFormat.Raw16, 4080, 3072)) }
                .image { copy(levels = LensLevels(true, List(4) { 64f }, 1023f)) }
                .copy(vendorKeys = listOf(force)),
            LensProfileCodec.decode(native)!!.single()
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
        assertEquals(LensLevels(true, List(4) { 1024f }, 8712f), lens.image.levels)
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
        for (field in listOf(
            """"streams":{"raw":{"buffers":{"count":1}}}""",
            """"streams":{"photo":{"format":"RAW16","width":0,"height":10}}""",
            """"streams":{"auxJpeg":{"width":640}}""",
            """"image":{"sensorLayout":{"block":3}}""",
            // Names schema 2 had before it matched the lens editor.
            """"keys":[]""",
            """"streams":{"capture":{"format":"RAW16","width":8,"height":8}}""",
            """"streams":{"raw":{"reader":{"maxImages":4}}}""",
            """"session":{"backend":"java"}""",
            """"session":{"fps":30}""",
            """"session":{"quirks":["holdTapFocus"]}""",
            """"image":{"cfa":{"block":2}}""",
            """"session":{"template":"video"}"""
        )) rejectedV2(field)
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
            assertTrue(lenses.isNotEmpty(), source.name)
            assertEquals(lenses, LensProfileCodec.decodeBackup(LensProfileCodec.encode(lenses)), source.name)
            assertEquals(lenses, LensProfileCodec.decodeBackup(source.readText()), source.name)
        }
    }

    @Test fun photoSequenceMatchesTheNativeRecipeForBuiltInLenses() {
        // The native recipe log on the V2562: F35 and H14 still=decisionFrame (the HAL latches its
        // full-size/binned choice until a decision frame), L14 still=direct.
        val lenses = requireNotNull(LensProfileCodec.decode(java.io.File("src/main/cpp/camera/profiles/vivo_x300_ultra.json").readText()))
        val byName = lenses.associateBy { it.name }
        for (lens in listOf("F35", "F85", "H14", "H35", "H85"))
            assertEquals(StillSequence.DecisionFrame, byName.getValue(lens).stillSequence(), lens)
        assertEquals(StillSequence.Direct, byName.getValue("L14").stillSequence())
        val f85 = byName.getValue("F85")
        assertEquals(StillSequence.Direct, f85.companion { copy(mode = CompanionMode.Off) }.stillSequence())
        assertEquals(StillSequence.PauseViewfinder, byName.getValue("L14").session { copy(stopViewfinderForPhotos = true) }.stillSequence())
    }
    @Test fun multipleDcpsRoundTripIncludingCameraDefaultAndNativeSelection() {
        val first = LensDcp("0".repeat(32) + ".dcp", "Daylight")
        val second = LensDcp("1".repeat(32) + ".dcp", "Tungsten")
        val lens = LensProfile("35", "3").image { importDcp(first).importDcp(second) }
        roundTrips(lens, lens.image { selectDcp(null) })
        val native = lensJson(LensProfileCodec.encode(listOf(lens), dcpDir = java.io.File("/profiles")))
            .getJSONObject("image")
        assertEquals(second.file, native.getJSONObject("dcp").getString("file"))
        assertTrue(!native.has("dcpProfiles"))
    }

    @Test fun backupRejectsMalformedInactiveDcpProfiles() {
        for (profiles in listOf("{}", "[null]", "[{\"file\":\"../escape.dcp\"}]")) {
            rejectedV2("\"image\":{\"dcpProfiles\":$profiles}")
        }
    }

}
