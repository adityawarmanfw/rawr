package com.rawr.camera.ui

import androidx.compose.foundation.layout.Box
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.testTagsAsResourceId
import com.rawr.camera.BuildConfig

/**
 * Debug builds publish every testTag as a `resource-id`, so `uiautomator dump` and adb scripts can
 * address controls by tag. Popup windows (dialogs, dropdowns) have their own root and are not covered.
 */
@Composable
@OptIn(androidx.compose.ui.ExperimentalComposeUiApi::class)
private fun ExposeTestTags(content: @Composable () -> Unit) {
    if (BuildConfig.DEBUG) Box(Modifier.semantics { testTagsAsResourceId = true }) { content() } else content()
}

@Composable
fun CaptureTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme =
            darkColorScheme(
                background = CaptureColors.Background,
                surface = CaptureColors.Surface,
                primary = CaptureColors.Accent,
                secondary = CaptureColors.AccentSoft,
                surfaceContainerLow =
                    androidx.compose.ui.graphics
                        .Color(0xFF111315),
                surfaceContainer =
                    androidx.compose.ui.graphics
                        .Color(0xFF151719),
                outlineVariant =
                    androidx.compose.ui.graphics
                        .Color(0xFF35383C),
                onBackground =
                    androidx.compose.ui.graphics
                        .Color(0xFFF4F4F4),
                onSurface =
                    androidx.compose.ui.graphics
                        .Color(0xFFF4F4F4)
            ),
        typography =
            Typography(
                labelSmall = MaterialTheme.typography.labelSmall.copy(fontFamily = CaptureMono),
                labelMedium = MaterialTheme.typography.labelMedium.copy(fontFamily = CaptureMono),
                bodySmall = MaterialTheme.typography.bodySmall.copy(fontFamily = CaptureMono)
            ),
        content = { ExposeTestTags(content) }
    )
}
