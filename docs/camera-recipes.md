# Lens profile recipe fields

A lens profile says three things about a camera: **where** it is (route), **how its session is
built** (session recipe) and **what every frame asks of it** (request policy). Everything below is
optional; leaving a field out gives the behaviour the app had before the field existed. Fields are
JSON in a lens profile (built-in profiles under `app/src/main/cpp/camera/profiles/`, user profiles
and backups through the lens editor).

Verification status is stated per section. "Host tests" means `tests/native` and the Kotlin unit
tests; "arm64 build" means the Android native build compiles. Unless a section says otherwise it has
not run on a phone.

## Route

| Field | Meaning |
|---|---|
| `cameraId` | Camera2 id to open: a logical camera, or a hidden/physical id opened directly. |
| `physicalCameraId` | Physical sub-camera of `cameraId` to stream from; outputs are tied to it. |
| `stream` | Viewfinder RAW: `format` (RAW10/RAW16), `width`/`height` (0 = largest), `unlisted` (use the size as written, do not negotiate it against the advertised sizes), `useCase`. |

## Session recipe

| Field | Default | Meaning |
|---|---|---|
| `template` | `preview` (viewfinder) / `still` (full-size stills) | Request template for the session parameters, the viewfinder and the stills. |
| `fps` | 0 (the app's cadence) | Fixed AE target fps on the session and every request. |
| `startupDelayMs` | 0 | Wait after the session is configured before the first request (0–5000). |
| `session.params` | `cadence` | What the session parameters carry. `cadence`: the fps range and session-scope keys. `none`: a plain `createCaptureSession` with no session parameters. `full`: the whole repeating request (defaults, request-scope keys, control state); the framework may drop keys the HAL does not list as session keys (the `advertised=` field of the key log shows where the HAL lists a key). `none` with an enabled session-scope key is rejected. |
| `vendorGate`, `gateHold` | none; `closed` on a capture-stream lens, else `configured` | Vendor state the app holds while the device is open. `gateHold` (`configured` or `closed`, needs a gate) says whether it is released once the session is configured or kept until the device closes. |
| `stopRepeatingForStill` | false | Pause the viewfinder for the still sequence and restore it afterwards. |
| `captureStream` | none | A second, still-only RAW stream (full-size modes). With `captureBurst` / `captureBurstSequential`. |

### Companion and aux JPEG

A companion is a small second output next to RAW. Some HALs will not start RAW alone, or will not
produce a real full-size frame without it. `companion` without a size tunes the companion the app
adds by its own rule and does not force one on; with a size it is part of the mode and is never
dropped by recovery.

| Field | Default | Meaning |
|---|---|---|
| `companion.width`, `height` | automatic | Size. Automatic picks the smallest advertised YUV whose shape matches the RAW's (within 2%), at least 320 wide. |
| `companion.fallback` | `base` | Automatic size only, when no output has the RAW's shape: `base` (smallest of at least 320×240, any shape), `nearest` (closest shape), `none` (no companion). |
| `companion.format` | `yuv` | `yuv` or `private`. |
| `companion.targeted` | true | Whether the repeating request writes to it. |
| `companion.detach` | true for an automatic companion, false for a sized one | Drop it from the repeating request after the first accepted frame. |
| `companion.usage`, `maxImages` | per format, 2 | Its reader's AHardwareBuffer usage and image count (2–16). |
| `companion.physical` | follows the route | Tie it to the route's physical camera or not. |
| `companion.useCase` | 0 | Stream use case (Java backend only). |
| `disableCompanion` | false | Never add a companion, including the capture-stream one. |
| `auxJpeg` | none | An extra JPEG output configured with the session and never targeted. `width`/`height`, `usage`, `useCase`. |

### RAW reader

| Field | Default | Meaning |
|---|---|---|
| `readerUsage`, `captureReaderUsage` | chosen by the reader usage ladder / GPU data buffer | Fixed AHardwareBuffer usage (`gpuDataBuffer`, `none`, or hex such as `0x100003`). A fixed usage is used as written. |
| `reader.maxImages` | 6 | Images the viewfinder RAW reader holds, 6–16. |
| `reader.probe` | false | Check a fixed usage with `AHardwareBuffer_isSupported` first and fall back to CPU-only when refused. |
| `reader.demote` | false | Let a silent fixed reader fall back to CPU-only like the app's own choice does. Without it a fixed reader does not take part in the usage ladder. |

## Request policy

| Field | Default | Meaning |
|---|---|---|
| `request.defaults` | `rawr` | `minimal` leaves distortion correction, hot-pixel mode, the hot-pixel map, face detection and the AE priority tag to the template. Controls, the shading map and the lens's keys stay. A lens on `minimal` gets no HAL hot-pixel list in its metadata. |
| `keys[].scope` | `session` | `session`, `request`, `sessionAndRequest` (the same value in the session parameters and on every request), `still` (full-size stills only), `decision` (the one low-ISO frame sent before full-size stills). |
| `keys[].type` | `int32` | `byte`, `int32`, `int64`, `float`, `double`. A HAL tag has one type; if the declared one is refused the other types are tried and the one accepted is remembered (see the `attempts=` log field). |

## Provider quirks

Behaviours a camera provider needs avoided, named so a profile says why a lens is special.
`quirks` on a lens adds to the quirks of the device (`quirks` on a built-in profile, which apply to
every lens on a matching device, user lenses included).

| Quirk | Effect |
|---|---|
| `afTapHold` | A tap's focus holds until the next tap; face-steered AF is off. |
| `noHalPriorityAe` | The camera's shutter/ISO priority AE is not used; the app's AE handles S/I. |
| `noTapAf` | Tap AF and AF regions are unavailable. |
| `requireFpsRange` | Every request carries an AE fps range, even with AE off. |
| `noAutoCompanion` | The rule-based companion is not added. |

Presets keep imported and user-edited profiles safe: a lens with a vendor gate implies `afTapHold`;
an `unlisted` stream implies the other four.

### Gate presets

A vendor gate can bring per-capture data with it. `vivoFullSize` adds three still-scope keys whose
value is the capture's millisecond stamp (`vivo.control.requestId`, `frameFlag`; `globalCaptureId` is
the stamp plus 5) and four result tags read back after each full-size still, one of which
(`vcf.parameter.seamlessMode`) is kept on the frame to show whether the HAL really switched sensor
mode. These are applied only when the camera exposes the first tag. Verified on a V2562: the 200 MP
still request carries the same tags and the same stamp relation as before they became data.

## Camera API (backend)

| Field | Meaning |
|---|---|
| `session.opMode` | Camera2 session operating mode (`SessionConfiguration` type). 0 = normal; 1 and 2 are refused. |
| `*.useCase` | Stream use case of an output (`stream`, `captureStream`, `companion`, `auxJpeg`); 0 = not set. Standard values are 1–6; anything else is vendor-defined. |
| `backend` | `auto`, `ndk` or `java`. |

The NDK has no way to set an operating mode or a stream use case. `auto` therefore resolves to the
Java camera API exactly when one of them is set, and an explicit `ndk` together with one is rejected
when the profile is parsed. **There is no Java backend in this build.** A lens that resolves to it
does not start: the log says `CAMERA_BACKEND lens=… backend=java available=0` and the viewfinder
shows the lens-unavailable notice, rather than running the lens without the mode it asked for.

## Diagnostics

| Line | Content |
|---|---|
| `CAMERA_SESSION_RECIPE` | One line per started session: route, RAW stream and aspect, template, session-parameter fps range, session params policy, request defaults, backend, key counts per scope, companion (format, size, targeting, source, shape, images, detach), aux JPEG, capture stream, reader usage, startup delay, gate and hold, still strategy. |
| `CAMERA_REQUEST_DUMP kind=session_start` / `repeating_start` | Every entry of the session parameters and of the repeating request, right after the first submit, for lenses that carry keys or recipe fields. Fields: tag, type, count, values, `typeName`. Compare two builds or devices with the same lens. |
| `CAMERA_KEY_INJECTION … attempts=` | Each setter call made for a vendor key, with status; `(cached)` marks a first try from the memory of an earlier success. |
| `CAMERA_PROFILE_REJECTED` | A lens profile the app sent was refused (reason, and the profile that stays in force). The viewfinder shows "Lens settings not applied". |

The still strategy in the recipe line (`direct`, `decisionFrame`, `stopRepeatThenStill`) is derived
from the capture stream, decision-scope keys, the companion and `stopRepeatingForStill`; it is not
configurable.

## What is not in the lens editor

The editor has the session policy, request defaults, gate hold, quirks, camera API, operating mode
and stream use cases ("Session and requests") next to the existing full-resolution group. The
companion, aux JPEG and RAW reader fields above are carried by profile backups and the codec but have
no editor controls yet.
