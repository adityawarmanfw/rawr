# vivo X300 Ultra full-size RAW (50 / 200 MP)

Measured on V2562 (global X300 Ultra, Android 16), 2026-10-17/18, with
`tools/vivo_fullres_probe` and the device's own camera binaries and configs.

## Gate: VivoCameraManager key 40 ("useVivoCamera")

`/system/lib64/libcameraservice.so` → `Camera3Device::setUseVivoCamera()`, run at every
camera open:

```
useVivoCamera = provider->getInfo(40)
if (useVivoCamera == 1) { provider->setValue(1, 1); provider->setValue(33, 1) }   // AppType = vivo camera
else { setValue(1, findAppType(package)); setValue(33, findVCFAppType(package)) }
```

Apps reach the same store through the hidden `android.hardware.vivocamera.VivoCameraManager`
(`getInfo(int)`, `setValue(int,int)`, service `media.vivocamera`; the service forwards the
pair to the vendor provider unchanged). Key 40 = 1 makes cameraserver classify the client as
vivo's camera (AppType 1). The HAL's sensor-mode selector
(`/vendor/etc/camera/sensormode-selector/PD2547.json`) keeps the full-size modes
(`DNGHWREMOSAIC`, `FULLSATREMOSAIC`) in its `AppType == vivo` branch; any other app type
fails full-size stream configuration ("Function not implemented"). Key 33 is rewritten by
cameraserver at open and needs no change. The value is system-wide: save it before the change,
restore it after the device closes, and recover it after a crash.

## 50 MP (streaming, Bayer)

| Camera | Mode (`vivo.control.forceSensorMode`) | RAW16 size |
|---|---|---|
| main 3 | 2 | 8160×6128 |
| tele 5 | 14 | 8160×6144 |

- Mode rows come from the characteristic `org.quic.camera2.sensormode.info.SensorModeTable`
  (`{count, stride, w, h, fps, ...}`); rows sharing a size were tried one by one.
- The key must be set on the session parameters **and** every request; session-only leaves
  the sensor binned (a 12 MP image lands in the corner of the 50 MP buffer).
- Reader usage: none or `GPU_DATA_BUFFER` only. Any CPU-read or sampled-image bit fails the
  configure. The NDK then refuses `AImage` plane queries; `AHardwareBuffer_lock` still works.
- Output verified as Bayer by a colour-block test (class means differ only at 1×1).
- Black 64, white 1023 (10-bit).

## 200 MP (single still, Bayer remosaiced by the HAL)

Copy of the stock camera's session (watched with `cmd media.camera watch`, ≤ ~15 tags per
attach; larger lists crashed cameraserver):

- Streams: YUV 1440×1080 preview, RAW 16320×12288 (usage 0), YUV 16320×12288,
  RAW 4080×3072; `vivo.control.streamsUsage = [4 1 0 5]`. No forced mode.
- Session keys **and the same keys on every request**: `ultra_highresolution 1`,
  `is_pro_mode 1`, `is_ProRaw_on 2`, `com.vivo.current_ui_module 74`, `echo.mode 2`,
  `camera_scenery_pro 1`, `scenery.subfunction 5`, `session.uiSwitch_state 2`.
- Still: `STILL_CAPTURE` to both RAW streams, manual ISO / exposure / focus, plus `onlyDng 1`,
  `isCapture 0`, `sensorMode 0`, `isUpscale 1`, `isPAPNeeded` (byte) 1,
  `requestId` = `frameFlag` = id, `globalCaptureId` = id + 5.
- The HAL switches the sensor for that frame (`vcf.parameter.seamlessMode` 768) and returns;
  the preview keeps running. 1.4–6.6 s per still. Night AF stays at infinity; use a fixed
  focus distance.
- Forcing mode 0 instead streams the raw Tetra (4×4) mosaic and stops the preview.

## Pitfalls

- vivo's own camera app running in the background makes full-size sessions fail (error 3/4).
- The app's logcat is muted after heavy logging; use files or `dumpsys media.camera`.
- Firmware updates can renumber modes or remove the API: verify the pixel pattern and turn the
  lens off cleanly on failure.
