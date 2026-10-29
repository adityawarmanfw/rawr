package com.rawr.camera.settings.architecture

import com.rawr.camera.model.ImageToneState
import com.rawr.camera.settings.model.*

/** Shared immutable projections used by feature reducers and the controller. */
internal fun SettingsUiState.withValues(next: SettingsValues): SettingsUiState = copy(
    values = next,
    runtime = runtime.copy(effective = capabilities.resolveEffective(next))
)

internal fun SettingsUiState.withApplication(
    ois: SettingApplicationState<Boolean> = runtime.application.ois,
    antiFlicker: SettingApplicationState<AntiFlicker> = runtime.application.antiFlicker,
    imageTone: SettingApplicationState<ImageToneState> = runtime.application.imageTone
): SettingsUiState = copy(
    runtime =
        runtime.copy(
            application = SettingsApplicationState(ois, antiFlicker, imageTone)
        )
)

internal fun SettingsUiState.withImageToneAcknowledged(): SettingsUiState = withApplication(
    imageTone = SettingApplicationState(values.activeImageTone(), SettingApplicationStatus.Applied)
)
