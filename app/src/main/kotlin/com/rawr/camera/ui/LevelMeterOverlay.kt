package com.rawr.camera.ui

import androidx.compose.animation.core.Spring
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.spring
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.Box
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.State
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.rotate
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.unit.dp
import androidx.compose.ui.util.lerp
import kotlin.math.abs
import kotlin.math.hypot
import kotlin.math.max

private val LevelGreen = Color(0xFF59E36B)
private val LevelYellow = Color(0xFFFFD84A)
private val LevelWhite = Color.White.copy(alpha = .9f)
private val LevelDim = Color.White.copy(alpha = .6f)
private val LevelShadow = Color.Black.copy(alpha = .35f)

/** Line width of the ticks, crosshair and ring; the dark outline adds 1 dp. */
private const val LEVEL_STROKE_DP = 1f

/** The weight the crosshair takes when the camera reports it has found focus. */
private const val FOCUS_STROKE_DP = 2f

/** Fixed tick distance from the centre and tick length (dp). */
private const val TICK_RADIUS_DP = 46f
private const val TICK_LENGTH_DP = 6f

/** Crosshair: centre gap, the arm that matters, and the short stub the other arm shrinks to (dp). */
private const val GAP_DP = 10f
private const val ARM_DP = 34f
private const val MINOR_ARM_DP = 18f

/** How far the bubble travels per degree, and the most it travels (dp / degrees). */
private const val BUBBLE_DP_PER_DEG = 2.5f
private const val BUBBLE_MAX_DEG = 16f

/** Radius of the moving bubble; the fixed ring around the centre is one dp larger. */
private const val BUBBLE_RADIUS_DP = 5f
private const val BUBBLE_ALPHA = .45f

/**
 * Four small fixed ticks (left, right, up, down) frame a crosshair that stays at the centre and
 * only rotates with the roll: lined up with the ticks means the horizon is level. A fixed ring at
 * the centre is the target; a faint borderless bubble moves freely for both roll and the up/down
 * angle and fills the ring when level. Only one crosshair arm matters at a time, the one that lines
 * up with the horizon as the photographer sees it: horizontal in portrait, vertical in landscape.
 * The other arm shrinks to a stub, and turning the phone springs the two over. When the lens swings
 * toward the sky or ground the crosshair eases to still, both arms grow back to full length, and the
 * bubble's meaning cross-fades to the tilt, over 60 to 75 degrees of elevation.
 */
@Composable
internal fun LevelMeterOverlay(modifier: Modifier = Modifier, focusConfirmed: Boolean = false) {
    val reading = rememberLevelReading()
    LockHaptic(reading)
    // Recomposes only when the phone crosses between portrait and landscape, not at sensor rate.
    val landscape by remember { derivedStateOf { reading.value?.quadrant.let { it == 90 || it == 270 } } }
    val landscapeBlend = animateFloatAsState(
        targetValue = if (landscape) 1f else 0f,
        animationSpec = spring(dampingRatio = Spring.DampingRatioMediumBouncy, stiffness = Spring.StiffnessMediumLow),
        label = "levelCrosshairLandscape"
    )
    Box(modifier.testTag(CaptureTestTags.LEVEL_METER)) {
        Canvas(Modifier.matchParentSize()) {
            val r = reading.value ?: return@Canvas
            drawLevel(r, landscapeBlend.value, focusConfirmed)
        }
    }
}

/**
 * One faint tick each time the display goes from not-level to level. Nothing fires for the first
 * reading, so enabling the meter or resuming the app while already level stays silent. Only the
 * locked flag is read here, so this does not recompose at sensor rate.
 */
@Composable
private fun LockHaptic(reading: State<LevelReading?>) {
    val haptics = LocalCaptureHaptics.current
    val locked by remember {
        derivedStateOf {
            reading.value?.let { if (it.flat) it.tiltLevel else it.rollLevel && it.pitchLevel }
        }
    }
    var previous by remember { mutableStateOf<Boolean?>(null) }
    LaunchedEffect(locked) {
        if (locked == true && previous == false) haptics.sliderTick()
        previous = locked
    }
}

/** Green once locked, yellow when almost there, white while still far. */
private fun levelColor(locked: Boolean, angleDeg: Float): Color = when {
    locked -> LevelGreen
    abs(angleDeg) <= 3f -> LevelYellow
    else -> LevelWhite
}

/** A line with a faint dark underlay so it stays readable on bright scenes. */
private fun DrawScope.mark(color: Color, a: Offset, b: Offset, width: Float = LEVEL_STROKE_DP.dp.toPx()) {
    drawLine(LevelShadow, a, b, width + 1.dp.toPx(), StrokeCap.Round)
    drawLine(color, a, b, width, StrokeCap.Round)
}

/** Lengths of the crosshair's horizontal and vertical arms, in px. */
internal data class CrosshairArms(val horizontal: Float, val vertical: Float)

/**
 * Only the arm that lines up with the horizon as the photographer sees it is full length: the
 * horizontal arm in portrait ([landscapeBlend] 0), the vertical arm in landscape (1). Pointing at the
 * sky or ground ([flatBlend] 1) there is no horizon, so both arms are full. The blends may overshoot
 * their 0..1 range while the spring settles, which makes the arms pop; [floor] keeps a stub visible.
 */
internal fun crosshairArms(landscapeBlend: Float, flatBlend: Float, major: Float, minor: Float, floor: Float): CrosshairArms {
    val horizontal = lerp(lerp(major, minor, landscapeBlend), major, flatBlend)
    val vertical = lerp(lerp(minor, major, landscapeBlend), major, flatBlend)
    return CrosshairArms(max(horizontal, floor), max(vertical, floor))
}

private fun DrawScope.drawLevel(r: LevelReading, landscapeBlend: Float, focusConfirmed: Boolean) {
    val c = center
    val w = r.flatBlend
    val flat = r.flat

    // Horizon reading: roll and up/down. Sky/ground reading: tilt along both screen axes.
    val horizonWorst = max(abs(r.rollDeg), abs(r.elevationDeg))
    val flatWorst = max(abs(r.bubbleXDeg), abs(r.bubbleYDeg))
    val horizonColor = levelColor(r.rollLevel && r.pitchLevel, horizonWorst)
    val flatColor = levelColor(r.tiltLevel, flatWorst)
    val activeColor = if (flat) flatColor else horizonColor
    // At the horizon the crosshair only reports roll against the fixed ticks.
    val crosshairColor = if (flat) flatColor else levelColor(r.rollLevel, r.rollDeg)
    // The camera having found its own focus is worth more attention than a level
    // horizon: it is the reading that says the photo will be sharp. Both green,
    // so focus wins the shade and the weight.
    val focusGreen = focusConfirmed && activeColor != LevelGreen
    val focusStroke = if (focusGreen) FOCUS_STROKE_DP.dp.toPx() else LEVEL_STROKE_DP.dp.toPx()
    val crosshairShown = if (focusGreen) LevelGreen else crosshairColor

    // Fixed ticks. Four-fold symmetric, so they need no rotation in any posture.
    val tr = TICK_RADIUS_DP.dp.toPx()
    val tl = TICK_LENGTH_DP.dp.toPx()
    listOf(-1f, 1f).forEach { s ->
        mark(LevelDim, Offset(c.x + s * tr, c.y), Offset(c.x + s * (tr + tl), c.y))
        mark(LevelDim, Offset(c.x, c.y + s * tr), Offset(c.x, c.y + s * (tr + tl)))
    }

    // Crosshair: fixed at the centre. It rolls with the horizon and unwinds smoothly to still as the
    // lens swings toward the sky or ground, where roll stops meaning anything.
    val gap = GAP_DP.dp.toPx()
    val arms = crosshairArms(landscapeBlend, w, ARM_DP.dp.toPx(), MINOR_ARM_DP.dp.toPx(), gap + 2.dp.toPx())
    rotate(r.rollDeg * (1f - w), c) {
        listOf(-1f, 1f).forEach { s ->
            mark(crosshairShown, Offset(c.x + s * gap, c.y), Offset(c.x + s * arms.horizontal, c.y), focusStroke)
            mark(crosshairShown, Offset(c.x, c.y + s * gap), Offset(c.x, c.y + s * arms.vertical), focusStroke)
        }
    }

    // Fixed bubble at the centre: the target the moving bubble settles into.
    val ringR = BUBBLE_RADIUS_DP.dp.toPx() + 1.dp.toPx()
    val ringColor = if (activeColor == LevelGreen || focusGreen) LevelGreen else LevelDim
    drawCircle(LevelShadow, ringR, c, style = Stroke((LEVEL_STROKE_DP + 1f).dp.toPx()))
    drawCircle(ringColor, ringR, c, style = Stroke(LEVEL_STROKE_DP.dp.toPx()))

    // Moving bubble: faint and borderless. The two readings cross-fade by elevation, so the bubble
    // fades from one meaning to the other and never jumps across the dial.
    val u = BUBBLE_DP_PER_DEG.dp.toPx()
    val limit = BUBBLE_MAX_DEG * u
    if (w < 1f) {
        // Horizon: toward the high side for roll; down when the lens is above the horizon.
        // Drawn in the phone's physical quadrant because the Activity is portrait-locked.
        rotate(r.quadrant.toFloat(), c) {
            drawBubble(c, r.rollDeg * u, r.elevationDeg * u, limit,
                if (focusGreen) LevelGreen else horizonColor, (1f - w) * BUBBLE_ALPHA)
        }
    }
    if (w > 0f) {
        drawBubble(c, r.bubbleXDeg * u, r.bubbleYDeg * u, limit,
            if (focusGreen) LevelGreen else flatColor, w * BUBBLE_ALPHA)
    }
}

private fun DrawScope.drawBubble(c: Offset, x: Float, y: Float, limit: Float, color: Color, alpha: Float) {
    var dx = x
    var dy = y
    val len = hypot(dx, dy)
    if (len > limit) { dx *= limit / len; dy *= limit / len }
    drawCircle(color.copy(alpha = alpha), BUBBLE_RADIUS_DP.dp.toPx(), Offset(c.x + dx, c.y + dy))
}
