package com.rawr.camera.location

import android.location.LocationManager

/** Shared freshness, selection and acquisition limits for photo and video location. */
internal object LocationPolicy {
    const val MAX_AGE_NANOS = 120_000_000_000L
    const val REFRESH_AGE_NANOS = 30_000_000_000L
    const val REFRESH_FINE_UNCERTAINTY_METERS = 50.0
    const val SAME_PROVIDER_WORSENING_METERS = 200.0

    const val CONTINUING_INTERVAL_MILLIS = 30_000L
    const val CONTINUING_MIN_INTERVAL_MILLIS = 10_000L
    const val CONTINUING_MIN_DISTANCE_METERS = 10f
    const val PASSIVE_MIN_INTERVAL_MILLIS = 0L
    const val REQUEST_DURATION_MILLIS = 120_000L
    const val MAINTENANCE_MILLIS = 110_000L

    const val ONE_SHOT_DURATION_MILLIS = 30_000L
    const val ONE_SHOT_COOLDOWN_NANOS = 30_000_000_000L

    val CONTINUING_PROVIDERS = listOf(LocationManager.FUSED_PROVIDER, LocationManager.NETWORK_PROVIDER)
    val ONE_SHOT_PROVIDERS = listOf(LocationManager.GPS_PROVIDER, LocationManager.FUSED_PROVIDER,
        LocationManager.NETWORK_PROVIDER)
}
