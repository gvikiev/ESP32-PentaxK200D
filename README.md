# PentaxHub

Firmware for an ESP32-S3 that turns a Pentax K200D into a WiFi-controllable
camera:

- **USB host + PTP**: the ESP32-S3's native USB-OTG port acts as a USB
  host and speaks PTP (Picture Transfer Protocol, ISO 15740 / PIMA 15740 -
  the "USB Still Image" device class) to the camera, using the standard
  ESP-IDF `usb_host` library.
- **WiFi access point**: the board creates a WiFi network called
  `PentaxHub` (configurable).
- **Web UI**: connect a phone/laptop to that network and open
  `http://192.168.4.1/` for remote shutter release, a photo gallery with
  thumbnails/download/delete, and a basic camera-settings panel.

## Important: has this actually been confirmed to work with a K200D?

**Partially, and this needs to be verified on real hardware.** A few
things are worth knowing before you build a project around this:

- The K200D predates Pentax's PTP-based "tethered capture" cameras. Its
  USB menu only offers `[PC]` and `[PictBridge]` connection modes --
  there is no documented `[PTP]` mode. `[PC]` is a USB Mass Storage
  interface (reading the SD card as a USB drive), which this firmware
  **cannot** talk to (that would require a completely different,
  file-system-level USB driver). `[PictBridge]` is built on top of the
  same PTP transport this firmware speaks, and is the mode you must
  select in the camera's menu for PentaxHub to see it at all.
- Pentax has no vendor-specific PTP operation codes in `libgphoto2`
  (its Vendor Extension ID `0x0000000D` exists only for identification),
  so this firmware only uses the standard ISO 15740 operation set
  (OpenSession, GetDeviceInfo, GetObjectHandles/Info, GetObject, GetThumb,
  InitiateCapture, Get/SetDevicePropValue, ...). There is nothing
  Pentax-specific to add here.
- Whether the K200D's PictBridge firmware answers *plain* PTP operations
  beyond the PictBridge/DPS printing commands (in particular
  `InitiateCapture` for remote shutter release) is **not confirmed**.
  Camera browsing/downloading is far more likely to work than remote
  capture. The firmware is written defensively around this: it reports
  whether `InitiateCapture` is in the camera's advertised
  `OperationsSupported` list (`/api/status` -> `capture_supported`, shown
  in the UI), and the "Take photo" button is only enabled when the
  camera claims to support it.
- If you plug in the camera and the UI shows **"Unsupported USB mode"**,
  go into the camera menu and set `[USB Connection]` to `[PictBridge]`
  (not `[PC]`).

If you confirm (or disprove) capture support on real K200D hardware,
that's worth recording for the next person -- please update this section.

## Hardware

- An ESP32-S3 board with a USB-OTG port wired to a USB-A host connector
  (or a USB-OTG/host adapter cable), full-speed (12 Mbit/s) is enough.
- A USB A-to-mini-B cable to the K200D (its port is USB 2.0 Mini-B).
- The K200D set to `[USB Connection]` = `[PictBridge]` (Menu 3 on the
  K200D, "USB connection").

## Building

This project targets ESP-IDF **v5.5** on **ESP32-S3**. The included
`.devcontainer` is the easiest way to get a matching toolchain:

```bash
idf.py set-target esp32s3
idf.py menuconfig   # optional: change the AP SSID/password under
                     # "PentaxHub Configuration"
idf.py -p PORT flash monitor
```

## Using it

1. Set the camera to `[PictBridge]` USB mode and connect it to the
   ESP32-S3's USB port (power the camera on first).
2. Connect your phone/laptop to the `PentaxHub` WiFi network (password
   configured at build time, default `pentaxhub`).
3. Open `http://192.168.4.1/`.
4. The status panel shows whether a camera is connected and whether
   remote capture is supported; the gallery lists photos already on the
   card (tap one to view/download/delete); "Camera settings" lists
   whatever `DevicePropertiesSupported` the camera reports (read-only
   values are shown disabled).

## Known limitations

- Only one camera/USB device at a time.
- The HTTP server runs its default single-task configuration: one
  request is served at a time (a large photo download will make the
  gallery/status UI wait until it finishes). Fine for the single-user,
  single-camera use case this is built for.
- The object/property listing performs one PTP round trip per item, so
  listing very large photo counts or property counts is intentionally
  capped (`/api/objects` pages at up to 60 items per request; properties
  at 32).
- No on-board storage: downloads/thumbnails are streamed straight from
  the camera through the ESP32-S3 to the browser, nothing is cached on
  the device.

## Code layout

| File | Purpose |
| --- | --- |
| `main/ptp.h/.c` | PTP container format + wire helpers (no I/O). |
| `main/ptp_transport.h/.c` | USB host plumbing: device enumeration, bulk/interrupt transfers, the generic PTP transaction engine. |
| `main/camera.h/.c` | High level camera manager: session handling, object listing/streaming, capture, device properties. |
| `main/wifi_ap.h/.c` | WiFi access point setup. |
| `main/web_server.h/.c` | HTTP server: static UI + JSON/streaming API. |
| `main/index.html`, `app.js`, `style.css` | The web UI (embedded into the firmware image). |
| `main/pentaxhub_main.c` | Startup/orchestration (`app_main`). |
