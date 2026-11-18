package com.rawr.camera.ui

import kotlin.test.*

class LevelCrosshairTest {
    private fun arms(landscape: Float, flat: Float) = crosshairArms(landscape, flat, major = 34f, minor = 18f, floor = 12f)

    @Test fun portraitKeepsTheHorizontalArmFull() {
        val a = arms(0f, 0f)
        assertEquals(34f, a.horizontal, 1e-4f)
        assertEquals(18f, a.vertical, 1e-4f)
    }

    @Test fun landscapeKeepsTheVerticalArmFull() {
        val a = arms(1f, 0f)
        assertEquals(18f, a.horizontal, 1e-4f)
        assertEquals(34f, a.vertical, 1e-4f)
    }

    @Test fun skyAndGroundGrowBothArmsToFullInEitherPosture() {
        for (landscape in listOf(0f, 1f)) {
            val a = arms(landscape, 1f)
            assertEquals(34f, a.horizontal, 1e-4f)
            assertEquals(34f, a.vertical, 1e-4f)
        }
    }

    @Test fun halfwayThroughTheTurnTheArmsMeetInTheMiddle() {
        val a = arms(0.5f, 0f)
        assertEquals(26f, a.horizontal, 1e-4f)
        assertEquals(26f, a.vertical, 1e-4f)
    }

    @Test fun springOvershootPopsTheArmButNeverCollapsesIt() {
        // 20% overshoot toward landscape: the vertical arm pops past full, the horizontal dips below its stub.
        val pop = arms(1.2f, 0f)
        assertTrue(pop.vertical > 34f)
        assertTrue(pop.horizontal < 18f && pop.horizontal >= 12f)
        // A wild overshoot is held at the floor.
        assertEquals(12f, arms(5f, 0f).horizontal, 1e-4f)
    }
}
