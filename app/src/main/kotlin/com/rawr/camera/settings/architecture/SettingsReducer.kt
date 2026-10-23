package com.rawr.camera.settings.architecture

import com.rawr.camera.settings.model.*

object SettingsReducer {
    private const val MAX_BACK_STACK = 30

    fun reduce(state: SettingsUiState, action: SettingsPresentationAction): SettingsUiState {
        val p = state.presentation
        val next =
            when (action) {
                is OpenSection -> {
                    push(p, SettingsDestination.Section(action.section))
                }

                NavigateBack -> {
                    val stack = p.backStack
                    if (stack.isNotEmpty()) {
                        p.copy(destination = stack.last(), backStack = stack.dropLast(1))
                    } else {
                        p.copy(
                            destination =
                                when (p.destination) {
                                    SettingsDestination.Home -> SettingsDestination.Home
                                    is SettingsDestination.Section -> parentSection(p.destination.section)
                                    is SettingsDestination.ChoiceSelector -> parentDestination(p.destination)
                                    is SettingsDestination.FilmSimSubPage ->
                                        SettingsDestination.Section(SettingsSection.FilmSim)
                                    is SettingsDestination.FilmSimDetailPage ->
                                        SettingsDestination.FilmSimSubPage(p.destination.detail.parent)
                                    is SettingsDestination.LensEditor ->
                                        SettingsDestination.Section(SettingsSection.Lens)
                                }
                        )
                    }
                }

                is SelectImageToneTab -> {
                    p.copy(imageToneTab = action.tab)
                }

                is OpenSelector -> {
                    push(p, SettingsDestination.ChoiceSelector(action.kind))
                }

                is OpenFilmSimSubPage -> {
                    push(p, SettingsDestination.FilmSimSubPage(action.section))
                }

                is OpenFilmSimDetail -> {
                    push(p, SettingsDestination.FilmSimDetailPage(action.detail))
                }

                is RequestReset -> {
                    p.copy(pendingReset = action.target)
                }

                DismissReset -> {
                    p.copy(pendingReset = null)
                }

                is OpenLensEditor -> {
                    push(p, SettingsDestination.LensEditor(action.lensName))
                }
            }
        return state.copy(presentation = next)
    }

    private fun push(
        presentation: SettingsPresentationState,
        destination: SettingsDestination
    ): SettingsPresentationState {
        if (destination == presentation.destination) return presentation
        return presentation.copy(
            destination = destination,
            backStack = (presentation.backStack + presentation.destination).takeLast(MAX_BACK_STACK)
        )
    }

    private fun parentSection(section: SettingsSection): SettingsDestination = when (section) {
        SettingsSection.GpuDriver, SettingsSection.ZeroCopy -> {
            SettingsDestination
                .Section(
                    SettingsSection.Experimental
                )
        }

        SettingsSection.Ois, SettingsSection.Multiframe,
        SettingsSection.Exposure, SettingsSection.Lens -> {
            SettingsDestination.Section(SettingsSection.Capture)
        }

        SettingsSection.LutProfile -> {
            SettingsDestination.Section(SettingsSection.ImageTone)
        }

        SettingsSection.ImageTone, SettingsSection.DynamicRange, SettingsSection.Demosaic, SettingsSection.HighlightReconstruction,
        SettingsSection.Defringe, SettingsSection.Denoise, SettingsSection.FilmSim,
        SettingsSection.LensShading, SettingsSection.Distortion -> {
            SettingsDestination.Section(
                SettingsSection.Image
            )
        }

        SettingsSection.Jpeg -> {
            SettingsDestination.Home
        }

        SettingsSection.Monitoring, SettingsSection.ControlStyle -> {
            SettingsDestination.Section(
                SettingsSection.DisplayControls
            )
        }

        SettingsSection.InternalLogging -> {
            SettingsDestination.Section(SettingsSection.Debug)
        }

        else -> {
            SettingsDestination.Home
        }
    }

    private fun parentDestination(destination: SettingsDestination): SettingsDestination =
        when ((destination as? SettingsDestination.ChoiceSelector)?.kind) {
            ChoiceSelectorKind.SaveLocation -> {
                SettingsDestination.Section(SettingsSection.Storage)
            }

            ChoiceSelectorKind.FalseColorPreset, ChoiceSelectorKind.PeakingSensitivity -> {
                SettingsDestination.Section(
                    SettingsSection.Monitoring
                )
            }

            ChoiceSelectorKind.OutputColorSpace, ChoiceSelectorKind.TransferFunction, ChoiceSelectorKind.JpegChromaSubsampling -> {
                SettingsDestination
                    .Section(
                        SettingsSection.Jpeg
                    )
            }

            ChoiceSelectorKind.DngCompression -> {
                SettingsDestination.Section(SettingsSection.Dng)
            }

            ChoiceSelectorKind.MaxPostGain, ChoiceSelectorKind.AutoMinFps -> {
                SettingsDestination.Section(
                    SettingsSection.Exposure
                )
            }

            ChoiceSelectorKind.FilmOutputSpace -> {
                SettingsDestination.FilmSimSubPage(FilmSimSection.Output)
            }

            null -> {
                SettingsDestination.Home
            }
        }
}
