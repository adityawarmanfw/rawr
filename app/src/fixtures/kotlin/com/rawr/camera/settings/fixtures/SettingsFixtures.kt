package com.rawr.camera.settings.fixtures

import com.rawr.camera.model.ControlSurfaceStyle
import com.rawr.camera.model.ImageToneState
import com.rawr.camera.settings.model.*

/** Representative settings capability and state data for previews and tests. */
object SettingsFixtures {
    const val DEFAULT_OUTPUT_COLOR_SPACE_ID = "output.srgb"
    const val DEFAULT_TRANSFER_FUNCTION_ID = "transfer.srgb"

    fun capabilities(fullySupported: Boolean = true) = SettingsCapabilities(
        saveLocationChoices =
            listOf(
                ChoiceCandidate(
                    "storage.dcim_camera",
                    "Internal storage / DCIM/Camera",
                    "Sample storage target for UI review"
                ),
                ChoiceCandidate(
                    "storage.pictures_raw",
                    "Pictures / RAW Camera",
                    "Sample storage target for UI review"
                )
            ),
        falseColorPresets =
            listOf(
                FalseColorPresetCandidate(
                    "monitor.falsecolor.standard",
                    "Standard",
                    "General exposure visualization"
                ),
                FalseColorPresetCandidate(
                    "monitor.falsecolor.skin",
                    "Skin / Portrait",
                    "Fixture mapping emphasizing midtone placement"
                ),
                FalseColorPresetCandidate(
                    "monitor.falsecolor.log",
                    "Extended Range",
                    "Fixture mapping for broad tonal distribution"
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
                ChoiceCandidate("jpeg.444", "4:4:4"),
                ChoiceCandidate("jpeg.422", "4:2:2"),
                ChoiceCandidate("jpeg.420", "4:2:0")
            ),
        dngCompressionChoices =
            listOf(
                ChoiceCandidate("dng.lossless", "Lossless"),
                ChoiceCandidate("dng.uncompressed", "Uncompressed")
            ),
        maxPostGainChoices =
            listOf(
                ChoiceCandidate("gain.100", "1x"),
                ChoiceCandidate("gain.200", "2x"),
                ChoiceCandidate("gain.400", "4x")
            ),
        autoMinFpsChoices =
            listOf(
                ChoiceCandidate("fps.30", "30 fps"),
                ChoiceCandidate("fps.24", "24 fps"),
                ChoiceCandidate("fps.20", "20 fps"),
                ChoiceCandidate("fps.15", "15 fps"),
                ChoiceCandidate("fps.12", "12 fps"),
                ChoiceCandidate("fps.8", "8 fps")
            ),
        oisSupported = fullySupported,
        cameraInformation =
            CameraInformation(
                displayName = if (fullySupported) "Rear camera · fixture" else "Alternate camera · fixture",
                lensSummary = if (fullySupported) "Primary camera fixture" else "Camera fixture without OIS",
                sensorSummary = "RAW-capable sensor fixture",
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
                peakingSensitivityId = "peaking.normal",
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
                            imageTone = SettingApplicationState(
                                values.imageTone,
                                SettingApplicationStatus.Applied
                            )
                        )
                )
        )
    }
}
