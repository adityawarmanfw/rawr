package com.rawr.camera.settings.model

import com.rawr.camera.settings.architecture.PersistentSettingsController
import com.rawr.camera.settings.architecture.SetVideoEncoder
import com.rawr.camera.settings.fixtures.SettingsFixtures
import com.rawr.camera.video.VideoResolutionMode
import kotlin.test.Test
import kotlin.test.assertEquals

class VideoEncoderConfigTest {
    @Test fun defaultsMatchThePreviousHardcodedRecorderSettings() {
        val d = VideoEncoderConfig()
        assertEquals(10, d.bitDepth)
        assertEquals(12, d.bitrateMbps(VideoResolutionMode.HD1080))
        assertEquals(40, d.bitrateMbps(VideoResolutionMode.UHD4K))
        assertEquals(60, d.bitrateMbps(VideoResolutionMode.OPEN_GATE))
        assertEquals(2, d.bitrateMode.wireValue)
        assertEquals(KeyframeInterval.One, d.keyframeInterval)
        assertEquals(-1, d.maxBFrames)
        assertEquals(2, d.audioChannels)
        assertEquals(192, d.audioBitrateKbps)
        assertEquals(d, d.sanitized())
    }

    @Test fun sanitizedClampsToWhatTheNativeRecorderAccepts() {
        val s = VideoEncoderConfig(
            bitDepth = 12, bitrate1080pMbps = 0, bitrate4kMbps = 500, bitrateOpenGateMbps = -3,
            maxBFrames = 7, audioChannels = 6, audioBitrateKbps = 200
        ).sanitized()
        assertEquals(10, s.bitDepth)
        assertEquals(1, s.bitrate1080pMbps)
        assertEquals(400, s.bitrate4kMbps)
        assertEquals(1, s.bitrateOpenGateMbps)
        assertEquals(2, s.maxBFrames)
        assertEquals(2, s.audioChannels)
        assertEquals(192, s.audioBitrateKbps)
    }

    @Test fun keyframeIntervalScalesWithFrameRate() {
        assertEquals(listOf(1, 12, 24, 48), KeyframeInterval.entries.map { it.frames(24) })
        assertEquals(listOf(1, 15, 30, 60), KeyframeInterval.entries.map { it.frames(30) })
    }

    @Test fun keyframeWireValuesMapToNearestChoice() {
        assertEquals(KeyframeInterval.AllIntra, KeyframeInterval.of(0))
        assertEquals(KeyframeInterval.One, KeyframeInterval.of(2))
        // Legacy whole seconds x 2: 1 s -> 1x, 10 s -> 2x.
        assertEquals(KeyframeInterval.Two, KeyframeInterval.of(20))
    }

    @Test fun gopPatternDescribesRequestedStructure() {
        assertEquals("I I I … (all-intra)", VideoEncoderConfig.gopPattern(1, 2))
        assertEquals("I B B P B B P … (30)", VideoEncoderConfig.gopPattern(30, 2))
        assertEquals("I P P P (4)", VideoEncoderConfig.gopPattern(4, 0))
        assertEquals("I P P P P P P … (30) · B-frames auto", VideoEncoderConfig.gopPattern(30, -1))
    }

    @Test fun bitrateModeWireValuesMatchMediaCodec() {
        assertEquals(VideoBitrateMode.Cq, VideoBitrateMode.of(0))
        assertEquals(VideoBitrateMode.Vbr, VideoBitrateMode.of(1))
        assertEquals(VideoBitrateMode.Cbr, VideoBitrateMode.of(2))
        assertEquals(VideoBitrateMode.Cbr, VideoBitrateMode.of(42))
    }

    @Test fun perResolutionBitrateEditsOnlyThatMode() {
        val edited = VideoEncoderConfig().withBitrateMbps(VideoResolutionMode.UHD4K, 80)
        assertEquals(12, edited.bitrate1080pMbps)
        assertEquals(80, edited.bitrate4kMbps)
        assertEquals(60, edited.bitrateOpenGateMbps)
    }

    @Test fun controllerStoresSanitizedEncoderSettings() {
        val controller = PersistentSettingsController(SettingsFixtures.initialState(fullySupported = true))
        controller.dispatch(SetVideoEncoder(VideoEncoderConfig(bitDepth = 8, maxBFrames = 9)))
        val stored = controller.state.value.values.videoEncoder
        assertEquals(8, stored.bitDepth)
        assertEquals(2, stored.maxBFrames)
    }
}
