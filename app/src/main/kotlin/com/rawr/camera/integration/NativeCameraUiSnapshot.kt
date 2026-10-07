package com.rawr.camera.integration

import com.rawr.camera.model.FaceDetection
import com.rawr.camera.model.WhiteBalanceMode
import org.json.JSONObject

internal data class NativeShutterAngleChoice(val degrees: Double, val exposureTimeNs: Long)

internal data class NativeCameraUiSnapshot(
    val generation: Long,
    val cameraId: String,
    val lensId: String,
    val sensitivityMin: Int,
    val sensitivityMax: Int,
    val exposureTimeMinNs: Long,
    val exposureTimeMaxNs: Long,
    val evMinSteps: Int,
    val evMaxSteps: Int,
    val evStep: Double,
    val manualExposureSupported: Boolean,
    val shutterPrioritySupported: Boolean,
    val isoPrioritySupported: Boolean,
    val tapAfSupported: Boolean,
    val manualFocusSupported: Boolean,
    val focusDistanceReadoutTrustworthy: Boolean,
    val minimumFocusDistance: Float,
    val hyperfocalDistance: Float,
    val exposureMode: Int,
    val semanticExposureMode: Int,
    val focusMode: Int,
    val whiteBalanceMode: Int,
    val whiteBalanceTemperatureK: Int,
    val whiteBalanceTint: Int,
    val manualWhiteBalanceSupported: Boolean,
    val supportedAwbModes: List<Int>,
    val hasAutoWbGains: Boolean,
    val autoWbGainR: Float,
    val autoWbGainG: Float,
    val autoWbGainB: Float,
    // Both calibrated and fallback estimates are resolved by the native camera owner.
    val hasAutoWbEstimate: Boolean = false,
    val autoWbTemperatureK: Int = WhiteBalanceMode.TEMP_DEFAULT_K,
    val autoWbTint: Int = 0,
    val requestedExposureTimeNs: Long,
    val requestedSensitivity: Int,
    val requestedEvSteps: Int,
    val requestedManualFocusNormalized: Float,
    val appliedExposureTimeNs: Long?,
    val appliedSensitivity: Int?,
    val appliedEvSteps: Int?,
    val afState: Int?,
    val appliedFocusDistance: Float?,
    /** Camera2 POST_RAW_SENSITIVITY_BOOST result (100 == 1x); null before first result. */
    val appliedPostRawSensitivityBoost: Int? = null,
    /** Sensor-reported rate (frame-duration echo, else AE range max); null when unknown. */
    val appliedRawFps: Double? = null,
    /** Measured viewfinder rate (timestamp-delta EMA); null until settled. */
    val measuredViewfinderFps: Double? = null,
    val tapAfActive: Boolean = false,
    val focusRequestId: Long = 0L,
    val videoMode: Boolean = false,
    val videoPreviewFps: Int = 30,
    val requestedShutterAngleDegrees: Double? = null,
    val shutterAngleChoices: List<NativeShutterAngleChoice> = emptyList(),
    val autoWbEstimateCalibrated: Boolean = false,
    val whiteBalanceRequestId: Long = 0L,
    /** Lenses of the native camera profile as (lensId, focal label); empty before the first snapshot. */
    val profileLenses: List<Pair<String, String>> = emptyList(),
    /** RAW frames reach the GPU through the slower CPU-copy path on this device/stream. */
    val rawCpuIngress: Boolean = false
) {
    companion object {
        fun parse(text: String): NativeCameraUiSnapshot? = runCatching {
            val j = JSONObject(text)
            val generation = j.optLong("generation", 0L)
            val cameraId = j.optString("cameraId", "")
            val lensId = j.optString("lensId", "")
            val isoMin = j.optInt("sensitivityMin", 0)
            val isoMax = j.optInt("sensitivityMax", 0)
            val ssMin = j.optLong("exposureTimeMinNs", 0L)
            val ssMax = j.optLong("exposureTimeMaxNs", 0L)
            if (generation <= 0L || cameraId.isBlank() || lensId.isBlank() || isoMin <= 0 || isoMax < isoMin ||
                ssMin <= 0 ||
                ssMax < ssMin
            ) {
                return null
            }
            val den = j.optInt("evStepDenominator", 1).coerceAtLeast(1)
            NativeCameraUiSnapshot(
                generation = generation,
                cameraId = cameraId,
                lensId = lensId,
                sensitivityMin = isoMin,
                sensitivityMax = isoMax,
                exposureTimeMinNs = ssMin,
                exposureTimeMaxNs = ssMax,
                evMinSteps = j.optInt("evMinSteps", 0),
                evMaxSteps = j.optInt("evMaxSteps", 0),
                evStep = j.optInt("evStepNumerator", 0).toDouble() / den.toDouble(),
                manualExposureSupported = j.optBoolean("manualExposureSupported", false),
                shutterPrioritySupported = j.optBoolean("shutterPrioritySupported", false),
                isoPrioritySupported = j.optBoolean("isoPrioritySupported", false),
                tapAfSupported = j.optBoolean("tapAfSupported", false),
                manualFocusSupported = j.optBoolean("manualFocusSupported", false),
                focusDistanceReadoutTrustworthy = j.optBoolean("focusDistanceReadoutTrustworthy", false),
                minimumFocusDistance = j.optDouble("minimumFocusDistance", 0.0).toFloat(),
                hyperfocalDistance = j.optDouble("hyperfocalDistance", 0.0).toFloat(),
                exposureMode = j.optInt("exposureMode", 0),
                semanticExposureMode = j.optInt("semanticExposureMode", 0),
                focusMode = j.optInt("focusMode", 0),
                whiteBalanceMode = j.optInt("whiteBalanceMode", 1),
                whiteBalanceTemperatureK = j.optInt("whiteBalanceTemperatureK", 5200),
                whiteBalanceTint = j.optInt("whiteBalanceTint", 0),
                manualWhiteBalanceSupported = j.optBoolean("manualGainsSupported", false),
                supportedAwbModes = buildList {
                    val arr = j.optJSONArray("supportedAwbModes") ?: return@buildList
                    for (i in 0 until arr.length()) add(arr.optInt(i, -1))
                },
                hasAutoWbGains = j.optBoolean("hasAutoWbGains", false),
                autoWbGainR = j.optDouble("autoWbGainR", 1.0).toFloat(),
                autoWbGainG = j.optDouble("autoWbGainG", 1.0).toFloat(),
                autoWbGainB = j.optDouble("autoWbGainB", 1.0).toFloat(),
                hasAutoWbEstimate = j.optBoolean("hasAutoWbEstimate", false),
                autoWbTemperatureK = j.optInt("autoWbTemperatureK", WhiteBalanceMode.TEMP_DEFAULT_K),
                autoWbTint = j.optInt("autoWbTint", 0),
                requestedExposureTimeNs = j.optLong("requestedExposureTimeNs", 0L),
                requestedSensitivity = j.optInt("requestedSensitivity", 0),
                requestedEvSteps = j.optInt("requestedEvSteps", 0),
                requestedManualFocusNormalized = j.optDouble("requestedManualFocusNormalized", 1.0).toFloat(),
                appliedExposureTimeNs = j.nullableLong("appliedExposureTimeNs"),
                appliedSensitivity = j.nullableInt("appliedSensitivity"),
                appliedEvSteps = j.nullableInt("appliedEvSteps"),
                afState = j.nullableInt("afState"),
                appliedFocusDistance = j.nullableDouble("appliedFocusDistance")?.toFloat(),
                appliedPostRawSensitivityBoost = j.nullableInt("appliedPostRawSensitivityBoost"),
                appliedRawFps = j.nullableDouble("appliedRawFps"),
                measuredViewfinderFps = j.nullableDouble("measuredViewfinderFps"),
                tapAfActive = j.optBoolean("tapAfActive", false),
                focusRequestId = j.optLong("focusRequestId", 0L),
                videoMode = j.optBoolean("videoMode", false),
                videoPreviewFps = j.optInt("videoPreviewFps", 30),
                requestedShutterAngleDegrees = j.nullableDouble("requestedShutterAngleDegrees"),
                shutterAngleChoices = buildList {
                    val choices = j.optJSONArray("shutterAngleChoices") ?: return@buildList
                    for (index in 0 until choices.length()) {
                        val choice = choices.optJSONObject(index) ?: continue
                        val angle = choice.optDouble("degrees", Double.NaN)
                        val ns = choice.optLong("exposureTimeNs", 0L)
                        if (angle.isFinite() && angle > 0 && angle <= 360 && ns > 0)
                            add(NativeShutterAngleChoice(angle, ns))
                    }
                },
                autoWbEstimateCalibrated = j.optBoolean("autoWbEstimateCalibrated", false),
                whiteBalanceRequestId = j.optLong("whiteBalanceRequestId", 0L),
                rawCpuIngress = j.optBoolean("rawCpuIngress", false),
                profileLenses = buildList {
                    val lenses = j.optJSONArray("profileLenses") ?: return@buildList
                    for (index in 0 until lenses.length()) {
                        val lens = lenses.optJSONObject(index) ?: continue
                        val id = lens.optString("id", "")
                        if (id.isNotBlank()) add(id to lens.optString("label", ""))
                    }
                }
            )
        }.getOrNull()

        /**
         * Parses the face-detections snapshot ([[x,y,w,h,score],...],
         * display-normalized). Hand-rolled instead of org.json so it stays
         * unit-testable: local JVM tests run against android.jar stubs where
         * every JSONObject/JSONArray method throws. Malformed entries are
         * skipped, never thrown.
         */
        fun parseFaces(text: String): List<FaceDetection> {
            val trimmed = text.trim()
            if (trimmed.length < 2 || !trimmed.startsWith('[') || !trimmed.endsWith(']')) return emptyList()
            val out = mutableListOf<FaceDetection>()
            var depth = 0
            val current = StringBuilder()
            for (c in trimmed) {
                when {
                    c == '[' -> {
                        depth++
                        if (depth == 2) current.clear()
                    }
                    c == ']' -> {
                        if (depth == 2) parseFaceEntry(current.toString())?.let(out::add)
                        depth--
                    }
                    depth == 2 -> current.append(c)
                }
            }
            return out
        }

        private fun parseFaceEntry(entry: String): FaceDetection? {
            val parts = entry.split(',')
            if (parts.size != 5) return null
            val x = parts[0].toFloatOrNull() ?: return null
            val y = parts[1].toFloatOrNull() ?: return null
            val w = parts[2].toFloatOrNull() ?: return null
            val h = parts[3].toFloatOrNull() ?: return null
            val score = parts[4].trim().toIntOrNull() ?: return null
            if (x < 0f || x > 1f || y < 0f || y > 1f || w <= 0f || h <= 0f || score <= 0) return null
            return FaceDetection(x, y, w, h, score.coerceIn(1, 100))
        }
    }
}

private fun JSONObject.nullableLong(name: String): Long? = if (!has(name) || isNull(name)) null else getLong(name)

private fun JSONObject.nullableInt(name: String): Int? = if (!has(name) || isNull(name)) null else getInt(name)

private fun JSONObject.nullableDouble(name: String): Double? = if (!has(name) || isNull(name)) null else getDouble(name)
