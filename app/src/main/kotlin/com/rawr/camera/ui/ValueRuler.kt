package com.rawr.camera.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.snapping.SnapPosition
import androidx.compose.foundation.gestures.snapping.rememberSnapFlingBehavior
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.wrapContentWidth
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.snapshotFlow
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlin.math.abs
import kotlinx.coroutines.flow.distinctUntilChanged

/**
 * A large horizontal ruler: flick or drag it, it glides and snaps to the nearest step with a haptic tick, and the step
 * under the centre line is the value. Replaces the small scrub-on-a-button gesture for every adjustable value.
 *
 * [stepCount] steps, one tick each; [isMajor] ticks are taller and carry the label from [labelAt]. [selectedIndex] is
 * the value owned by the caller: when it changes from elsewhere (reset, auto readout) the ruler follows, unless the
 * user is moving it right now. [onIndexChange] fires for every step the user passes.
 */
@Composable
internal fun ValueRuler(
    stepCount: Int,
    selectedIndex: Int,
    onIndexChange: (Int) -> Unit,
    isMajor: (Int) -> Boolean,
    labelAt: (Int) -> String,
    modifier: Modifier = Modifier,
    stepWidth: Dp = 14.dp,
    accent: Color = CaptureColors.Accent
) {
    if (stepCount <= 0) return
    val haptics = LocalCaptureHaptics.current
    val safeSelected = selectedIndex.coerceIn(0, stepCount - 1)
    val listState = rememberLazyListState(initialFirstVisibleItemIndex = safeSelected)
    val latestOnChange by rememberUpdatedState(onIndexChange)
    val latestSelected by rememberUpdatedState(safeSelected)

    BoxWithConstraints(modifier.fillMaxWidth().height(RULER_HEIGHT)) {
        val sidePadding = ((maxWidth - stepWidth) / 2).coerceAtLeast(0.dp)

        // The step whose centre is nearest the middle of the viewport.
        fun centredIndex(): Int {
            val info = listState.layoutInfo
            val centre = (info.viewportStartOffset + info.viewportEndOffset) / 2
            return info.visibleItemsInfo
                .minByOrNull { abs(it.offset + it.size / 2 - centre) }
                ?.index
                ?: latestSelected
        }

        // Follow the caller when the value moves without the user touching the ruler.
        LaunchedEffect(safeSelected, stepCount) {
            if (!listState.isScrollInProgress && centredIndex() != safeSelected) {
                listState.scrollToItem(safeSelected)
            }
        }
        // Report every step the user's finger or fling passes over.
        LaunchedEffect(listState) {
            snapshotFlow { if (listState.isScrollInProgress) centredIndex() else -1 }
                .distinctUntilChanged()
                .collect { index ->
                    if (index >= 0 && index != latestSelected) {
                        haptics.detent()
                        latestOnChange(index)
                    }
                }
        }

        LazyRow(
            state = listState,
            modifier = Modifier.fillMaxWidth().height(RULER_HEIGHT),
            contentPadding = PaddingValues(horizontal = sidePadding),
            flingBehavior = rememberSnapFlingBehavior(listState, snapPosition = SnapPosition.Center),
            verticalAlignment = Alignment.CenterVertically
        ) {
            items(stepCount) { index ->
                val major = isMajor(index)
                val distance = abs(index - safeSelected)
                val alpha = (1f - distance * .035f).coerceIn(.28f, 1f)
                Column(
                    Modifier.width(stepWidth),
                    horizontalAlignment = Alignment.CenterHorizontally
                ) {
                    Box(
                        Modifier
                            .width(if (major) 2.dp else 1.dp)
                            .height(if (major) 24.dp else 13.dp)
                            .background(Color.White.copy(alpha = if (major) .85f * alpha else .5f * alpha))
                    )
                    // The label is wider than a step, so it overflows around the tick instead of wrapping.
                    if (major) {
                        Text(
                            text = labelAt(index),
                            color = Color.White.copy(alpha = .78f * alpha),
                            fontFamily = CaptureMono,
                            fontSize = 10.sp,
                            maxLines = 1,
                            softWrap = false,
                            textAlign = TextAlign.Center,
                            modifier = Modifier
                                .width(stepWidth)
                                .wrapContentWidth(Alignment.CenterHorizontally, unbounded = true)
                        )
                    }
                }
            }
        }
        // Fixed centre line: the value is whatever sits under it.
        Box(
            Modifier
                .align(Alignment.TopCenter)
                .width(3.dp)
                .height(40.dp)
                .background(accent, RoundedCornerShape(2.dp))
        )
    }
}

private val RULER_HEIGHT = 46.dp

/** Value readout shown above a ruler: big number, small unit or title. */
@Composable
internal fun RulerReadout(title: String, value: String, modifier: Modifier = Modifier) {
    Column(modifier.fillMaxWidth(), horizontalAlignment = Alignment.CenterHorizontally) {
        Text(
            title,
            color = CaptureColors.Muted,
            fontFamily = CaptureMono,
            fontSize = 10.sp,
            fontWeight = FontWeight.Bold
        )
        Text(
            value,
            color = Color.White,
            fontFamily = CaptureMono,
            fontSize = 26.sp,
            fontWeight = FontWeight.Bold
        )
    }
}
