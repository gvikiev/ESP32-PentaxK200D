# PentaxHub

Firmware for an ESP32-S3 that turns a Pentax K200D into a WiFi-controllable
camera:

- **USB host, two camera backends at once**: the ESP32-S3's native USB-OTG
  port acts as a USB host. The K200D's `[USB Connection]` menu offers two
  modes, and this firmware talks to whichever one the camera is actually
  in -- no rebuild, reflash or reboot needed to switch:
  - **`[PC]` (Pentax vendor SCSI over USB Mass Storage)** -- the primary,
    far more capable mode: full exposure/image-quality/AF parameter
    control, shutter/AF/green-button/dust-removal/bulb, all 10 RAM buffer
    slots, and (unverified on real hardware) read-only SD card browsing.
    See `docs/pentax_k200d_architecture_gemeni.md` for the protocol.
  - **`[PictBridge]` (standard PTP)** -- browsing/downloading photos
    already on the card; whether the K200D's PictBridge firmware answers
    `InitiateCapture` for remote shutter release is not confirmed.
  - Both backends run all the time; whichever USB interface class the
    attached camera exposes decides which one has a camera to talk to
    (`main/usb_host_shared.c`). Unplug the camera, switch its menu, plug
    it back in -- the firmware follows along.
- **WiFi access point**: the board creates a WiFi network called
  `PentaxHub` (configurable).
- **Web UI**: connect a phone/laptop to that network and open
  `http://192.168.4.1/` for remote shutter release, parameter control, a
  photo gallery with thumbnails/download/delete, and (in `[PC]` mode) SD
  card browsing.

## Important: has this actually been confirmed to work with a K200D?

**Partially, and this needs to be verified on real hardware.** A few
things are worth knowing before you build a project around this:

- The `[PC]` / Pentax vendor SCSI protocol (shutter, parameters, RAM
  buffer download) is sourced from the `pkTriggerCord` project's reverse
  engineering (see `docs/REFERENCES.md`) and exercises the documented
  command set, but has not been run against a physical K200D by this
  firmware. SD card browsing over plain SCSI `READ(10)` alongside the
  vendor channel is explicitly an unverified hypothesis (H-1 in
  `docs/pentax_k200d_architecture_gemeni.md`).
- Pentax has no vendor-specific PTP operation codes in `libgphoto2`
  (its Vendor Extension ID `0x0000000D` exists only for identification),
  so the `[PictBridge]` backend only uses the standard ISO 15740
  operation set (OpenSession, GetDeviceInfo, GetObjectHandles/Info,
  GetObject, GetThumb, InitiateCapture, Get/SetDevicePropValue, ...).
- Whether the K200D's PictBridge firmware answers *plain* PTP operations
  beyond the PictBridge/DPS printing commands (in particular
  `InitiateCapture` for remote shutter release) is **not confirmed**.
  Camera browsing/downloading is far more likely to work than remote
  capture there. `[PC]` mode's dedicated shutter command doesn't have
  this ambiguity.
- If you plug in the camera and the UI shows a "wrong USB mode" state for
  the backend you expected, check `[USB Connection]` in the camera menu
  matches the mode you intended (`[PC]` for the full feature set,
  `[PictBridge]` for basic PTP browsing).

If you confirm (or disprove) something on real K200D hardware, that's
worth recording for the next person -- please update this section.

## Hardware

- An ESP32-S3 board with a USB-OTG port wired to a USB-A host connector
  (or a USB-OTG/host adapter cable), full-speed (12 Mbit/s) is enough.
- A USB A-to-mini-B cable to the K200D (its port is USB 2.0 Mini-B).
- The K200D's `[USB Connection]` (Menu 3, "USB connection") set to
  `[PC]` for the full feature set, or `[PictBridge]` for basic PTP
  browsing only.

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

1. Connect the camera to the ESP32-S3's USB port (power it on first),
   in whichever `[USB Connection]` mode you want (see Hardware above).
2. Connect your phone/laptop to the `PentaxHub` WiFi network (password
   configured at build time, default `pentaxhub`).
3. Open `http://192.168.4.1/`. The page you get depends on which backend
   currently has a camera attached (`[PC]`'s dashboard, or `[PictBridge]`'s
   gallery/settings page); with nothing attached yet it defaults to the
   `[PC]` dashboard.
4. In `[PC]` mode: exposure/image-quality/AF controls, shutter/AF/green
   button/dust removal/bulb, the RAM buffer (10 slots), and SD card
   browsing live under `/api/pentax/*` and `/api/sd/*` (see
   `main/pentax_http.h`). In `[PictBridge]` mode: the gallery lists
   photos already on the card and a settings panel lists whatever
   `DevicePropertiesSupported` the camera reports, under `/api/*` (see
   `main/web_server.h`).

## Known limitations

- Only one camera/USB device at a time.
- The HTTP server runs its default single-task configuration: one
  request is served at a time (a large photo download will make the
  gallery/status UI wait until it finishes). Fine for the single-user,
  single-camera use case this is built for.
- The PTP backend's object/property listing performs one PTP round trip
  per item, so listing very large photo counts or property counts is
  intentionally capped (`/api/objects` pages at up to 60 items per
  request; properties at 32).
- No on-board storage: photo downloads/thumbnails are streamed straight
  from the camera through the ESP32-S3 to the browser, nothing is cached
  on the device.

## Code layout

| File | Purpose |
| --- | --- |
| `main/pentaxhub_main.c` | Startup/orchestration (`app_main`): starts both backends and one shared HTTP server. |
| `main/usb_host_shared.h/.c` | Shared `usb_host_install()` + library daemon task, so both backends' USB Host clients can coexist. |
| `main/wifi_ap.h/.c` | WiFi access point setup. |
| **`[PC]` / Pentax SCSI backend** | |
| `main/bot.h/.c` | USB Mass Storage Bulk-Only Transport (CBW/CSW, stall + reset recovery). USB-stack independent (`bot_io_t`). |
| `main/pentax_scsi.h/.c` | Pentax vendor SCSI (`F0 4F/24/26/49`) on top of BOT. |
| `main/pentax_status.h/.c` | Parser for the 408-byte `[00 08]` full status buffer. |
| `main/usb_msc_link.h/.c` | ESP-IDF USB Host binding that provides the BOT pipes. |
| `main/photo_stream.h/.c` | Bounded PSRAM block queue, USB reader -> HTTP sender. |
| `main/pentax_sd.h/.c` | Read-only FatFs access to the SD card over standard SCSI `READ(10)`. |
| `main/pentax_service.h/.c` | Camera service: parameters, actions, RAM buffer download/delete, SD passthrough. |
| `main/pentax_http.h/.c` | HTTP API (`/api/pentax/*`, `/api/sd/*`) + WebSocket `/ws` progress. |
| **`[PictBridge]` / PTP backend** | |
| `main/ptp.h/.c` | PTP container format + wire helpers (no I/O). |
| `main/ptp_transport.h/.c` | USB host plumbing: device enumeration, bulk/interrupt transfers, the generic PTP transaction engine. |
| `main/camera.h/.c` | High level camera manager: session handling, object listing/streaming, capture, device properties. |
| `main/web_server.h/.c` | HTTP API (`/api/*`) + static UI assets. |
| `main/index.html`, `app.js`, `style.css` | The PTP backend's web UI (embedded into the firmware image). |
