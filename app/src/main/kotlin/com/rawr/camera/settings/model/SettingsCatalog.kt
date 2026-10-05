package com.rawr.camera.settings.model

import com.rawr.camera.model.ControlSurfaceStyle
import com.rawr.camera.model.ImageToneState

/**
 * Static capability catalog and initial-state factory.
 *
 * Catalog lists (choices, presets, specs) and the default SettingsUiState
 * assembly live here. Persisted value types live in SettingsModels;
 * DataStore IO lives in preferences/.
 */

object SettingsCatalog {
    const val DEFAULT_OUTPUT_COLOR_SPACE_ID = "output.srgb"
    const val DEFAULT_TRANSFER_FUNCTION_ID = "transfer.srgb"

    fun capabilities(fullySupported: Boolean = true) = SettingsCapabilities(
        saveLocationChoices =
            listOf(
                ChoiceCandidate("storage.dcim_camera", "Internal storage / DCIM/Camera", "Default storage target"),
                ChoiceCandidate("storage.pictures_raw", "Pictures / RAW Camera", "Default storage target")
            ),
        falseColorPresets =
            listOf(
                FalseColorPresetCandidate(
                    "monitor.falsecolor.standard",
                    "Standard",
                    "General exposure visualization"
                )
            ),
        peakingSensitivityChoices =
            listOf(
                ChoiceCandidate("peaking.low", "Low"),
                ChoiceCandidate("peaking.normal", "Normal"),
                ChoiceCandidate("peaking.high", "High")
            ),
        outputColorSpaces = listOf(ChoiceCandidate("output.srgb", "sRGB / Rec. 709")),
        transferFunctions = listOf(ChoiceCandidate("transfer.srgb", "sRGB")),
        jpegChromaSubsamplingChoices =
            listOf(
                ChoiceCandidate("jpeg.444", "4:4:4", "Maximum chroma detail / largest files"),
                ChoiceCandidate("jpeg.422", "4:2:2", "Balanced chroma detail and size"),
                ChoiceCandidate("jpeg.420", "4:2:0", "Smallest files / broadest compatibility")
            ),
        dngCompressionChoices =
            listOf(
                ChoiceCandidate("dng.lossless", "Lossless", "Compressed DNG, smaller files"),
                ChoiceCandidate("dng.uncompressed", "Uncompressed", "Legacy layout, largest files")
            ),
        maxPostGainChoices =
            listOf(
                ChoiceCandidate("gain.100", "1x", "No post-RAW lift; darkest but never boosted"),
                ChoiceCandidate("gain.200", "2x", "Up to +1EV linear, then 1EV soft knee"),
                ChoiceCandidate("gain.400", "4x", "Up to +2EV linear, then 1EV soft knee")
            ),
        autoMinFpsChoices =
            listOf(
                ChoiceCandidate("fps.30", "30 fps", "Template default; shortest exposure, most boost"),
                ChoiceCandidate("fps.24", "24 fps"),
                ChoiceCandidate("fps.20", "20 fps"),
                ChoiceCandidate("fps.15", "15 fps", "Longer exposure preferred over digital boost"),
                ChoiceCandidate("fps.12", "12 fps"),
                ChoiceCandidate("fps.8", "8 fps", "Longest exposure; motion blur risk")
            ),
        oisSupported = fullySupported,
        cameraInformation =
            CameraInformation(
                displayName = if (fullySupported) "Rear camera" else "Alternate camera",
                lensSummary = if (fullySupported) "Primary camera" else "Camera without OIS",
                sensorSummary = "RAW-capable sensor",
                capabilitySummary = if (fullySupported) "OIS · AF · MF" else "AF · MF"
            )
    )

    fun initialState(
        fullySupported: Boolean = true,
        controlSurfaceStyle: ControlSurfaceStyle = ControlSurfaceStyle.Basic
    ): SettingsUiState {
        val c = capabilities(fullySupported)
        val values =
            SettingsValues(
                saveLocationId = c.saveLocationChoices.first().id,
                falseColorPresetId = c.falseColorPresets.first().id,
                peakingSensitivityId = "peaking.high",
                imageTone =
                    ImageToneState(
                        outputColorSpaceId = DEFAULT_OUTPUT_COLOR_SPACE_ID,
                        transferFunctionId = DEFAULT_TRANSFER_FUNCTION_ID
                    ),
                controlSurfaceStyle = controlSurfaceStyle
            )
        return SettingsUiState(
            capabilities = c,
            values = values,
            runtime =
                SettingsRuntimeState(
                    effective = c.resolveEffective(values),
                    application =
                        SettingsApplicationState(
                            ois =
                                if (c.oisSupported) {
                                    SettingApplicationState(
                                        values.oisEnabledPreference,
                                        SettingApplicationStatus.Applied
                                    )
                                } else {
                                    SettingApplicationState(status = SettingApplicationStatus.Unsupported)
                                },
                            antiFlicker =
                                SettingApplicationState(
                                    values.antiFlicker,
                                    SettingApplicationStatus.Applied
                                ),
                            locationTagging =
                                SettingApplicationState(
                                    values.locationTagging,
                                    SettingApplicationStatus.Applied
                                ),
                            imageTone = SettingApplicationState(
                                values.imageTone,
                                SettingApplicationStatus.Applied
                            )
                        )
                )
        )
    }
}
