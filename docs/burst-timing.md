# Burst timing logs

Detailed capture timing is Debug-only (native `!NDEBUG`, Kotlin
`BuildConfig.DEBUG`). Release omits these logs and their additional clock
measurements. Existing operational/error logs are unchanged.

Capture timing records are INFO logcat output, independent of persistent
pipeline diagnostics. They time existing stages without changing capture,
remosaic, merge or output behaviour. Enable no extra RAW/RZSL diagnostic dump
for a baseline: those dumps add work and storage traffic.

```sh
adb logcat -v threadtime | rg --line-buffered 'CAPTURE_PRESS|CAPTURE_TIMING|CAPTURE_ACCEPTED|CAPTURE_FINISHED|CAPTURE_DNG_PUBLISHED|FULLRES_|BURST_|MERGE_STRIP|MULTIFRAME_(MERGE_TIMING|STRIP|MEMORY_PLAN|ARENA|JPEG_INPUT)|DNG_(BEGIN|TIMING)'
```

## Correlation and clocks

`CAPTURE_PRESS token` identifies the foreground shutter event. Kotlin's
`submit_job` end record maps this token to `requestId`. The acquisition stage
runs before that request ID exists: `fullres_* id` is the native burst tag,
also printed as `FULLRES_BURST_BEGIN tag`. Correlate its monotonic begin/end
with the enclosing Kotlin `acquire` stage. After acquisition, `id` is the
request ID. `frame` is one-based during acquisition/loading; on merge-strip
stages it is the strip's starting row (also printed as `y0`). A fullres arrival
and its processing record join on `sensorTimestampNs`.

`CAPTURE_TIMING` has begin/end pairs, absolute `monoMs` and end `wallMs`.
These are elapsed wall times, including scheduling, IO and GPU waits, not
CPU-utilization or GPU-only timings. Parent stages include their children;
concurrent DNG writers may overlap merge/render. Do not sum nested or
concurrent durations. An end record also appears on stack unwinding and does
not imply success. A begin with no end identifies a stage still running or a
process that died in that stage.

Native `monoMs` is steady_clock. Kotlin monoMs uses System.nanoTime; click
elapsed durations use elapsedRealtimeNanos consistently within Kotlin.
Sensor timestamps are a separate camera clock: compare adjacent sensor
values, never subtract one from monoMs. `FULLRES_ARRIVAL` reports both
sensorDeltaMs and arrivalDeltaMs; -1 means no preceding sample.

## What to inspect

* `worker_start clickElapsedMs`: delay between the shutter and capture worker.
* `assets_ready`, `prepare_outputs`, `submit_job`: asset/provider/native job setup.
* `fullres_allocate`, `fullres_submit`: journal/buffer allocation and camera submission.
* `FULLRES_ARRIVAL`: actual callback cadence and sensor timestamp cadence.
  Reader backpressure can delay callbacks and eventually sensor requests.
* `fullres_wait_buffer_and_result`: wait for a matching RAW buffer and metadata.
  A frame may already be queued while the previous one is processed.
* `FULLRES_COPY`: fence/map lock, RAW unpack/copy, unlock and source stride.
* `fullres_release_and_resume_reader`: return the camera buffer and drain any queued images.
* `fullres_validate_buffer`, `fullres_remosaic`, `fullres_journal_append`: content
  validation, CPU remosaic and the per-frame journal write. These serialize on
  the acquisition worker; expensive remosaic/write can hold up consumption of
  subsequent frames even if the sensor is fast.
* `fullres_wait_sequence_end`: camera burst completion after the buffers are consumed.
* `BURST_WORKER_DEQUEUED acceptedElapsedMs`, `burst_wait_spool`,
  `burst_wait_processing_mutex`, `burst_spool`, `burst_reference_selection`,
  `burst_save`, `burst_load`:
  background queue, persistence and processing-lock delays. Accepted time is
  after fullres acquisition; it excludes the preceding camera/remosaic stage.
* `burst_frame_storage_read`, `burst_frame_gpu_upload`: each lazy frame load.
  Strip merge may load the same frame repeatedly for different row windows.
* `merge_strip`, `merge_strip_compute_and_load`, `merge_strip_project_and_stitch`:
  wall time per strip, including lazy loads and pipeline setup where applicable.
  `MERGE_STRIP_STAGES` breaks down initialize/noise/reference/preparation/alignment/
  kernel/robustness/accumulation/finalize. Existing `MULTIFRAME_MERGE_TIMING`
  summarizes the algorithm's timing counters.
* `dng_metadata_adapt`, `dng_encode_and_write`, `dng_fsync` and `DNG_TIMING`:
  file encoding/output. pwriteMs is time spent inside write syscalls;
  nonPwriteMs is the rest of encoding/write wall time, not pure CPU time.
  fsync measures the subsequent durability wait. DNG_BEGIN/name distinguish
  base and merged files when they share a request ID.
* `CAPTURE_DNG_PUBLISHED`: MediaStore completion and click-to-published time.
* `CAPTURE_FINISHED clickElapsedMs`: foreground shutter to final publication.

For a 200 MP three-frame burst, start with fullres_remosaic for all three
frames, then lazy read/upload and merge strips, then DNG encode/pwrite/fsync.
The logs are needed before assigning the bottleneck; no fixed cause is assumed.
