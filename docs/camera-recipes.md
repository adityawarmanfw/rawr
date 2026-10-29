# Lens profile fields

A lens profile says where a lens is, how its streams and session are built, what every request
carries, and how its image is read. Everything below is optional; leaving a field out gives the app's
own behaviour. Profiles are JSON: built-in ones under `app/src/main/cpp/camera/profiles/`, user ones
through the lens editor (stored and backed up in the same form).

Verification status is stated per section. "Host tests" means `tests/native` and the Kotlin unit
tests. Unless a section says otherwise it has not run on a phone.

## Schema

A profile has `"schema": 2`, and each lens is grouped by what it configures:

```json
{"id": "user", "schema": 2, "lenses": [{
  "id": "F35",
  "camera":  {"id": "3", "physical": ""},
  "streams": {
    "raw":       {"format": "RAW16", "width": 4080, "height": 3064, "reader": {...}, "levels": {...}},
    "capture":   {"format": "RAW16", "width": 16320, "height": 12256, "reader": {...}, "burst": {...}},
    "companion": {"mode": "auto"},
    "auxJpeg":   {"width": 4096, "height": 3072}
  },
  "session": {"backend": "auto", "gate": {"name": "vivoFullSize"}, "quirks": [...]},
  "request": {"defaults": "rawr"},
  "keys":    [{"tag": "...", "type": "int32", "scope": "session", "values": [1], "enabled": true}],
  "image":   {"levels": {...}, "cfa": {...}, "distortion": {...}, "mirror": "none", "dcp": {...}, "exposure": {...}}
}]}
```

A file without `"schema"` is schema 1 (flat fields beside a few blocks, as written before this
layout). Each schema-1 lens is upgraded into the layout above and then read by the same code, in the
native parser and in the app, so old settings and backups keep working and are validated the same
way. The upgrade drops capture-reader usage, burst and capture levels from a lens without a capture
stream; they only ever applied to that stream. Any other schema number is refused.

Both the app and the native writer write schema 2. `tests/fixtures/camera/builtin_routes.txt` holds
every field of every built-in lens as parsed; the built-in files must keep producing it.

## Camera

| Field | Meaning |
|---|---|
| `camera.id` | Camera2 id to open: a logical camera, or a hidden/physical id opened directly. |
| `camera.physical` | Physical sub-camera of `camera.id` to stream from; outputs are tied to it. |

## Streams

### RAW stream (`streams.raw`)

| Field | Default | Meaning |
|---|---|---|
| `format`, `width`, `height` | RAW16, largest | RAW10/RAW16; a zero size means the largest. |
| `unlisted` | false | Use the size as written instead of negotiating it against the advertised sizes. |
| `useCase` | 0 | Stream use case (Java backend only). |
| `reader.usage` | chosen by the usage ladder | Fixed AHardwareBuffer usage: `gpuDataBuffer`, `none`, or hex such as `0x100003`. Used as written. |
| `reader.maxImages` | 6 | Images the reader holds, 6–16. |
| `reader.probe` | false | Check a fixed usage with `AHardwareBuffer_isSupported` first; CPU-only when refused. |
| `reader.demote` | false | Let a silent fixed reader fall back like the app's own choice. Without it a fixed reader stays out of the usage ladder. |
| `levels` | the lens's | Black/white levels for this stream only. |

### Capture stream (`streams.capture`)

A second RAW stream that only full-size still requests fill (vendor full-resolution modes); the RAW
stream is then the binned viewfinder.

| Field | Default | Meaning |
|---|---|---|
| `format`, `width`, `height` | required | The full-size readout. |
| `useCase` | 0 | Stream use case (Java backend only). |
| `reader.usage` | GPU data buffer | Its reader's usage. |
| `burst.max` | 0 (the app's cap) | Most full-size stills one multi-frame photo may request (1 = none). |
| `burst.sequential` | false | Request the stills one at a time, each after the previous arrived. |
| `levels` | the lens's | Black/white levels for full-size stills. |

### Companion (`streams.companion`)

A small second output next to RAW. Some HALs will not start RAW alone, or will not produce a real
full-size frame without it.

| Field | Default | Meaning |
|---|---|---|
| `mode` | `auto` | `auto`: added by the app's rule (unverified devices, capture-stream lenses), sized to the RAW. `fixed`: always configured at `width`×`height`, part of the mode, never dropped by recovery. `off`: never, including the capture-stream one. |
| `width`, `height` | — | The size, `fixed` only. |
| `fallback` | `base` | `auto` only, when no output has the RAW's shape: `base` (smallest of at least 320×240, any shape), `nearest` (closest shape), `none` (no companion). |
| `format` | `yuv` | `yuv` or `private`. |
| `targeted` | true | Whether the repeating request writes to it. |
| `detach` | true for `auto`, false for `fixed` | Stop writing to it after the first frame paired with its metadata. |
| `usage`, `maxImages` | per format, 2 | Its reader's usage and image count (2–16). |
| `physical` | follows the route | Tie it to the route's physical camera or not. |
| `useCase` | 0 | Stream use case (Java backend only). |

### Aux JPEG (`streams.auxJpeg`)

An extra JPEG output configured with the session and never targeted: `width`, `height`, `usage`,
`useCase`.

## Session (`session`)

| Field | Default | Meaning |
|---|---|---|
| `backend` | `auto` | Camera API; see below. |
| `opMode` | 0 | Camera2 session operating mode (Java backend only). 1 and 2 are refused. |
| `params` | `cadence` | What the session parameters carry. `cadence`: the fps range and session-scope keys. `none`: a plain session, no parameters; refused with an enabled session-scope key. `full`: the whole repeating request (defaults, request-scope keys, control state); the framework may drop keys the HAL does not list as session keys (`advertised=` in the key log). |
| `template` | preview viewfinder, still-capture stills | `still` or `preview` for the session parameters, the viewfinder and the stills alike. |
| `fps` | 0 (the app's cadence) | Fixed AE target fps on the session and every request. |
| `startupDelayMs` | 0 | Wait after the session is configured before the first request (0–5000). |
| `gate.name`, `gate.hold` | none; `closed` on a capture-stream lens, else `configured` | Vendor state the app holds while the device is open; `hold` says whether it is released once the session is configured or kept until the device closes. |
| `stopRepeatingForStill` | false | Pause the viewfinder for the still sequence and restore it afterwards. |
| `quirks` | none | Provider quirks this lens avoids; see below. |

How a full-size photo is taken is derived, not set: a lens with a capture stream, a companion it
writes to and an enabled `decision` key uses the decision frame (one low-ISO viewfinder frame lets the
HAL switch to full size, then the viewfinder pauses for the stills); otherwise `stopRepeatingForStill`
pauses the viewfinder, and without it the still goes out beside the running viewfinder. The lens
editor shows the result as "Photo sequence".

## Requests and keys

| Field | Default | Meaning |
|---|---|---|
| `request.defaults` | `rawr` | `minimal` leaves distortion correction, hot-pixel mode, the hot-pixel map, face detection and the AE priority tag to the template. Controls, the shading map and the lens's keys stay. A lens on `minimal` gets no HAL hot-pixel list in its metadata. |
| `keys[].scope` | `session` | `session`, `request`, `sessionAndRequest` (the same value in the session parameters and on every request), `still` (full-size stills only), `decision` (the one low-ISO frame sent before full-size stills). |
| `keys[].type` | `int32` | `byte`, `int32`, `int64`, `float`, `double`. A HAL tag has one type; if the declared one is refused the other types are tried and the one accepted is remembered (see the `attempts=` log field). |

## Image (`image`)

`levels` (static or dynamic black/white), `cfa` (block, order, live and photo remosaic), `distortion`
(rational model on its own frame size), `mirror`, `dcp` (camera profile file) and `exposure` (requests
beyond the advertised ISO and shutter ranges).

## Provider quirks

Behaviours a camera provider needs avoided, named so a profile says why a lens is special.
`session.quirks` on a lens adds to the quirks of the device (`quirks` at the root of a built-in
profile, which apply to every lens on a matching device, user lenses included).

| Quirk | Effect |
|---|---|
| `afTapHold` | A tap's focus holds until the next tap; face-steered AF is off. |
| `noHalPriorityAe` | The camera's shutter/ISO priority AE is not used; the app's AE handles S/I. |
| `noTapAf` | Tap AF and AF regions are unavailable. |
| `requireFpsRange` | Every request carries an AE fps range, even with AE off. |
| `noAutoCompanion` | The rule-based companion is not added. |

Presets keep imported and user-edited profiles safe: a lens with a vendor gate implies `afTapHold`;
an `unlisted` RAW stream implies `noHalPriorityAe`, `requireFpsRange` and `noAutoCompanion`
(not `noTapAf`: add it to the lens when a camera crashes on AF regions).

### Gate presets

A vendor gate can bring per-capture data with it. `vivoFullSize` and `vivoFullSizeVcf` add three still-scope keys whose
value is the capture's millisecond stamp (`vivo.control.requestId`, `frameFlag`; `globalCaptureId` is
the stamp plus 5) and four result tags read back after each full-size still, one of which
(`vcf.parameter.seamlessMode`) is kept on the frame to show whether the HAL really switched sensor
mode. These are applied only when the camera exposes the first tag. Verified on a V2562: the 200 MP
still request carries the same tags and the same stamp relation as before they became data.

## Camera API (backend)

| Field | Meaning |
|---|---|
| `session.opMode` | Camera2 session operating mode (`SessionConfiguration` type). 0 = normal; 1 and 2 are refused. |
| `streams.*.useCase` | Stream use case of an output (`raw`, `capture`, `companion`, `auxJpeg`); 0 = not set. Standard values are 1–6; anything else is vendor-defined. |
| `session.backend` | `auto`, `ndk` or `java`. |

The NDK has no way to set an operating mode or a stream use case. `auto` therefore resolves to the
Java camera API exactly when one of them is set, and an explicit `ndk` together with one is rejected
when the profile is parsed. `java` can also be chosen for a lens without either, to compare the two
APIs on the same route.

A lens with a vendor gate or with `stopRepeatingForStill` cannot use the Java backend, whether it
asks for `java` or gets it from an operating mode or use case: the vivo full-size sequence is proven
on the NDK only. The profile is refused with the reason.

On the Java backend Kotlin opens the camera and configures the session (`SessionConfiguration` with
the operating mode, `OutputConfiguration.setStreamUseCase` per output, session parameters). Native
code keeps everything else: the readers (their windows become `Surface`s), the requests, the
controls, keys and stills. A request is built as data in native code and turned into a
`CaptureRequest` on each submit; each entry is set by its Camera2 key name with the primitive type of
the tag (a vendor key by the name it was resolved from). Results come back through
`ACameraMetadata_fromCameraMetadata`, so result handling is the same on both APIs. The log says
`CAMERA_BACKEND lens=… backend=java available=1` when a lens opens this way.

Differences to know when comparing bundles:

- `CAMERA_REQUEST_DUMP` of a Java session lists only what the app set. The template defaults are
  added by Camera2 when the request is built and are not visible to native code.
- A Java request fails as a whole when it carries a tag with no Camera2 key name (an unlisted vendor
  tag given by number). The NDK would set it. Name such keys instead.

Checked on the V2562 with the Java backend:

- camera 4 (RAW16 4096×3072): viewfinder, a single JPEG, a forced companion that detaches after the
  start, a 30-frame multiframe from the viewfinder ring, a 29-frame 0.6 s stack (a burst of submitted
  requests, each result matched to its request), 4K 24 fps video (193 frames in 8.08 s), and lens
  switches between Java and NDK lenses with the F35 200 MP still afterwards;
- camera 0 with physical camera 4: frames pair with the physical camera's results and a JPEG is
  written;
- with a viewfinder use case of 1 the camera service reports `Stream use case: 1` on the RAW stream.

Results reach native code through JNI and the Camera2 callback thread, and the pairer keeps one
image waiting for its result. On camera 4 over about 30 s of viewfinder this cost nothing measurable:
the NDK dropped 1 of 840 frames and Java 0 of 960, with no `PAIRER_PRESSURE` line on either.

`adb shell setprop debug.rawr.camera_backend java|ndk` runs every lens on one API, for comparing the two
on the same route (`CAMERA_BACKEND_OVERRIDE` in the log).

`adb shell setprop debug.rawr.camera_profile file:<path>` runs a profile JSON (schema 1 or 2) from a
path the app can read, in place of the built-in and the user's lenses, so an exact recipe can be run
without the lens editor. Put the file in the app's own storage, e.g.
`adb shell "run-as <package> sh -c 'cat > files/debug_camera_profile.json'" < profile.json` and
`file:/data/user/0/<package>/files/debug_camera_profile.json`. A file that does not parse is logged
(`CAMERA_PROFILE_FILE_REJECTED`) and the built-in profile is used. A gated lens stays on the NDK and a lens with
an operating mode or use case stays on Java. Clear it with `setprop debug.rawr.camera_backend ''`.

Not checked: a vendor operating mode on a lens (only a debug probe session accepted one), session
params `full` on Java, the aux JPEG, and stills through a separate capture stream. The last one cannot
be checked on this phone: without the vendor gate a capture-stream still times out on the NDK as well,
and gated lenses stay on the NDK.

## Diagnostics

| Line | Content |
|---|---|
| `CAMERA_SESSION_RECIPE` | One line per started session: route, RAW stream and aspect, template, session-parameter fps range, session params policy, request defaults, backend, key counts per scope, companion (format, size, targeting, source, shape, images, detach), aux JPEG, capture stream, reader usage, startup delay, gate and hold, still strategy; `opMode=` and `useCases=` only when set. |
| `CAMERA_REQUEST_DUMP kind=session_start` / `repeating_start` | Every entry of the session parameters and of the repeating request, right after the first submit, for lenses that carry keys or recipe fields. Fields: tag, type, count, values, `typeName`. Compare two builds or devices with the same lens. |
| `CAMERA_KEY_INJECTION … attempts=` | Each setter call made for a vendor key, with status; `(cached)` marks a first try from the memory of an earlier success. |
| `CAMERA_PROFILE_REJECTED` | A lens profile the app sent was refused (reason, and the profile that stays in force). The viewfinder shows "Lens settings not applied". |

The still strategy in the recipe line (`direct`, `decisionFrame`, `stopRepeatThenStill`) is derived
from the capture stream, decision-scope keys, the companion and `stopRepeatingForStill`; it is not
configurable.

## Where the fields are in the lens editor

The editor follows the profile: Lens and camera, RAW stream, Full-size capture stream, Companion
stream, Aux JPEG stream, Session (with the derived photo sequence), Camera workarounds, Requests and
keys, then Image, Black & white levels, Sensor layout and Exposure range. Every field above has a
control except the per-stream levels and the distortion model, which come from built-in profiles and
backups.
