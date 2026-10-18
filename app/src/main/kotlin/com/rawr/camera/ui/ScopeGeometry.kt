package com.rawr.camera.ui

import androidx.compose.ui.unit.dp

internal data class ScopeGeometry(
    val logicalWidth: androidx.compose.ui.unit.Dp,
    val logicalHeight: androidx.compose.ui.unit.Dp,
    val physicalWidth: androidx.compose.ui.unit.Dp,
    val physicalHeight: androidx.compose.ui.unit.Dp,
    val x: androidx.compose.ui.unit.Dp,
    val y: androidx.compose.ui.unit.Dp,
    val presentationQuarterTurns: Int
)

internal fun calculateScopeGeometry(
    maxWidth: androidx.compose.ui.unit.Dp,
    maxHeight: androidx.compose.ui.unit.Dp,
    landscape: Boolean,
    scopeCount: Int,
    index: Int,
    expanded: Boolean,
    deviceRotationDegrees: Int
): ScopeGeometry {
    val availablePhysicalWidth = maxWidth - 32.dp
    val availablePhysicalHeight = maxHeight - 80.dp
    val logicalWidth =
        if (expanded) {
            if (landscape) {
                // A logical 4:3 instrument becomes a physical 3:4 card after the same
                // clockwise quarter-turn used by the rest of the landscape camera UI.
                minOf(300.dp, availablePhysicalHeight, availablePhysicalWidth * (4f / 3f))
            } else {
                minOf(300.dp, availablePhysicalWidth, availablePhysicalHeight * (4f / 3f))
            }
        } else {
            CaptureDimens.ScopeCompactWidth
        }
    val safeLogicalWidth = logicalWidth.coerceAtLeast(1.dp)
    val logicalHeight = if (expanded) safeLogicalWidth * .75f else CaptureDimens.ScopeCompactHeight
    val physicalWidth = if (landscape) logicalHeight else safeLogicalWidth
    val physicalHeight = if (landscape) safeLogicalWidth else logicalHeight
    // Compact scopes use physical dp margins instead of container fractions. This keeps
    // the visible outer margins symmetric across device sizes and makes the landscape
    // top/bottom margins exactly match the portrait left/right margin after rotation.
    val compactOuterMargin = 10.dp
    // Use the same physical inset for portrait top and landscape left/right placement.
    // Match it to the inter-scope gap for a coherent 16 dp spacing rhythm.
    val compactSideInset = 16.dp
    val compactGap = 16.dp
    val x =
        if (expanded) {
            (maxWidth - physicalWidth) / 2
        } else if (landscape) {
            when (index) {
                1 -> compactOuterMargin
                else -> maxWidth - compactOuterMargin - physicalWidth
            }
        } else {
            when (index) {
                0 -> {
                    if (scopeCount ==
                        1
                    ) {
                        maxWidth - compactOuterMargin - physicalWidth
                    } else {
                        compactOuterMargin
                    }
                }

                else -> {
                    maxWidth - compactOuterMargin - physicalWidth
                }
            }
        }
    val y =
        if (expanded) {
            (maxHeight - physicalHeight) / 2
        } else if (landscape) {
            compactSideInset + if (index == 2) physicalHeight + compactGap else 0.dp
        } else {
            compactSideInset + if (index == 2) physicalHeight + compactGap else 0.dp
        }
    return ScopeGeometry(
        logicalWidth = safeLogicalWidth,
        logicalHeight = logicalHeight,
        physicalWidth = physicalWidth,
        physicalHeight = physicalHeight,
        x = x,
        y = y,
        presentationQuarterTurns =
            if (landscape) {
                // The Activity remains portrait-locked, so use the physical-device quadrant
                // rather than a hard-coded clockwise turn. 90° and 270° landscape postures
                // require opposite instrument rotations. DeviceOrientationEffect updates this
                // value before dispatching the Landscape state that causes this recomposition.
                when (deviceRotationDegrees) {
                    270 -> 3
                    else -> 1
                }
            } else {
                0
            }
    )
}

/**
 * Lowest edge of the compact scopes in the right half of the viewfinder, or 0
 * when none sit there. The viewfinder rail stays below it.
 */
internal fun rightScopeBottom(
    maxWidth: androidx.compose.ui.unit.Dp,
    maxHeight: androidx.compose.ui.unit.Dp,
    landscape: Boolean,
    scopeCount: Int,
    deviceRotationDegrees: Int
): androidx.compose.ui.unit.Dp {
    var bottom = 0.dp
    repeat(scopeCount.coerceAtMost(3)) { index ->
        val g = calculateScopeGeometry(maxWidth, maxHeight, landscape, scopeCount, index, false, deviceRotationDegrees)
        if (g.x + g.physicalWidth / 2 > maxWidth / 2) bottom = maxOf(bottom, g.y + g.physicalHeight)
    }
    return bottom
}
