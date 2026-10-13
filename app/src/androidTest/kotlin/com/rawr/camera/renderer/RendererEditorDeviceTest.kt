package com.rawr.camera.renderer

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class RendererEditorDeviceTest : RendererEditorInteractionChecks()

@RunWith(AndroidJUnit4::class)
class RendererEditorLandscapeDeviceTest : RendererEditorInteractionChecks() {
    override val landscape = true
}
