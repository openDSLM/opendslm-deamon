# opendslm-daemon

`opendslm-daemon` is a lightweight HTTP controller that drives the openDSLM
libcamera pipeline for DSLR-style workflows. It exposes endpoints to configure
sensor parameters, trigger CinemaDNG stills, start/stop RAW video sequences, and
stream preview frames. The current alpha has only been validated on Raspberry Pi
5 with the IMX585 “StarlightEye” module using Will Whang’s IMX585 driver and
libcamera build; see the [compatibility matrix](README.md#compatibility-alpha)
for platforms that are expected but untested.

On startup the daemon now probes the host (board model, kernel, OS build) and
enumerates every libcamera sensor that is present. The detected metadata,
including every advertised RAW resolution/mode, is cached and exposed via the
HTTP API so clients can present accurate options in their UI without hardcoding
per-sensor tables. If a sensor is found, the daemon also seeds the metadata
defaults (model/unique model) with the detected module so `/metadata` immediately
reflects the attached hardware without any manual overrides.

## Building

The daemon is built alongside the shared `rpicam_app` support library:

```bash
meson setup build
ninja -C build opendslm-daemon
```

The compiled binary lives at `build/apps/opendslm-daemon`; the legacy
`rpicam-daemon` name is no longer used.

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

### Where image processing happens

All ISP processing (AWB, denoise, sharpening, colour correction) happens inside
libcamera before frames reach the daemon’s `appsrc`. The shared preview/feed
already carries those adjustments, so you can tee the same source into a
recording branch without losing manual ISO/shutter/white balance controls. If
you need a different look for recording, add a downstream colour-conversion or
encoding branch in the GStreamer pipeline after the `appsrc` rather than trying
to bypass the ISP.

### Gtk UI integration

The daemon is the backend for the
[openDSLM GTK UI](https://github.com/openDSLM/gtk-ui). Start the daemon first
(`./build/apps/opendslm-daemon --port 8400`) so the UI client can attach to
`/status`, `/settings`, `/preview`, and the GStreamer shared-memory socket. If
you customise ports or socket paths, export matching overrides before launching
the GTK frontend.

### Known limitations (alpha warning)

- ⚠️ The HTTP server does **not** provide TLS, authentication, or request
  throttling. Only expose it on trusted networks or place it behind a reverse
  proxy that enforces access control and request size limits.
- ⚠️ `/preview/stream` accepts a single MJPEG client at a time. Additional
  connections receive HTTP 409 until the active viewer disconnects.
- ⚠️ CinemaDNG video capture is still experimental, currently unreliable, and
  writes every frame to disk. Treat it as **not working** for production; expect
  to manage clip directories manually and babysit disk usage.
- ⚠️ Still captures block until a frame finishes writing; very long exposures
  can exceed the default timeout and return HTTP 503 even though the camera is
  still busy.

## HTTP endpoints

All endpoints speak UTF-8 JSON unless otherwise noted.

### `GET /status`

Returns the current session state, camera settings, preview pipelines, and the
summary of the latest capture (or `null` if nothing has been recorded yet). The
payload now also contains a `hardware` block that mirrors `GET /hardware`
described below, so most clients only need a single request to populate camera
settings and available resolutions.

`recording` contains the MP4 controller status with the last requested
parameters. The `config` sub-object surfaces the requested `width`, `height`,
`fps`, `bitrate`, GOP (`intra`, set `1` for All-Intra), `codec`, `profile`,
`level`, `inline` (write SPS/PPS on every I‑frame), `frames`, and rolling
options (`save_pts`, `segment`, `split`). The `audio` sub-object reports whether
audio is enabled along with the chosen `codec`, `source` (`pulse` or `alsa`),
`device`, `channels`, `bitrate` (bps), `samplerate` (Hz), `auto_gain`/`gain_db`,
and `sync_us`/`av_sync` offset applied to the audio track. When recording is
live, `elapsed_ms` counts how long the encoder has been running and `filename`
echoes the active clip name.

### `GET /hardware`

Returns a snapshot of the detected platform and all attached sensors. A typical
payload looks like:

```json
{
  "board_model": "Raspberry Pi 5 Model B Rev 1.1",
  "board_revision": "d03141",
  "os_name": "Debian GNU/Linux 12 (bookworm)",
  "kernel": "6.6.20-v8+",
  "cameras": [
    {
      "id": "unicam-0",
      "model": "Sony IMX585",
      "location": "1",
      "modes": [
        {"width": 3840, "height": 2160, "bit_depth": 12, "format": "SRGGB12_CSI2P", "max_fps": 24.00},
        {"width": 1920, "height": 1080, "bit_depth": 12, "format": "SRGGB12_CSI2P", "max_fps": 60.00}
      ]
    }
  ]
}
```

Fields:

- `board_model`, `board_revision`, `os_name`, `kernel`: identifiers scraped from
  `/proc/device-tree`, `/proc/cpuinfo`, `/etc/os-release`, and `uname(2)`.
- `cameras`: one entry per libcamera sensor (USB webcams are excluded).
  - `location` is the raw integer reported by libcamera (`0` front, `1` back,
    `2` external).
  - `modes` lists every advertised RAW sensor mode with pixel format, frame size,
    bit depth, and the maximum FPS determined by applying the mode and querying
    `FrameDurationLimits`.

The same structure is embedded in `/status` under the `hardware` key.

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
| `output_dir` | string | Directory where CinemaDNG frames are written (configure with `--output-dir`; choose any writable path). |
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

### `POST /recordings/video` (unstable)
Starts a CinemaDNG video recording that runs until `DELETE /recordings/video` is
called. **Alpha warning:** this path is still experimental, regularly drops
frames, and expects you to provision the storage location. Treat it as a preview
feature rather than a working recorder.

The JSON body **must** provide a destination folder name via `path`,
`directory`, `folder`, or `folder_name`. Relative entries are used as-is, so
provide absolute paths or pre-join them with the configured `output_dir`. Make
sure the directory exists and has ample free space before starting a capture.

Example:

```json
{"directory": "/path/to/RAW/20240520_clip"}
```

If a recording is already running the daemon returns HTTP 409. If anything goes
wrong mid-capture you must clean up the partially written DNG files manually.

### `DELETE /recordings/video`

Stops the active video recording. Returns HTTP 409 if nothing is running.

### `POST /recordings/mp4/start`

Starts an MP4 recording using the hardware encoder and the preview pipeline
configured for YUV420. Parameters (all optional except `filename`):

| Field | Type | Notes |
| --- | --- | --- |
| `filename` | string | Required output path. `.mp4` containers are muxed; `.h265`/`.hevc` switches the encoder to HEVC. |
| `width` / `height` | int | Override video resolution; defaults to the current camera mode. |
| `fps` | float | Requested frame rate; capped to the probed sensor limit and mirrored to the preview caps. |
| `bitrate` | int | Target video bitrate in bits per second. |
| `codec` | string | `h264`/`h264_v4l2m2m`, `hevc`/`hevc_v4l2m2m`, `libx264`, or `libx265`. |
| `profile` / `level` | string | Encoder profile and level strings passed through to libav when supported. |
| `intra` | int | GOP length; set to `1` for All-Intra or leave unset for long-GOP. |
| `inline` | bool | Emit SPS/PPS on every I-frame (decoder-friendly All-Intra). |
| `frames` | int | Stop after this many frames (optional cap). |
| `save_pts` | string | Write presentation timestamps to the given file. |
| `segment` | int | Milliseconds per segment when rolling recordings. |
| `split` | bool | Create a new file after each pause/resume cycle. |
| `libav_audio` / `audio` | bool | Enable/disable audio (default `true`). |
| `audio_codec` | string | Audio codec passed to libav (default `aac`). |
| `audio_source` | string | `pulse` or `alsa` (default `pulse`). |
| `audio_device` | string | Device name for the chosen source (default `default`). |
| `audio_channels` | int | Number of channels; omit to use the source default. |
| `audio_bitrate` | int | Audio bitrate in bits per second (default 32000). |
| `audio_samplerate` | int | Audio sample rate in Hz; omit to follow the source. |
| `audio_auto_gain` | bool | Enable lightweight AGC on the audio input (default `false`). |
| `audio_gain_db` | float | Apply a fixed pre-encode gain in decibels (default `0.0`). |
| `audio_sync_us` / `av_sync` | int | Microsecond offset applied to audio relative to video (positive or negative). |

The daemon rejects requests if a RAW recording is running, if an MP4 capture is
already active, or if the encoder/muxer cannot be initialised. Error responses
echo the requested filename to simplify UI messaging.

### `POST /recordings/mp4/stop`

Stops the active MP4 recording and returns the updated `/status` payload.
Returns HTTP 409 if nothing is running.

### ISP look controls and presets

`POST /settings` now accepts additional ISP tuning fields alongside exposure:

- `contrast`, `saturation`, `sharpness`, `brightness`
- `denoise` (`auto`, `off`, `cdn_off`, `cdn_fast`, `cdn_hq`)
- `awb` plus manual `awb_gain_r` / `awb_gain_b` when locking white balance
- `tuning_file` for users who want to swap libcamera tuning JSON blobs

The shipped `assets/picture_profiles.json` file defines two GTK/UI-friendly
presets you can load and tweak before starting a recording:

- **Flat / grade-friendly**: lower contrast/saturation/sharpness, light denoise,
  manual AWB gains, HEVC, high bitrate, All-Intra, `inline=true`.
- **Standard / straight from cam**: near-default ISP values, AWB auto, H.264,
  long-GOP, moderate bitrate.

### Audio level overlay notes

Audio is captured via libav and not exposed in the preview stream. If you need
live meters in your UI, run a sidecar GStreamer probe against the same Pulse
source and listen for `level` element messages. Example:

```bash
gst-launch-1.0 -q pulsesrc device=default ! level interval=100000000 ! fakesink silent=true
```

Parse the bus `level` messages for RMS/peak data and overlay them in the UI.

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
# Detected board/sensor inventory
curl http://localhost:8400/hardware | jq

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
     -d '{"directory":"/path/to/RAW/20240520_clip"}'

# Stop the active recording
curl -X DELETE http://localhost:8400/recordings/video

# Fetch a one-off JPEG preview
curl http://localhost:8400/preview --output preview.jpg
```
