package com.rawr.camera.integration

import android.os.Build

/** Which SoC family this phone is. The custom GPU driver loader only exists for Qualcomm Adreno. */
object GpuPlatform {
    /** True on Snapdragon, the only platform where a custom Vulkan driver can be loaded. */
    val supportsCustomDriver: Boolean by lazy {
        Build.SOC_MANUFACTURER.equals("QTI", ignoreCase = true) ||
            Build.SOC_MANUFACTURER.equals("Qualcomm", ignoreCase = true)
    }
}
