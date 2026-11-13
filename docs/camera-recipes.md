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
    "raw":       {"format": "RAW16", "width": 4080, "height": 3064, "buffers": {...}, "levels": {...}},
    "photo":     {"format": "RAW16", "width": 16320, "height": 12256, "buffers": {...}, "mergeFrameLimit": 3},
    "companion": {"mode": "auto"},
    "auxJpeg":   {"width": 4096, "height": 3072}
  },
  "session": {"cameraApi": "auto", "gate": {"name": "vivoFullSize"}, "workarounds": [...]},
  "request": {"defaults": "rawr"},
  "vendorKeys": [{"tag": "...", "type": "int32", "scope": "session", "values": [1], "enabled": true}],
  "image":   {"levels": {...}, "sensorLayout": {...}, "distortion": {...}, "mirror": "none", "dcp": {...}, "exposure": {...}}
}]}
```

A file without `"schema"` is schema 1 (flat fields beside a few blocks, as written before this
layout). Each schema-1 lens is upgraded into the layout above and then read by the same code, in the
native parser and in the app, so old settings and backups keep working and are validated the same
way, and comes out in the current names (`quirks` become `workarounds` with their new ids, `backend: ndk`
becomes `cameraApi: native`). The upgrade drops photo-stream buffer usage, merge frame limit and photo
levels from a lens without a photo stream; they only ever applied to that stream. Any other schema
number is refused.

Field names match the lens editor's labels. Schema-2 names used before that (`keys`, `streams.capture`,
`raw.unlisted`, `raw.reader`, `session.backend`, `session.fps`, `session.stopRepeatingForStill`,
`session.quirks`, `image.cfa`) are refused with the new name rather than ignored.

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
| `highResLive` | false | A vendor mode the camera doesn't list: use the size as written instead of negotiating it against the advertised sizes. |
| `useCase` | 0 | Stream use case (Java Camera2 only). |
| `buffers.usage` | chosen by the usage ladder | Fixed AHardwareBuffer usage: `gpuDataBuffer`, `none`, or hex such as `0x100003`. Used as written. |
| `buffers.count` | 6 | Buffers the reader holds, 2–16. Below 6 the realtime pipeline can starve the reader (the preview drops frames); a smaller count is for a lens probing what the HAL allocates at a full-size stream. |
| `buffers.testFirst` | false | Check a fixed usage with `AHardwareBuffer_isSupported` first; CPU-only when refused. |
| `buffers.fallBack` | false | Let a silent fixed reader fall back like the app's own choice. Without it a fixed reader stays out of the usage ladder. |
| `levels` | the lens's | Black/white levels for this stream only. |

### Photo stream (`streams.photo`)

A second RAW stream that only full-size photo requests fill (vendor full-resolution modes, "High-res
photos" in the editor); the RAW stream is then the binned viewfinder.

| Field | Default | Meaning |
|---|---|---|
| `format`, `width`, `height` | required | The full-size readout. |
| `useCase` | 0 | Stream use case (Java Camera2 only). |
| `buffers.usage` | GPU data buffer | Its reader's usage. |
| `buffers.count` | 6 | Photo reader maxImages, 2–16. |
| `includePreview` | true | Target the viewfinder RAW in photo requests too. OPPO uses false: photo RAW only. |
| `mergeFrameLimit` | 0 (the app's cap) | Most full-size frames one merged photo may request (1 = single photos only). |
| `mergeFramesOneByOne` | false | Request the frames one by one, each after the previous arrived. |
| `levels` | the lens's | Black/white levels for full-size photos. |

### Companion (`streams.companion`)

A small second output next to RAW. Some HALs will not start RAW alone, or will not produce a real
full-size frame without it.

| Field | Default | Meaning |
|---|---|---|
| `mode` | `auto` | `auto`: added by the app's rule (unverified devices, high-res photo lenses), sized to the RAW. `fixed`: always configured at `width`×`height`, part of the mode, never dropped by recovery. `off`: never, including the high-res photo one. |
| `width`, `height` | — | The size, `fixed` only. |
| `fallback` | `base` | `auto` only, when no output has the RAW's shape: `base` (smallest of at least 320×240, any shape), `nearest` (closest shape), `none` (no companion). |
| `format` | `yuv` | `yuv` or `private`. |
| `targeted` | true | Whether the repeating request writes to it. |
| `detach` | true for `auto`, false for `fixed` | Stop writing to it after the first frame paired with its metadata. |
| `buffers.usage`, `buffers.count` | per format, 2 | Its reader's usage and buffer count (2–16). |
| `physical` | follows the route | Tie it to the route's physical camera or not. |
| `useCase` | 0 | Stream use case (Java Camera2 only). |

### Aux JPEG (`streams.auxJpeg`)

An extra JPEG output configured with the session and never targeted: `width`, `height`,
`buffers.usage`, `useCase`.

## Session (`session`)

| Field | Default | Meaning |
|---|---|---|
| `cameraApi` | `auto` | `auto`, `native` or `java`; see below. |
| `opMode` | 0 | Camera2 session operating mode (Java Camera2 only). 1 and 2 are refused. |
| `params` | `cadence` | What the session parameters carry. `cadence`: the fps range and session-scope keys. `none`: a plain session, no parameters; refused with an enabled session-scope key. `full`: the whole repeating request (defaults, request-scope keys, control state); the framework may drop keys the HAL does not list as session keys (`advertised=` in the key log). |
| `template` | preview viewfinder, still-capture stills | `still` or `preview` for the session parameters, the viewfinder and the stills alike. |
| `frameRate` | 0 (the app's cadence) | Fixed AE target frame rate on the session and every request (0–240). |
| `startupDelayMs` | 0 | Wait after the session is configured before the first request (0–5000). |
| `gate.name`, `gate.hold` | none; `closed` with a photo stream, else `configured` | Vendor state the app holds while the device is open; `hold` says whether it is released once the session is configured or kept until the device closes. |
| `stopViewfinderForPhotos` | false | Stop the viewfinder stream for a full-size photo and restart it afterwards. The screen freezes for full-size photos either way. |
| `workarounds` | none | Camera workarounds this lens needs; see below. |

How a full-size photo is taken is derived, not set: a lens with a photo stream, a companion it
writes to and an enabled `decision` key uses the decision frame (one low-ISO viewfinder frame lets the
HAL switch to full size, then the viewfinder stream stops for the photos); otherwise
`stopViewfinderForPhotos` stops it, and without it the photo goes out beside the running viewfinder.

## Requests and vendor keys

| Field | Default | Meaning |
|---|---|---|
| `request.defaults` | `rawr` | `minimal` leaves distortion correction, hot-pixel mode, the hot-pixel map, face detection and the AE priority tag to the template. Controls, the shading map and the lens's keys stay. A lens on `minimal` gets no HAL hot-pixel list in its metadata. |
| `vendorKeys[].scope` | `session` | `session`, `request`, `sessionAndRequest` (the same value in the session parameters and on every request), `still` (full-size photos only), `decision` (the one low-ISO frame sent before full-size photos). |
| `vendorKeys[].type` | `int32` | `byte`, `int32`, `int64`, `float`, `double`. A HAL tag has one type; if the declared one is refused the other types are tried and the one accepted is remembered (see the `attempts=` log field). |

## Image (`image`)

`levels` (static or dynamic black/white), `sensorLayout` (`block`, `colourOrder`, `liveRemosaic`,
`photoRemosaic`, `photoBlock`), `distortion`
(rational model on its own frame size), `mirror`, `dcp` (camera profile file) and `exposure` (requests
beyond the advertised ISO and shutter ranges).

`sensorLayout.block` describes the viewfinder RAW (1 = Bayer, 2 = quad, 4 = 4x4).
`photoBlock` independently describes the separate `streams.photo` output and defaults to 1.
Both are remosaiced at full resolution. Photo-stream burst frames are remosaiced before
merge; binned preview can remain Bayer while full-size photos use quad CFA.

## Camera workarounds

Behaviours a camera provider needs avoided, named so a profile says why a lens is special.
Workarounds live on lenses only (`session.workarounds`), so the lens editor shows everything a lens
runs with; a list at the root of a built-in profile is refused. A lens added by hand starts without
any, even on a device whose built-in lenses carry some. A vendor gate and a high-res live stream add
their presets on top (see `effectiveWorkarounds`), and the editor shows those as automatic.

| Workaround | Effect |
|---|---|
| `holdTapFocus` | A tap's focus holds until the next tap; face-steered AF is off. |
| `appPriorityAe` | The camera's shutter/ISO priority AE is not used; the app's AE handles S/I. |
| `noTapFocus` | Tap AF and AF regions are unavailable. |
| `alwaysSendFrameRate` | Every request carries an AE fps range, even with AE off. |
| `noAutoCompanion` | The rule-based companion is not added. |
| `keepTemplateRegions` | A request without a tap region leaves the template's own AF and AE regions alone instead of writing an empty one (the vivo X200 Ultra's AF node dereferences a null ROI otherwise and the provider crashes on the first full-size request). |

Presets keep imported and user-edited profiles safe: a lens with a vendor gate implies `holdTapFocus`;
a `highResLive` RAW stream implies `appPriorityAe`, `alwaysSendFrameRate` and `noAutoCompanion`
(not `noTapFocus`: add it to the lens when a camera crashes on AF regions).

### Gate presets

A vendor gate can bring per-capture data with it. `vivoFullSize` and `vivoFullSizeVcf` add three still-scope keys whose
value is the capture's millisecond stamp (`vivo.control.requestId`, `frameFlag`; `globalCaptureId` is
the stamp plus 5) and four result tags read back after each full-size still, one of which
(`vcf.parameter.seamlessMode`) is kept on the frame to show whether the HAL really switched sensor
mode. These are applied only when the camera exposes the first tag. Verified on a V2562: the 200 MP
still request carries the same tags and the same stamp relation as before they became data.

## Camera API

| Field | Meaning |
|---|---|
| `session.opMode` | Camera2 session operating mode (`SessionConfiguration` type). 0 = normal; 1 and 2 are refused. |
| `streams.*.useCase` | Stream use case of an output (`raw`, `photo`, `companion`, `auxJpeg`); 0 = not set. Standard values are 1–6; anything else is vendor-defined. |
| `session.cameraApi` | `auto`, `native` or `java`. |

The native (NDK) API has no way to set an operating mode or a stream use case. `auto` therefore resolves to the
Java camera API exactly when one of them is set, and an explicit `native` together with one is rejected
when the profile is parsed. `java` can also be chosen for a lens without either, to compare the two
APIs on the same route.

A lens with `stopViewfinderForPhotos` cannot use Java Camera2, whether it asks for `java` or gets
it from an operating mode or use case: the vivo decision sequence is proven on the native API only. The
profile is refused with the reason. A vendor gate works on either API: the native session takes it
before the camera opens and holds it as the lens asks.

On Java Camera2 Kotlin opens the camera and configures the session (`SessionConfiguration` with
the operating mode, `OutputConfiguration.setStreamUseCase` per output, session parameters). Native
code keeps everything else: the readers (their windows become `Surface`s), the requests, the
controls, keys and stills. A request is built as data in native code and turned into a
`CaptureRequest` on each submit; each entry is set by its Camera2 key name with the primitive type of
the tag (a vendor key by the name it was resolved from). Results come back through
`ACameraMetadata_fromCameraMetadata`, so result handling is the same on both APIs. The log says
`CAMERA_API lens=… cameraApi=java available=1` when a lens opens this way.

Differences to know when comparing bundles:

- `CAMERA_REQUEST_DUMP` of a Java session lists only what the app set. The template defaults are
  added by Camera2 when the request is built and are not visible to native code.
- A Java request fails as a whole when it carries a tag with no Camera2 key name (an unlisted vendor
  tag given by number). The NDK would set it. Name such keys instead.

Checked on the V2562 with Java Camera2:

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

`adb shell setprop debug.rawr.camera_api java|native` runs every lens on one API, for comparing the two
on the same route (`CAMERA_API_OVERRIDE` in the log).

`adb shell setprop debug.rawr.camera_profile file:<path>` runs a profile JSON (schema 1 or 2) from a
path the app can read, in place of the built-in and the user's lenses, so an exact recipe can be run
without the lens editor. Put the file in the app's own storage, e.g.
`adb shell "run-as <package> sh -c 'cat > files/debug_camera_profile.json'" < profile.json` and
`file:/data/user/0/<package>/files/debug_camera_profile.json`. A file that does not parse is logged
(`CAMERA_PROFILE_FILE_REJECTED`) and the built-in profile is used. A lens that stops the viewfinder for photos stays on the
native API and a lens with an operating mode or use case stays on Java. Clear it with
`setprop debug.rawr.camera_api ''`.

Not checked: a vendor operating mode on a lens (only a debug probe session accepted one), session
params `full` on Java, the aux JPEG, and photos through a photo stream on Java.

## Diagnostics

| Line | Content |
|---|---|
| `CAMERA_SESSION_RECIPE` | One line per started session: route, RAW stream and aspect, template, session-parameter fps range, session params policy, request defaults, `cameraApi`, key counts per scope, companion (format, size, targeting, source, shape, images, detach), aux JPEG, `photoStream`, `rawBufferUsage`, `photoBufferUsage`, `frameRate`, startup delay, gate and hold, still strategy, `stopViewfinderForPhotos`; `opMode=` and `useCases=` only when set. |
| `CAMERA_REQUEST_DUMP kind=session_start` / `repeating_start` | Every entry of the session parameters and of the repeating request, right after the first submit, for lenses that carry keys or recipe fields. Fields: tag, type, count, values, `typeName`. Compare two builds or devices with the same lens. |
| `CAMERA_KEY_INJECTION … attempts=` | Each setter call made for a vendor key, with status; `(cached)` marks a first try from the memory of an earlier success. |
| `CAMERA_PROFILE_REJECTED` | A lens profile the app sent was refused (reason, and the profile that stays in force). The viewfinder shows "Lens settings not applied". |

The still strategy in the recipe line (`direct`, `decisionFrame`, `stopRepeatThenStill`) is derived
from the photo stream, decision-scope keys, the companion and `stopViewfinderForPhotos`; it is not
configurable.

## Where the fields are in the lens editor

Labels match the field names. The main page has Lens (camera, mirror), Capture (Resolution: Normal,
High-res live = `highResLive`, High-res photos = `streams.photo`; the vendor gate; exposure range),
Vendor mode (operating mode, frame rate, vendor keys), Black & white levels, Sensor layout and Colour.
Everything the app sets by itself is on the Advanced page: companion and aux JPEG streams, buffers,
stream use cases, session (camera API, parameters, template, request defaults, startup delay,
`stopViewfinderForPhotos`) and camera workarounds. The distortion model has no control; it comes from
built-in profiles and backups.
