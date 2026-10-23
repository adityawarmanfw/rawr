package com.rawr.camera.ui.icons.rounded

import androidx.compose.ui.graphics.vector.ImageVector
import com.rawr.camera.ui.icons.Icons
import com.rawr.camera.ui.icons.materialIcon
import com.rawr.camera.ui.icons.materialPath

val Icons.Rounded.ContentCopy: ImageVector
    get() = contentCopy ?: materialIcon(name = "Rounded.ContentCopy") {
        materialPath {
            moveTo(8f, 6f)
            horizontalLineTo(20f)
            verticalLineTo(22f)
            horizontalLineTo(8f)
            close()
            moveTo(10f, 8f)
            verticalLineTo(20f)
            horizontalLineTo(18f)
            verticalLineTo(8f)
            close()
            moveTo(4f, 2f)
            horizontalLineTo(16f)
            verticalLineTo(4f)
            horizontalLineTo(6f)
            verticalLineTo(18f)
            horizontalLineTo(4f)
            close()
        }
    }.also { contentCopy = it }

private var contentCopy: ImageVector? = null
