# oDSLMd Daemon Extract

This directory contains a standalone build of the rpicam settings/preview daemon
and the minimum shared support code copied from the original `rpicam-apps`
project.  It is intended to make it easier to evolve the daemon independently of
other camera applications while preserving attribution and licensing.

The layout mirrors the upstream source tree so that existing include paths
remain valid:

* `core/`, `encoder/`, `image/`, `output/`, `preview/`, `post_processing_stages/`,
  and `utils/` provide the common infrastructure the daemon relies on. Preview
  assists that enrich the live feed (focus peaking, zebras, false colour, …)
  now live under `post_processing_stages/assist/`.
* `daemon/` contains the HTTP controller implementation.
* `apps/` supplies the `rpicam-daemon` entry point and Meson build rules.

## Building

```
meson setup build
ninja -C build 
```

The resulting binary links against the locally built `rpicam_app` support
library and exports no additional applications by default.

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
