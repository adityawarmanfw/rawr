# OPPO Find X9 Ultra: 50 MP and 200 MP RAW

Support is implemented through the built-in
[`oppo_find_x9_ultra.json`](../app/src/main/cpp/camera/profiles/oppo_find_x9_ultra.json)
profile, without adding a new gate like `vivoFullSize`. The profile matches
CPH2841, PMA110 and PMA120. These settings come from X9 Ultra testing; they
are not a universal recipe for every OPPO phone or firmware version.

## Unlocking full resolution

We use Java Camera2, minimal request defaults and two separate RAW10 streams:
a small viewfinder stream and a large photo stream. The companion is disabled.
The session parameters include:

- `com.oplus.is.sdk.camera.package`: `byte`, value `1`;
- `com.oplus.is.from.extension`: `int32`, value `1`.

For 50 MP, we select the sensor mode using
`com.oplus.engineercamera.agingtest.mode.select` (`int32`), **on still capture
requests only (`scope: still`)**. This key must not reach repeating preview
requests or session parameters. The same rule applies to captureBurst.

| Lens | Camera ID | Photo resolution | Sensor mode | Photo CFA |
|---|---|---|---|---|
| H0.6 | 3 | 8192×6144 | 5 | Quad 2×2 |
| H1x | 2 | 8160×6128 | 13 | Quad 2×2 |
| H3x | 4 | 8192×6144 | 8 | Quad 2×2 |
| H10x | 5 | 8192×6144 | 25 | Quad 2×2 |
| 1xF | 2 | 16320×12256 | Automatic, no agingtest key | 4×4 |
| 3xF | 4 | 16384×12288 | Automatic, no agingtest key | 4×4 |

For **200 MP, the agingtest key is omitted entirely**. The large RAW stream
makes the HAL select mode 0 automatically. Preview can remain active during
capture; experimental changes that stopped preview were reverted.
`com.oplus.extension.operation.mode` also proved unnecessary and was removed.

### 200 MP requires root and a patched camera library

**200 MP does not work without root on the tested setup.** In addition to the
app profile, it requires patching `camera.qcom.sm8850.so` to raise the MCTFE
input-width limit. The profile and automatic sensor-mode selection alone are
not sufficient.

Locate `MCTFENode::FetchSensorInfo(int)` in `camera.qcom.sm8850.so` and find the
branch that logs `IPP Path input width ... Out-of limit`. Immediately before
the width comparison, identify the instruction that loads the limit from the
hardware-capability structure (`ldr w8, [x8, #12]` in our build). Replace
**that instruction**, not an instruction at a fixed file offset, with
`mov w8, #16384`.

The instruction location can vary between library/firmware builds, so identify
it from the function and comparison above. Applying the modified camera
library on the device requires root. This patch is separate from the changes
in this repository.

Patched `camera.qcom.sm8850.so` files for each X9 Ultra version will be shared
in the [OFX7U Telegram group](https://t.me/OFX7U).

## Changes required in the app

- **Separate request targets and geometry.** Preview remains Bayer RAW10 at
  4080×3064 for 1x or 4096×3072 for the other cameras. `includePreview: false`
  targets only the large photo stream in still requests; repeating preview
  continues separately. Applying the full-res mode to RAW preview reproduced
  `CamxResultExtBindFailed` and lost buffers. Two full-size RAW streams were
  not a working configuration either.
- **Photo buffers.** The profile sets `usage: "0x3"` (CPU_READ_OFTEN) and
  `count: 2`. Without the appropriate usage flags, the app could not read
  RAW10 correctly. We added configurable photo buffer counts and still
  request target selection.
- **Independent photo CFA.** `sensorLayout.block: 1` keeps preview Bayer,
  while `photoBlock: 2` or `4` describes the photo RAW. Remosaic converts it
  to Bayer at full resolution; it does not bin or reduce 200 MP to 50 MP.
  In a burst, each full-res frame is remosaiced before journaling and merge.
- **Editable settings.** These options are exposed in the lens editor.
  Sensor layout retains the familiar Bayer / Quad 2×2 / 4×4 CompactChoiceRow,
  with a Viewfinder CFA / Photo CFA selector only for lenses with a separate
  photo stream. Remosaic controls follow the selected tab; colour order is
  explicitly shared. Descriptions distinguish request targets from live
  preview operation and buffer count from burst length.

New fields default to the previous configuration: `includePreview: true`,
`bufferCount: 0` (default of 6), and `photoBlock: 1` (Bayer). The vivo gate was
not changed. OPPO-specific settings live in its profile.

## Diagnostics and performance on this branch

We added detailed shutter-to-DNG-publication timings for frame arrival,
copying, remosaic, journaling, queues, reads/uploads, merge and encoding/output.
See [burst-timing.md](burst-timing.md) for correlation rules and a logcat filter.
The new detailed logs and additional clock measurements are Debug-only;
existing operational and error logs remain unchanged.

The original unoptimized Debug trace spent about 48.5 s remosaicing three
200 MP frames. We enabled `-O2` for `session/SessionEngine.cpp` in Debug
(the inline remosaic code is compiled in that translation unit). Two subsequent
tests took 2.59/2.76 s for remosaic and 17.77/17.88 s from shutter press to
completion. Testing `-O3` showed no clear overall advantage, so we retained
`-O2`. The algorithm, resolution and release flags were unchanged. These were
device measurements, not a controlled benchmark or an image-equivalence test.

Debug and Release builds passed. Device testing confirmed successful 200 MP
photo output and three-frame HDR+ captures; it does not cover every lens,
settings combination or firmware version.
