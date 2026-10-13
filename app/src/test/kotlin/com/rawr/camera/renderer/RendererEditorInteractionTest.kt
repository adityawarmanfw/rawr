package com.rawr.camera.renderer

import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.annotation.LooperMode

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34], qualifiers = "w412dp-h892dp")
@LooperMode(LooperMode.Mode.PAUSED)
class RendererEditorInteractionTest : RendererEditorInteractionChecks()

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34], qualifiers = "w892dp-h412dp-land")
@LooperMode(LooperMode.Mode.PAUSED)
class RendererEditorLandscapeInteractionTest : RendererEditorInteractionChecks()
