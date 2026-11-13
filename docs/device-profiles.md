# Built-in device profiles

Camera profiles in `app/src/main/cpp/camera/profiles/*.json` are embedded at
build time. Each has a unique `id`, a display `name`, optional `description`,
`version`, `match` rules and schema-2 `lenses`.

Several profiles may match one device. Mark exactly one of those variants
with the top-level JSON boolean `"default": true`; mark the others `false`
or omit the field. The description should explain differences and prerequisites
such as root and a patched camera library.

Exact `match.models` matches take precedence over property-prefix matches.
Within the matching set, the explicit default is selected automatically on
first launch. A single matching legacy profile needs no default marker.
Multiple matches with no default, or with multiple defaults, fall back to the
generic profile instead of selecting an arbitrary variant. Host profile tests
check that overlapping model matches have exactly one default.

**Settings → Lens → Load device profile** lists the matching profiles with
their descriptions and marks the default. Loading requires confirmation and
replaces the current lens list, including custom lens settings. Cancelling
keeps the list unchanged. Loading the default restores the device defaults;
loading another variant saves its lenses as the user's editable configuration.
No other settings sections are replaced.

**Reset to device default** always restores the automatically selected default
profile after confirmation, without opening the picker. It is disabled while
the device defaults are already in use.
Profiles requiring root are opt-in; choosing one does not install a library
patch or grant root access.
