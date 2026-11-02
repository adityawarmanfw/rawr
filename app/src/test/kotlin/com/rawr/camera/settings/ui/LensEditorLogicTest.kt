package com.rawr.camera.settings.ui

import com.rawr.camera.settings.model.AuxJpegSettings
import com.rawr.camera.settings.model.PhotoStreamSettings
import com.rawr.camera.settings.model.CompanionMode
import com.rawr.camera.settings.model.LensLevels
import com.rawr.camera.settings.model.LensOutputSize
import com.rawr.camera.settings.model.LensProfile
import com.rawr.camera.settings.model.RawStreamChoice
import com.rawr.camera.settings.model.RawStreamFormat
import com.rawr.camera.settings.model.VendorKey
import com.rawr.camera.settings.model.VendorKeyScope
import com.rawr.camera.settings.model.validateLensProfile
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNull
import kotlin.test.assertTrue

class LensEditorLogicTest {
    private val lens = LensProfile(name = "H35", cameraId = "0")
    private val normal = RawStreamChoice(RawStreamFormat.Raw16, 4080, 3064)
    private val live = RawStreamChoice(RawStreamFormat.Raw10, 8160, 6144, highResLive = true)
    private val photoStream = PhotoStreamSettings(RawStreamChoice(RawStreamFormat.Raw16, 8192, 6144), mergeFrameLimit = 3)

    @Test
    fun resolutionFollowsTheStreams() {
        assertEquals(LensResolution.Normal, lens.resolution())
        assertEquals(LensResolution.HighResLive, lens.raw { copy(choice = live) }.resolution())
        // A photo stream wins over a high-res live viewfinder stream: photos come from the photo stream.
        assertEquals(LensResolution.HighResPhotos, lens.raw { copy(choice = live) }.streams { copy(photo = photoStream) }.resolution())
    }

    @Test
    fun switchingModesSetsTheStreams() {
        val toLive = lens.withResolution(LensResolution.HighResLive, normal, live, photoStream, vivo = false)
        assertEquals(live, toLive.streams.raw.choice)
        assertNull(toLive.streams.photo)

        val toPhotos = toLive.withResolution(LensResolution.HighResPhotos, normal, live, photoStream, vivo = false)
        assertEquals(normal, toPhotos.streams.raw.choice)
        assertEquals(photoStream, toPhotos.streams.photo)

        val back = toPhotos.withResolution(LensResolution.Normal, normal, live, photoStream, vivo = false)
        assertEquals(normal, back.streams.raw.choice)
        assertNull(back.streams.photo)
    }

    @Test
    fun liveIsAlwaysUnlistedAndNormalNever() {
        val listed = live.copy(highResLive = false)
        assertTrue(lens.withResolution(LensResolution.HighResLive, normal, listed, photoStream, vivo = false).streams.raw.choice.highResLive)
        val unlistedNormal = normal.copy(highResLive = true)
        for (mode in listOf(LensResolution.Normal, LensResolution.HighResPhotos)) {
            assertEquals(false, lens.withResolution(mode, unlistedNormal, live, photoStream, vivo = false).streams.raw.choice.highResLive)
        }
    }

    @Test
    fun highResOnVivoGetsTheGateAndNormalDropsIt() {
        for (mode in listOf(LensResolution.HighResLive, LensResolution.HighResPhotos)) {
            assertEquals("vivoFullSize", lens.withResolution(mode, normal, live, photoStream, vivo = true).session.gate)
            assertEquals("", lens.withResolution(mode, normal, live, photoStream, vivo = false).session.gate)
        }
        // A gate the user picked is kept.
        val vcf = lens.session { copy(gate = "vivoFullSizeVcf") }
        assertEquals("vivoFullSizeVcf", vcf.withResolution(LensResolution.HighResPhotos, normal, live, photoStream, vivo = true).session.gate)

        val held = lens.session { copy(gate = "vivoFullSize", gateHold = "closed") }
        val normalAgain = held.withResolution(LensResolution.Normal, normal, live, photoStream, vivo = true).session
        assertEquals("", normalAgain.gate)
        assertNull(normalAgain.gateHold)
    }

    @Test
    fun switchedLensesStayValid() {
        for (mode in LensResolution.entries) {
            assertEquals(emptyList(), validateLensProfile(lens.withResolution(mode, normal, live, photoStream, vivo = true), emptyList()))
        }
    }

    @Test
    fun keyScopesFollowPhotoStreamAndSessionParameters() {
        assertEquals(
            listOf(VendorKeyScope.Session, VendorKeyScope.Request, VendorKeyScope.SessionAndRequest),
            lens.vendorKeyScopes()
        )
        assertEquals(VendorKeyScope.entries, lens.streams { copy(photo = photoStream) }.vendorKeyScopes())
        assertEquals(listOf(VendorKeyScope.Request), lens.session { copy(params = "none") }.vendorKeyScopes())
    }

    @Test
    fun advancedSummaryNamesWhatIsChanged() {
        assertEquals(emptyList(), lens.advancedSummary())
        assertEquals(listOf("Companion fixed"), lens.companion { copy(mode = CompanionMode.Fixed) }.advancedSummary())
        assertEquals(listOf("Companion off"), lens.companion { copy(mode = CompanionMode.Off) }.advancedSummary())
        assertEquals(listOf("Companion tuned"), lens.companion { copy(bufferCount = 4) }.advancedSummary())
        assertEquals(listOf("Buffers"), lens.raw { copy(buffers = buffers.copy(usage = "gpuDataBuffer")) }.advancedSummary())
        assertEquals(listOf("Use cases"), lens.raw { copy(useCase = 1) }.advancedSummary())
        assertEquals(listOf("Java Camera2"), lens.session { copy(cameraApi = "java") }.advancedSummary())
        assertEquals(listOf("Session"), lens.copy(request = lens.request.copy(defaults = "minimal")).advancedSummary())
        assertEquals(listOf("Stream stops for photos"), lens.session { copy(stopViewfinderForPhotos = true) }.advancedSummary())
        assertEquals(listOf("2 workarounds"), lens.session { copy(workarounds = listOf("noTapFocus", "keepTemplateRegions")) }.advancedSummary())
    }

    @Test
    fun advancedSummaryIgnoresTheVisibleSettings() {
        val visible = lens
            .raw { copy(choice = live) }
            .streams { copy(photo = photoStream) }
            .session { copy(opMode = 32768, frameRate = 60, gate = "vivoFullSize") }
            .copy(vendorKeys = listOf(VendorKey("vivo.control.forceSensorMode", values = listOf(17.0))))
            .image { copy(levels = LensLevels(true, listOf(64f, 64f, 64f, 64f), 1023f)) }
        assertEquals(emptyList(), visible.advancedSummary())
    }

    @Test
    fun resetToAutomaticKeepsTheVisibleSettings() {
        val visible = lens
            .streams { copy(photo = photoStream) }
            .session { copy(opMode = 32768, frameRate = 60, gate = "vivoFullSize") }
            .copy(vendorKeys = listOf(VendorKey("vivo.control.forceSensorMode", values = listOf(17.0))))
        val tuned = visible
            .companion { copy(mode = CompanionMode.Fixed, size = LensOutputSize(640, 480), useCase = 4) }
            .streams { copy(auxJpeg = AuxJpegSettings(LensOutputSize(4096, 3072)), photo = photo!!.copy(useCase = 2, bufferUsage = "none")) }
            .raw { copy(useCase = 1, buffers = buffers.copy(count = 8, testFirst = true), levels = LensLevels()) }
            .session { copy(cameraApi = "java", params = "full", stillTemplate = true, startupDelayMs = 500, stopViewfinderForPhotos = true, workarounds = listOf("noTapFocus")) }
            .copy(request = lens.request.copy(defaults = "minimal"))
        assertTrue(tuned.advancedSummary().isNotEmpty())
        val reset = tuned.withAutomaticAdvanced()
        assertEquals(visible, reset)
        assertEquals(emptyList(), reset.advancedSummary())
    }

    @Test
    fun lensSettingsImplyWorkarounds() {
        assertEquals(emptyMap(), impliedWorkarounds(lens))
        assertEquals(setOf("holdTapFocus"), impliedWorkarounds(lens.session { copy(gate = "vivoFullSize") }).keys)
        assertEquals(
            setOf("appPriorityAe", "alwaysSendFrameRate", "noAutoCompanion"),
            impliedWorkarounds(lens.raw { copy(choice = live) }).keys
        )
    }
}
