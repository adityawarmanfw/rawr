package com.rawr.camera.settings.preferences

import com.rawr.camera.settings.model.*
import kotlin.test.*

class LookFileCodecTest {
    @Test fun filmLookFileRoundTrips() {
        val look = FilmFactoryPresets.baseLook.copy(film = 6, grainEnabled = true, grainAmount = 0.4f)
        val imported = LookFileCodec.import(LookFileCodec.exportFilm(FilmPreset("x", "Warm", look)))
        assertEquals(ImportedLook.Film("Warm", look), imported)
    }

    @Test fun agxLookFileRoundTrips() {
        val values = AgxPreset.Sigmoid.defaults.with(AgxParameter.Contrast, 2.5f)
        val imported = LookFileCodec.import(LookFileCodec.exportAgx(AgxLook("x", "Soft", AgxPreset.Sigmoid, values)))
        assertEquals(ImportedLook.Agx("Soft", AgxPreset.Sigmoid, values), imported)
    }

    @Test fun savedAgxLooksRoundTripAndSkipDamagedRows() {
        val looks = listOf(AgxLook("a", "A", AgxPreset.Default, AgxPreset.Default.defaults),
            AgxLook("b", "B", AgxPreset.Smooth, AgxPreset.Smooth.defaults.with(AgxParameter.LookSaturation, 1.2f)))
        assertEquals(looks, LookFileCodec.decodeAgxLooks(LookFileCodec.encodeAgxLooks(looks)))
        assertEquals(emptyList(), LookFileCodec.decodeAgxLooks("not json"))
    }

    @Test fun rejectsForeignFiles() {
        assertFailsWith<IllegalArgumentException> { LookFileCodec.import("{\"name\":\"x\"}") }
    }
}
