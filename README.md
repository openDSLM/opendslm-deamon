# oDSLMd Daemon Extract

This directory contains a standalone build of the openDSLM settings/preview
daemon (referred to as the `opendslm-daemon`) and the minimum shared support
code copied from the original `rpicam-apps` project.  It is intended to make it
easier to evolve the daemon independently of other camera applications while
preserving attribution and licensing.  The codebase currently targets Raspberry
Pi 4B/5 boards paired with an IMX585 "StarlightEye" module but should be readily
adaptable to other libcamera-supported sensors.

The layout mirrors the upstream source tree so that existing include paths
remain valid:

* `core/`, `encoder/`, `image/`, `output/`, `preview/`, `post_processing_stages/`,
  and `utils/` provide the common infrastructure the daemon relies on. Preview
  assists that enrich the live feed (focus peaking, zebras, false colour, …)
  now live under `post_processing_stages/assist/`.
* `daemon/` contains the HTTP controller implementation.
* `apps/` supplies the `opendslm-daemon` entry point and Meson build rules.

## Status and warnings

* ⚠️ The HTTP server ships without TLS/authentication or request throttling. Run
  it on trusted networks only, ideally behind a reverse proxy that enforces
  authentication and size limits.
* ⚠️ MJPEG streaming (`/preview/stream`) supports a single client at a time; new
  connections are rejected until the previous viewer disconnects.
* ⚠️ CinemaDNG video mode remains **experimental** and is **not** considered
  working yet. The daemon writes every RAW frame to disk, expects you to manage
  clip directories manually, and may drop frames—treat it as a preview only.
* ⚠️ Slow shutters can exceed the default still capture timeout. Adjust
  expectations and keep the daemon responsive while we add configurable limits.

## Default UI integration

The daemon powers the [openDSLM GTK UI](https://github.com/openDSLM/gtk-ui), and
current UI builds expect the HTTP API documented here. Start `opendslm-daemon`
before launching the GTK client so it can attach to `/status`, `/settings`,
`/preview`, and the GStreamer shared memory endpoints. GTK record/stop buttons
should call `/recordings/mp4/start` and `/recordings/mp4/stop`, bubble up any
errors (RAW conflicts, unsupported codecs, encoder failures), and reflect the
`/status` fields for filename, elapsed time, codec, bitrate, GOP mode, and
whether audio is active. If you change ports or socket paths, export matching
overrides for the UI environment variables.

## Compatibility (alpha)

| Platform / Sensor | Status | Notes |
| --- | --- | --- |
| Raspberry Pi 5 + IMX585 StarlightEye (Will Whang libcamera fork) | ✅ Tested | Reference platform; requires Will Whang’s IMX585 driver and libcamera build. |
| Raspberry Pi 4B + IMX585 StarlightEye | ⚠️ Expected | Should work if the IMX585 driver/libcamera fork is installed; currently unverified. |
| Raspberry Pi 5 + OneInchEye (IMX678) | ⚠️ Untested | Expected to work but needs mode strings/metadata verification; may require small code tweaks. |
| Raspberry Pi 5 + FourthThirdsEye (IMX492) | ⚠️ Untested | Sensor maps exist but require review of RAW mode timings. |
| Other libcamera sensors | 🛠️ Community help needed | Should build, but add your mode strings/metadata and report results. |

The IMX585 path assumes the driver and modified libcamera stack from Will Whang. Stock libcamera has not been validated for this module yet.

### Quickstart (RPi 5 + IMX585)

1. Install Raspberry Pi OS (64-bit) and the IMX585 driver plus Will Whang’s libcamera build.
2. Configure and build:
   ```
   meson setup build
   ninja -C build opendslm-daemon
   ```
3. Run the daemon:
   ```
   ./build/apps/opendslm-daemon --port 8400
   ```
4. (Optional) Start the GTK UI from the companion repo; it expects the default `http://127.0.0.1:8400` endpoint.

If you bring up a new sensor, update `metadata_config.hpp.in` /
`metadata_make` options and share the `--list-cameras` output so we can extend
the defaults.

## Building

1. Install the usual Raspberry Pi camera dependencies (libcamera, Meson ≥ 0.64,
   ninja-build, GStreamer if preview streaming is required).
2. Configure a build directory (example uses the default release profile):
   ```
   meson setup build
   ```
3. Compile the daemon and supporting library:
   ```
   ninja -C build
   ```
   The resulting binary is `build/apps/opendslm-daemon` (the old `rpicam-daemon`
   name is retired).
4. Run the service on the Pi:
   ```
   ./build/apps/opendslm-daemon --port 8400
   ```

The resulting binary links against the locally built `rpicam_app` support
library and exports no additional applications by default. See
[`opendslm-daemon.md`](opendslm-daemon.md) for the HTTP API and workflow guide.

## Licensing

All files are copied verbatim from the upstream project and retain their
original copyright and SPDX headers.  The top-level `license.txt` from
`rpicam-apps` is included for convenience.

## GStreamer preview

Build with `meson setup build -Denable_gstreamer=enabled` so the daemon links against libgstreamer and enables the preview backend.

When GStreamer support is available the daemon now publishes preview frames through a shared-memory `shmsink` by default. The
socket lives at `/tmp/opendslm-preview.sock` (configurable via `--preview-gstreamer-socket`). The matching client pipeline is
advertised in `/status` under `preview_client_pipeline` and looks like:

```
shmsrc socket-path="/tmp/opendslm-preview.sock" is-live=true do-timestamp=true ! \
  queue max-size-buffers=2 leaky=downstream ! video/x-raw,format=RGBA ! gtk4paintablesink
```

Replace `gtk4paintablesink` with any sink suitable for your UI. For example, use `autovideosink` for a quick local test window:

```
gst-launch-1.0 shmsrc socket-path=/tmp/opendslm-preview.sock is-live=true do-timestamp=true ! \
  queue max-size-buffers=2 leaky=downstream ! video/x-raw,format=RGBA ! autovideosink
```

The daemon's default preview pipeline inserts one or two `queue` elements ahead of the shared-memory sink to absorb jitter from
MP4 encoding. Set `ODS_PREVIEW_EXTRA_QUEUE=0` if you want to drop the second queue for a tighter latency budget.

## MP4 recording + picture profiles

The HTTP API exposes `/recordings/mp4/start` and `/recordings/mp4/stop` to control
hardware-accelerated MP4 captures while leaving the preview pipeline running.
Recording status (filename, elapsed time, codec/bitrate, GOP or All-Intra
settings, and whether audio is active) is returned via `/status` so the GTK UI can
keep its widgets in sync and surface any start-up errors (unsupported codec,
conflicting RAW session, etc.).

The GTK-facing `assets/picture_profiles.json` file ships `flat` and `standard`
presets that bundle ISP look controls and encoder defaults. The flat preset dials
back contrast/saturation/sharpness, uses light denoise with manual AWB gains, and
targets a high-bitrate All-Intra HEVC recording with inline SPS/PPS headers.

## Preview assist stages

The daemon exposes a dedicated directory for preview assist post-processing
stages in `post_processing_stages/assist/`. Assist stages derive from the shared
`PreviewAssistStage` helper, ensuring they automatically bind to the YUV420
viewfinder stream and receive convenient access to the luma/chroma planes.

The first assist provided out of the box is `focus_peaking`, implemented in
`assist/focus_peaking_stage.cpp` with a matching configuration template under
`assets/focus_peaking.json`. Enable it by referencing the new module from a
post-processing pipeline definition, e.g.:

```
{
    "post_process": [
        {
            "stage": "focus_peaking"
        }
    ]
}
```

This architecture intentionally mirrors the approach used by other post-
processing modules so that future assists like exposure zebras or false colour
overlays can reuse the same scaffolding.

To override the internal pipeline entirely, pass `--preview-gstreamer <pipeline>`; the string still describes the elements
downstream of the daemon-managed `appsrc` named `rpicam_src`. Use `--preview-gstreamer=none` or `--no-preview-gstreamer` to
disable the GStreamer preview backend when not required.
