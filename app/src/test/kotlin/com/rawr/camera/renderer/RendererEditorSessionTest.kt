package com.rawr.camera.renderer

import com.rawr.camera.settings.architecture.*
import com.rawr.camera.settings.model.*
import kotlin.test.*

class RendererEditorSessionTest {
    private fun assertRegion(expected: FloatArray, actual: FloatArray) {
        expected.forEachIndexed { i, value -> assertEquals(value,actual[i],1e-6f) }
    }
    private val defaults = SettingsCatalog.initialState().values

    @Test fun editsPublishImmediatelyAndStayLocalToTheJob() {
        val customized = defaults.copy(rawrBaseTone = ProfileTone(contrast = 23f))
        val pushed = mutableListOf<SettingsValues>()
        val first = RendererEditorSession(customized, pushed::add)
        val second = RendererEditorSession(customized) {}
        first.controller.value.dispatch(SetPhotoLensShadingEnabled(!defaults.photoLensShadingEnabled))
        assertEquals(!defaults.photoLensShadingEnabled, pushed.last().photoLensShadingEnabled)
        first.controller.value.dispatch(SetNumericValue(ImageToneNumericParameter.Contrast, 17f))
        assertEquals(17f, pushed.last().activeImageTone().contrast)
        assertEquals(23f, second.controller.value.state.value.values.activeImageTone().contrast)
        assertEquals(23f, customized.rawrBaseTone.contrast)
        first.controller.value.dispatch(ConfirmReset(ResetTarget.ExposureTonality))
        assertEquals(23f, pushed.last().activeImageTone().contrast)
    }

    @Test fun letterboxedCanvasMapsOnlyVisibleImageAndHandlesOffscreenPan() {
        val view = RendererViewport(2f,0f,0f,100,50,100,100)
        assertRegion(floatArrayOf(.25f,0f,.5f,1f),detailRegion(1,view))
        assertRegion(floatArrayOf(0f,.25f,1f,.5f),detailRegion(6,view))
        assertEquals(0f, displayRegion(view.copy(offsetX=500f))[2])
    }

    @Test fun detailCropRespectsExifTransposeAndMirroring() {
        val viewport = RendererViewport(zoom = 2f, offsetX = 20f, offsetY = -10f, width = 100, height = 100)
        val base = detailRegion(1, viewport)
        floatArrayOf(0.15f, 0.3f, 0.5f, 0.5f).forEachIndexed { i, value -> assertEquals(value, base[i], 1e-6f) }
        assertRegion(floatArrayOf(base[1], 1 - base[0] - base[2], base[3], base[2]), detailRegion(6, viewport))
        assertRegion(floatArrayOf(1 - base[0] - base[2], base[1], base[2], base[3]), detailRegion(2, viewport))
    }
}
