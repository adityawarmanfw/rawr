# vivo X300 Ultra full-size RAW (50 / 200 MP)

Investigated on V2562 (global X300 Ultra, Android 16), 2026-10-17–23, using
rawr, `tools/vivo_fullres_probe`, and the device's own camera binaries and configs.
The app behavior below reflects the current implementation; measurements describe
the tested firmware and are not guarantees for other devices.

## Gate: VivoCameraManager key 40 ("useVivoCamera")

This gate was identified by inspecting the device's camera binaries and configs,
then testing full-size sessions with `tools/vivo_fullres_probe`. The recorded
reverse-engineering finding attributes the following logic to
`Camera3Device::setUseVivoCamera()` in `/system/lib64/libcameraservice.so` during
camera opening. The exact X300 Ultra disassembly and caller trace are not linked
here, so this is a firmware-specific finding, not an Android API guarantee or an
independently verified claim that the function runs at every camera open.

The numbers are keys in vivo's private provider store, not Camera2 metadata tags:

| Key | Meaning | Role |
|---|---|---|
| 40 | `useVivoCamera` | Input override set by the app: `1` requests vivo-camera classification. |
| 1 | `AppType` | Application classification written by cameraserver; `1` denotes vivo's camera. |
| 33 | `VCFAppType` | Separate application classification for vivo's VCF camera layer, also written by cameraserver. |

Recorded logic (pseudocode):

```
useVivoCamera = provider->getInfo(40)
if (useVivoCamera == 1) {
    provider->setValue(1, 1)     // AppType = vivo camera
    provider->setValue(33, 1)    // VCFAppType = vivo camera
} else {
    provider->setValue(1, findAppType(package))
    provider->setValue(33, findVCFAppType(package))
}
```

rawr loads
`android.hardware.vivocamera.VivoCameraManager` with `Class.forName`, calls its static
`getInstance()`, and resolves `getInfo(int)` and `setValue(int, int)` by reflection.
The gate uses those methods to read key 40, set it to `1`, verify the value, and
later restore the previous value. rawr does not directly look up a Binder service
or implement forwarding to the vendor provider.

Key 33 appears in the pseudocode because it is an output of cameraserver's
classification decision. The current app changes only key 40; it does not write
key 1 or key 33.

On the tested firmware, the HAL's sensor-mode selector
(`/vendor/etc/camera/sensormode-selector/PD2547.json`) keeps the full-size modes
(`DNGHWREMOSAIC`, `FULLSATREMOSAIC`) in its `AppType == vivo` branch. The investigation
reported full-size stream configuration failing without that classification
("Function not implemented").

Current rawr gate lifetime:

- A lens with `vendorGate: "vivoFullSize"` acquires key 40 before opening the camera.
  The app saves the previous value, sets `1`, waits for the asynchronous write,
  and verifies it. If the hidden API is unavailable or acquisition fails, the
  lens does not open.
- Routes without a separate capture stream release the gate after session
  configuration; a later session configuration acquires it again.
- Routes with a `captureStream` (the current H/F lenses) keep the gate until the
  device closes. The implementation records that the HAL rebuilds its still
  pipeline for full-size requests and can crash if the gate was released after
  configuration. The earlier configure-only lifetime does not apply to these routes.

The value is system-wide. rawr journals the previous value before changing it,
restores it on release, and attempts recovery at the next app start after a crash
when the value is still `1` and no camera is in use.

## 50 MP (H lenses: binned viewfinder, Bayer still capture)

The current built-in X300 Ultra profile configures a binned RAW16 `stream` for the
viewfinder and a separate RAW16 `captureStream` filled on shutter press. All three
H lenses use `vendorGate: "vivoFullSize"`, held until the camera closes.

| Lens | Camera ID | Viewfinder `stream` | Still `captureStream` buffer | Still `vivo.control.forceSensorMode` |
|---|---|---|---|---|
| H14 (ultrawide) | 4 | 4096×3072 | 8192×6144 | 0 |
| H35 (main) | 3 | 4080×3064 | 8192×6144 | 2 |
| H85 (tele) | 5 | 4080×3072 | 8192×6144 | 14 |

These are configured buffer sizes. Capture geometry is derived from the camera's
pixel array and trims padding: the 8192-wide buffer can contain only 8160 useful
pixels, with main/tele image sizes of 8160×6128 and 8160×6144 respectively.

### Profile keys and shutter behavior

The current H profiles assign keys to these scopes (names below use the
`vivo.control.` prefix unless written in full):

- **Session and request:** `ai_highresolution = 1`, `ultra_highresolution = 0`,
  `is_ProRaw_on = 2`, `echo.mode = 2`, `scenery.subfunction = 5`,
  `session.uiSwitch_state = 2`.
- **Request only:** `is_pro_mode = 1`, `camera_scenery_pro = 1` (byte).
- **Still only:** `com.vivo.current_ui_module = 74`, `onlyDng = 1`,
  `isCapture = 0`, `sensorMode = 0`, `isUpscale = 0`, `isPAPNeeded = 1` (byte),
  and the lens-specific `forceSensorMode` from the table. Other listed profile
  keys are `int32`.

Request-scope keys are applied to the repeating preview and to the fresh
`TEMPLATE_STILL_CAPTURE` request. On shutter press, the still targets both the
binned RAW and full-size RAW outputs, uses the selected exposure, and preserves
the preview AF mode/region (manual focus distance is set only in MF). Native code
also adds the `int64` still identifiers `requestId`, `frameFlag`, and
`globalCaptureId` (the first two share an ID; the last is ID + 5).

The H profiles have no `decision` keys, so they do not run the 200 MP decision
request or explicitly stop repeating for that sequence. Sensor switching may
still interrupt preview delivery.

### Required YUV companion

For this separate full-size capture route, a small YUV output must remain
configured alongside the two RAW outputs. The recorded reason is that full-size
stills can crash the vivo camera provider without it. rawr therefore requests the
companion whenever a lens has a `captureStream`, even if the companion debug
override is set to off. It needs no extra field in the lens profile.

Being **configured in the session** and being **targeted by a request** are
different:

- At startup, preview requests target both binned RAW and companion YUV.
- Once the first RAW frame arrives, rawr asks the camera to send only RAW frames
  for the live preview. The YUV buffer stays attached to the camera session, but
  rawr no longer asks the camera to put images into it. The HAL needs that YUV
  output present for full-size captures, even when it is idle.
- A 50 MP still targets both RAW outputs, without targeting the YUV companion.
- Any YUV images that arrive are immediately discarded. They are never used as
  the viewfinder or saved with the photo.

The companion therefore satisfies a HAL session requirement without continuously
producing YUV frames. Current error handling still allows a RAW-only startup if
creating the companion fails; that fallback does not establish that full-size
capture is safe without it.

### Buffer handling

The full-size capture reader uses `GPU_DATA_BUFFER`. If NDK `AImage` plane queries
fail, the RAW16 copy path uses the hardware buffer's layout and locks it for CPU
reading. The capture context treats the HAL output as Bayer. The H profiles use
static black levels `[64, 64, 64, 64]` and white level `1023` (10-bit values stored
in RAW16).

### Earlier continuous-stream probe findings

The original probe also streamed 50 MP RAW directly: main camera 3 at mode 2
(8160×6128), and tele camera 5 at mode 14 (8160×6144). Mode candidates came from
`org.quic.camera2.sensormode.info.SensorModeTable`
(`{count, stride, w, h, fps, ...}`); rows sharing a size were tried individually.

For that continuous-stream experiment, `forceSensorMode` had to be applied to
both session parameters and every request; session-only produced a binned image
in the corner of the large buffer. That finding does not describe the current H
profiles, which apply the forced mode only to still requests.

The probe reported successful configuration with usage 0 or `GPU_DATA_BUFFER`,
and failure with CPU-read or sampled-image usage bits. It verified Bayer output
with a colour-block test (class means differed only at 1×1). These observations
and mode numbers belong to the tested firmware; another device needs its own
stream/mode validation.

## 200 MP (F lenses: binned viewfinder, Bayer still capture)

F35 and F85 use a binned RAW16 viewfinder, a separate 200 MP RAW16 capture output,
and the required small YUV companion described above. Both profiles use
`vendorGate: "vivoFullSize"`, held until the camera closes. The full-size reader
uses `GPU_DATA_BUFFER`; the HAL supplies Bayer data, with static black levels
`[64, 64, 64, 64]` and white level `1023`.

| Lens | Camera ID | Viewfinder `stream` | Still `captureStream` |
|---|---|---|---|
| F35 (main) | 3 | 4080×3064 | 16320×12256 |
| F85 (tele) | 5 | 4080×3072 | 16320×12288 |

### Profile keys

Names below use the `vivo.control.` prefix unless written in full:

- **Session and request:** `ultra_highresolution = 1`, `is_ProRaw_on = 2`,
  `echo.mode = 2`, `scenery.subfunction = 5`, `session.uiSwitch_state = 2`.
- **Request only:** `is_pro_mode = 1`, `camera_scenery_pro = 1` (byte).
- **Still only:** `onlyDng = 1`, `isCapture = 0`, `sensorMode = 0`,
  `isUpscale = 1`, `isPAPNeeded = 1` (byte).
- **Still and decision:** `com.vivo.current_ui_module = 74`.

Other listed profile keys are `int32`. Request-scope keys are applied to both
normal preview requests and fresh still requests. Neither F profile sets
`forceSensorMode`. In particular, UI module 74 is absent from normal RAW preview
requests: it is sent on a special YUV-only preparation request and on the still.

### Shutter sequence and viewfinder

A **decision frame** is a temporary camera frame used to make the HAL choose
full resolution for the upcoming still. It is not displayed or saved.

1. rawr proceeds without waiting for autofocus or manual lens movement to settle.
   This applies to both H and F capture-stream lenses. It chooses the still
   exposure from manual settings, rawr AE, or the latest applied exposure.
   Pressing the shutter does not start a new AF scan or switch autofocus to
   manual focus; an ongoing focus adjustment can therefore affect the shot.
2. It replaces normal preview requests with decision requests. These ask the
   camera to fill only the companion YUV buffer, using UI module 74, AE off,
   the lowest supported ISO, the preview's exposure time (1/30 s if unavailable),
   and the widest advertised FPS range. The low ISO is only for this preparation
   step; it does not replace the photo's selected ISO.
3. It waits up to two seconds for the first decision result. The YUV image is
   discarded, and the result is used only to signal that preparation completed.
   A failed or missing result still allows a capture attempt if the camera
   remains valid.
4. After successfully starting the decision requests, rawr stops repeating
   requests before submitting the still sequence. This pauses continuous live
   viewfinder updates; requests already in flight may still finish.
5. A fresh `TEMPLATE_STILL_CAPTURE` request asks for both binned RAW and 200 MP
   RAW, using the selected photo exposure and the existing AF mode/region.
   Manual focus distance is supplied only in MF. The request also carries the
   still keys and `int64` IDs: `requestId = frameFlag`, `globalCaptureId = id + 5`.
6. When the sequence completes or aborts, rawr resumes normal RAW preview
   requests. If preparation is abandoned after replacing preview requests, it
   also attempts to restore normal preview.

The companion YUV buffer stays attached throughout the session. It receives
images during startup and decision preparation, but normal preview stops asking
for YUV after the first RAW frame. The still itself asks for the two RAW outputs
only. Thus the companion both satisfies the HAL's session requirement and gives
the decision frame somewhere safe to go without filling the RAW viewfinder.

### Capture notice and preview recovery

**Hold still** covers capture preparation and the estimated remaining exposures,
including gaps between sequential shots. After the final exposure starts, the
estimate includes its exposure time plus a 150 ms readout margin. It does not wait
for the large RAW buffers to finish processing or copying.

If no newer live RAW frame has been presented yet, the notice changes to
**Restoring preview…**. It clears when the renderer presents a live frame from
the same camera session with a timestamp later than the last exposure's estimated
end. Submitting preview requests alone does not mean the viewfinder has resumed.

Normal preview is still restored on sequence completion or abort, not immediately
after the last exposure starts. Experiments that resumed RAW preview earlier
failed on this firmware. A RAW10 companion preview experiment also failed to
provide usable continuous live frames while the full-size capture completed;
rawr continues to use the binned RAW16 viewfinder.

### Bursts and fallback

Both built-in F profiles default to `captureBurst = 3` and queued submission
(`captureBurstSequential = false`). A saved custom profile can override either.
The old separate six-frame application limit has been removed; the app limit is
now 30. That is a software limit, not a claim that the HAL supports 30 queued
200 MP captures.

The effective full-size burst count is the smallest of:

- The multiframe count requested for the shot, including any requested long frames.
- The lens's `captureBurst` ceiling, if nonzero.
- The application's 30-frame limit.

For example, a lens ceiling of 30 with Multiframe Maximum Frames set to 16
requests 16 frames for an ordinary unbracketed merge. The lens ceiling does not
select the requested count. `captureBurst = 0` uses the app limit; `1` selects
a single still instead of a merge. Full-size bursts use the selected still
exposure for all frames; they do not add binned long-exposure frames.

- **Queued:** prepare once, then submit all still requests as one sequence. The
  camera can schedule their exposures consecutively while rawr drains completed
  buffers.
- **Sequential:** submit one still and wait for its matching image, metadata, and
  sequence completion before preparing the next. The next shot is submitted
  before copying the current image, allowing some overlap with the CPU copy.
  This repeats the decision step for each F-lens shot and adds a delivery wait
  between exposures; it cannot maintain the queued sensor cadence.

The RAW reader can lend six images at once; that is not the total burst limit.
rawr copies and releases each acquired image, then explicitly checks the reader
again for queued images. Without that retry, a burst could stall after filling
the reader even though more frames were waiting. Copied frames are written to a
disk journal for the merge rather than retaining the entire burst in CPU memory.

Images and metadata are matched by timestamp. If the HAL returns a binned image
in the corner of the large buffer, a single shot is saved at the actual binned
size. Such frames are excluded from a merge; fewer than two valid full-size
frames triggers a fresh single-shot fallback. This differs from acquisition
failure: a failed frame, timeout, or camera error aborts acquisition without
submitting a replacement shot. If submission of a later sequential shot is
refused, the app can finish with the frames already collected.

The UI reports binned/single-shot fallbacks. The current app marks these 200 MP
capture routes as DNG-only.

#### Burst tests on 2026-10-23

These F85 observations are individual tests on V2562, not supported-count limits:

| Submission | Observed result |
|---|---|
| 6 and 7 queued | Completed and saved. |
| 8 queued, before reader retry | All metadata arrived, but acquisition timed out waiting for images. |
| 8 queued, after reader retry | Completed and saved; the retry acquired a stranded image. |
| 10 queued | Nine frame metadata results and one explicit camera capture failure; acquisition aborted. |
| 16 queued | Repeated vendor camera-provider SIGSEGV in `com.qti.stats.asd.so` / `CamX::CASDStatsProcessor`. |
| 16 sequential | Completed and saved. |
| 30 queued | Not verified: attempts described as “30” had a lens ceiling of 30 but actually submitted 16. |

The ten-frame failure did not include a vendor-process crash. Neither the
six-image reader capacity nor the lens ceiling explains the observed
sixteen-frame vendor crash. The maximum reliable queued count remains unproven.

### Earlier stock-camera probe

The original probe copied the stock camera's session, observed with
`cmd media.camera watch` (≤ ~15 tags per attach; larger lists crashed cameraserver).
This was a different session layout from rawr's current F profiles:

- Streams: YUV 1440×1080 preview, RAW 16320×12288 (usage 0), YUV 16320×12288,
  RAW 4080×3072; `vivo.control.streamsUsage = [4 1 0 5]`. No forced mode.
- Session keys **and the same keys on every request**: `ultra_highresolution 1`,
  `is_pro_mode 1`, `is_ProRaw_on 2`, `com.vivo.current_ui_module 74`, `echo.mode 2`,
  `camera_scenery_pro 1`, `scenery.subfunction 5`, `session.uiSwitch_state 2`.
- Still: `STILL_CAPTURE` to both RAW streams, manual ISO / exposure / focus, plus `onlyDng 1`,
  `isCapture 0`, `sensorMode 0`, `isUpscale 1`, `isPAPNeeded` (byte) 1,
  `requestId` = `frameFlag` = id, `globalCaptureId` = id + 5.
- In that probe, the HAL switched the sensor for the still
  (`vcf.parameter.seamlessMode` 768) and returned while preview continued running.
  Stills took 1.4–6.6 s. Night AF stayed at infinity, so the probe used a fixed
  focus distance. rawr's current sequence pauses repeating requests and preserves
  the user's focus mode as described above.
- Forcing mode 0 in the experiment streamed the raw Tetra (4×4) mosaic and stopped
  the preview.

### Stability investigation (2026-10-22)

rawr's F35/F85 lenses (binned RAW16 viewfinder + 16320-wide RAW16 capture stream + a 320×240
companion YUV the viewfinder doesn't target) failed two ways: stills came back binned (12 MP in
the corner of the 200 MP buffer, `seamlessMode` 0) whatever the still's ISO, and stills longer
than ~4 s ended in device error 4. The fix was to send a low-ISO decision frame
before the still, then stop repeating requests until the still sequence ended.

#### Why the decision frame is needed

- On the tested firmware, the HAL picked full size or binned on a **preview request carrying
  `com.vivo.current_ui_module` 74**, from that frame's gain: ISO ≤ 500 full size, ≥ 519 binned
  (cutoff ~512, ISO 50 = 1× `AECLinearGain`; same on tele cam 5 and main cam 3). One such frame
  is enough.
- It keeps the decision, per sensor, for later stills **of any app** until the next such frame.
  rawr sent ui 74 only on stills (on the viewfinder it silences the RAW stream), so it inherited
  whatever the last session left. A/B: probe still with a low-gain viewfinder → rawr ISO 800
  stills full size; probe with an ISO 800 viewfinder → rawr ISO 100 stills binned.
- Not factors: the still's ISO / shutter, viewfinder frame brightness or shutter, AF mode, tap
  AF, lens switches, scene. A low-ISO frame without ui 74 does nothing.
- ui 74 on a frame that fills the RAW viewfinder stream = device error 4. Send it to the
  companion YUV only.

#### Why rawr pauses repeating requests during capture

With the 200 MP still exposing, what repeats decides whether the HAL survives past ~4 s:

| Repeating during a ≥ 4.5 s still | Result |
|---|---|
| RAW viewfinder request (rawr's) | device error 4 (4 s still OK) |
| companion YUV only, no ui 74 | device error 4 |
| companion YUV + ui 74, fps range 12–30, 7–30, 30–30, 5–15 or none | device error 4 |
| companion YUV + ui 74 + fps range 5–30 (widest advertised) | full size, 5–10 s |
| nothing (stopped after the decision frame) | full size, 5 and 8 s |

On the tested firmware, camera 5 advertised 5–7, 7–7, 10–10, 5–15, 10–15, 15–15,
5–16, 16–16, 5–20, 24–24, 5–30, 7–30, 30–30. At the time of the investigation,
rawr's viewfinder in M/I carried no range at all (result `aeTargetFpsRange [0 0]`).

In these tests, repeating requests through the still broke the NDK result handling:
those frames completed before the still. `ACameraDevice` logged
`Frame number out of order` / `Frame number … decreased`, ended the still's
sequence early and dropped its result. The image arrived without metadata and the
capture timed out. The Java API used by the probe tolerated the reordering, so
the probe did not show this failure.

#### Verification of the current sequence

Verified on V2562 from a forced binned state: F85 ISO 100 / 800, 5 s and 8 s at ISO 800, F35:
all full size, no device error, viewfinder back after each. The binned-corner detection stays
as the safety net. Saved custom lens lists retain their own keys; installing an updated
app does not merge new built-in keys into them. Older F profiles need the
`com.vivo.current_ui_module = 74` decision-scope key added, or can be reset to device
defaults if discarding custom lens edits is intended.

### Timing and storage

Distinguish **sensor exposure cadence**, **RAW delivery latency**, and **merge/save
time**. Sensor timestamps about 133 ms apart mean roughly 7.5 exposures per second
once the sensor has switched. They do not mean rawr receives, processes, or saves
a finished 200 MP frame every 133 ms.

Earlier small-burst measurements on F85 at 1/15 s showed about 0.6 s for decision
preparation, 1.25 s from submission to the first RAW, and roughly 0.6 s for each
further delivered frame. Later larger bursts did not maintain that delivery
pattern: metadata and buffers could arrive late in a group.

For the successful eight-frame queued test on October 23, the first RAW result
arrived about 2.3 s after submission; the remaining metadata arrived around
12.3 s. Acquisition and journaling finished around 16 s, then merge/save took
about another 43 s. The sixteen-frame sequential test took about 45 s for
acquisition and another 73 s for processing. These timings depend on the scene,
exposure, firmware, storage, and processing configuration.

Queuing the decision frame in the same burst as the stills, without waiting for
its result, left the stills binned in the earlier probe. The app retains that
preparation wait even though the separate focus-settling wait was removed.

A packed F85 RAW16 frame is about 382.5 MiB. A thirty-frame acquisition journal
alone needs about 11.2 GiB, before other output/workspace requirements. Bursts
reuse a CPU copy buffer and release camera buffers as they are copied. Admission
reserves disk space for twice the RAW payload plus 32 MiB, with another 512 MiB
kept free: about 23 GiB free for thirty F85 frames when no other jobs have
reservations. Raising the frame ceiling does not bypass that check. Ordinary
single-shot captures also reserve CPU memory and can wait up to five seconds
for admission.

## Configuring another device or a custom lens

`vendorGate: "vivoFullSize"` selects the vivo key-40 gate. That code does not check
for an X300 Ultra model name. It does not by itself choose a high-resolution size,
sensor mode, or still request sequence. An X200 Ultra profile can select the same
mechanism, but its hidden API, camera IDs, sizes, vendor keys, and HAL behavior
must be validated on that phone; X300 Ultra values are not automatically portable.

The lens editor exposes:

| Setting | Purpose |
|---|---|
| Camera and physical camera IDs | Select the sensor/route. |
| Preview RAW stream | Format and dimensions used for the live viewfinder. |
| Vendor gate | Select `vivoFullSize` for the tested vivo override. |
| Separate capture stream | Enable an additional still output and set its format, width, and height. |
| Burst frame limit | Per-lens ceiling, independent of the requested multiframe count. |
| Burst submission | Queued or sequential. |
| Vendor keys and scopes | Supply session, request, still, and decision settings for that sensor/firmware. |
| Black/white levels and other lens parameters | Interpret and render the returned RAW data. |

Dimensions can be entered manually, including sizes absent from advertised
Camera2 streams. This does not guarantee that the HAL accepts them. The tested
separate-capture path uses RAW16; the UI's RAW10 option is not evidence that
full-size RAW10 acquisition and copying work on the device. Capture-reader usage
and the companion stream are managed automatically, without extra editor fields.

Lens-profile backups carry these fields. Saved custom profiles override the
built-in list and should survive reinstalling. If values revert, check whether
settings were actually saved: a malformed preferences file during the October 23
tests prevented all writes, leaving edits effective only until the next restart.
That file was repaired on the test phone; the app now logs persistence errors
and shows a warning on save failure. It does not automatically repair corrupted
preferences files.

## Tools

- `tools/vivo_fullres_probe` (build with `-PprobePackage=com.rawr.probe`; the default id is
  `android.camera.cts`), stock route options for all of the above (`rawrkeys`, `pviso`/`pvexp`,
  `prepareiso`/`prepareui`/`prepareyuv`, `rawrpv pvfps`, `restorebeforestill`, `yuvnoui`,
  `stopbeforestill`, `dumpreq`, `nosave`). A local runner (`tmp/fullres-investigation/run2.py`, not
  in the repo) runs a JSON case list at ~13 s per 5 s still.
- rawr logs every key of the session, viewfinder and first still request per full-size burst
  (`CAMERA_REQUEST_DUMP`); the local `reqdiff.py` maps the hex tags with `dumpsys media.camera` and diffs
  them against the probe's `dumpreq`.
- Read the HAL's live gain with `dumpsys media.camera` → "Latest received frame":
  `AECLinearGain`, `AECSensitivity`, `SensorCurrentMode`, `aeTargetFpsRange`.

## Pitfalls

- vivo's own camera app running in the background makes full-size sessions fail (error 3/4).
- The app's logcat is muted after heavy logging; use files or `dumpsys media.camera`.
- Firmware updates can renumber modes or remove the API: verify the pixel pattern and turn the
  lens off cleanly on failure.
- A probe killed mid-run, or rawr force-stopped mid-session, can leave key 40 = 1; reset it with
  `ProbeActivity --es mode lease --ei sdk 0 --ei vcf 2`.
- Wireless adb drops when the phone sleeps; keep the screen on for installs.
