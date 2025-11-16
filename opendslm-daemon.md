# opendslm-daemon

`opendslm-daemon` is a lightweight HTTP controller that drives the openDSLM
libcamera pipeline for DSLR-style workflows. It exposes endpoints to configure
sensor parameters, trigger CinemaDNG stills, start/stop RAW video sequences, and
stream preview frames. The current alpha has only been tested on Raspberry Pi 4B
and Raspberry Pi 5 boards paired with the IMX585 “StarlightEye” module, but the
code should adapt easily to other libcamera-compatible sensors.

## Building

The daemon is built alongside the shared `rpicam_app` support library:

```bash
meson setup build
ninja -C build opendslm-daemon
```

## Running

```bash
./build/apps/opendslm-daemon --port 8400
```

The process runs until it receives `SIGINT`/`SIGTERM`.

### Preview backends and assists

Enable the optional GStreamer preview backend with `meson setup build
-Denable_gstreamer=enabled`. When present, the daemon publishes frames through a
shared-memory `shmsink` bound to `/tmp/opendslm-preview.sock` by default and
advertises matching client pipeline strings via `/status`. Use
`--preview-gstreamer=…` to override the downstream pipeline, `--preview-
gstreamer-socket` to change the socket path, and `--preview-gstreamer-client`
to customise the recommended client pipeline. Preview overlays (focus peaking,
zebras, etc.) live under `post_processing_stages/assist/` and can be enabled via
the existing post-processing pipeline configuration.

### Known limitations (alpha warning)

- ⚠️ The HTTP server does **not** provide TLS, authentication, or request
  throttling. Only expose it on trusted networks or place it behind a reverse
  proxy that enforces access control and request size limits.
- ⚠️ `/preview/stream` accepts a single MJPEG client at a time. Additional
  connections receive HTTP 409 until the active viewer disconnects.
- ⚠️ CinemaDNG video capture is still experimental and writes every frame to
  disk. Monitor free space carefully and stop recordings explicitly.
- ⚠️ Still captures block until a frame finishes writing; very long exposures
  can exceed the default timeout and return HTTP 503 even though the camera is
  still busy.

## HTTP endpoints

All endpoints speak UTF-8 JSON unless otherwise noted.

### `GET /status`

Returns the current session state, camera settings, preview pipelines, and the
summary of the latest capture (or `null` if nothing has been recorded yet).

### `GET /settings`

Returns only the settings block.

### `POST /settings`

Updates camera settings. Only supplied keys change. Supported fields:

| Field | Type | Notes |
| --- | --- | --- |
| `fps` | float | Frames per second (> 0). |
| `shutter_us` | float | Exposure time in microseconds (≥ 0). |
| `analogue_gain` | float | Analogue gain (> 0). |
| `auto_exposure` | bool | Toggle automatic exposure. When true, manual shutter/gain are ignored. |
| `output_dir` | string | Directory where CinemaDNG frames are written (default `/ssd/RAW`). |
| `mode` | string | Optional sensor mode string (`W:H:bit-depth:P|U`) to pin a RAW resolution. |

Example:

```json
{"mode": "1928:1090:12:P", "fps": 25.0, "auto_exposure": false, "shutter_us": 40000, "analogue_gain": 2.0}
```

### `GET /metadata`

Returns the current metadata configuration (make/model/software/etc.) along with
the resolved strings for the detected sensor.

### `POST /metadata`

Allows updating the metadata fields (`make`, `model`, `unique_model`,
`software`, `artist`, `copyright`). All supplied fields must be strings.

### `POST /capture/still`

Triggers a single CinemaDNG frame. The request blocks until the frame is
written (up to ~5 s by default). The response contains `frames` and `count`. A
JSON body may optionally override metadata for this shot:

```json
{
  "make": "Custom Rig",
  "model": "openDSLM (IMX585)",
  "artist": "Unit 01"
}
```

If another session is active the daemon returns HTTP 409; if the shot takes too
long it returns HTTP 503.

### `POST /recordings/video`
####** UNSTABLE **
Starts a CinemaDNG video recording that runs until `DELETE /recordings/video` is
called. The JSON body **must** provide a destination folder name via `path`,
`directory`, `folder`, or `folder_name`. Relative entries are used as-is, so
provide absolute paths or pre-join them with the configured `output_dir`.

Example:

```json
{"directory": "/ssd/RAW/20240520_clip"}
```

If a recording is already running the daemon returns HTTP 409.

### `DELETE /recordings/video`

Stops the active video recording. Returns HTTP 409 if nothing is running.

### `GET /preview`

Returns the most recent preview frame as a JPEG (or HTTP 503 if no frame is
available). Clients should not poll faster than the camera frame rate.

### `GET /preview/stream`

Streams an MJPEG feed suitable for browsers. Only one client is allowed at a
time; additional clients receive HTTP 409. While streaming, `/preview` requests
will return HTTP 503 because the preview buffer is busy.

### Compatibility alias

Legacy clients can still drive sessions via:

- `POST /session` with `{ "mode": "still" | "video" | "none" }`
- `DELETE /session` to stop the current video capture

Prefer the dedicated endpoints for clarity.

## Example commands

```bash
# Status and advertised preview pipelines
curl http://localhost:8400/status | jq

# Update exposure
curl -X POST http://localhost:8400/settings \
     -H 'Content-Type: application/json' \
     -d '{"fps":25.0,"auto_exposure":false,"shutter_us":40000,"analogue_gain":2.0}'

# Trigger a still with metadata overrides
curl -X POST http://localhost:8400/capture/still \
     -H 'Content-Type: application/json' \
     -d '{"artist":"Unit 01","software":"openDSLM daemon (alpha)"}'

# Begin a video capture into an explicit folder
curl -X POST http://localhost:8400/recordings/video \
     -H 'Content-Type: application/json' \
     -d '{"directory":"/ssd/RAW/20240520_clip"}'

# Stop the active recording
curl -X DELETE http://localhost:8400/recordings/video

# Fetch a one-off JPEG preview
curl http://localhost:8400/preview --output preview.jpg
```
