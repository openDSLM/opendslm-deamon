# oDSLMd Daemon Extract

This directory contains a standalone build of the rpicam settings/preview daemon
and the minimum shared support code copied from the original `rpicam-apps`
project.  It is intended to make it easier to evolve the daemon independently of
other camera applications while preserving attribution and licensing.

The layout mirrors the upstream source tree so that existing include paths
remain valid:

* `core/`, `encoder/`, `image/`, `output/`, `preview/`, `post_processing_stages/`,
  and `utils/` provide the common infrastructure the daemon relies on.
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

Run `rpicam-daemon` with `--preview-gstreamer` to channel the camera viewfinder into a custom GStreamer pipeline.
The option expects the elements that should follow an `appsrc` named `rpicam_src`; the daemon creates the
source, feeds I420 frames at the configured preview framerate, and keeps timestamps in sync for live rendering.

Example (renders directly to GTK4):

```
./build/apps/rpicam-daemon --preview-gstreamer "queue ! videoconvert ! gtk4paintablesink" --framerate 30
```

Any pipeline that can consume planar I420 video can be used, so you can replace the sink with a shared
`gtk4paintablesink`, `waylandsink`, network sinks, or additional processing stages before the UI.
When the daemon is idle the pipeline pauses in READY state and resumes automatically as soon as new frames arrive.
