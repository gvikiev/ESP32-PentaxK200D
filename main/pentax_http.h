/*
 * HTTP + WebSocket front end for the Pentax SCSI backend. Registers its
 * routes on a shared httpd server (see pentaxhub_main.c), which also hosts
 * the PTP backend's routes (web_server.h) -- both backends run at the same
 * time now; whichever USB interface class the attached camera exposes
 * decides which one actually has a camera to talk to (see
 * docs/pentax_k200d_architecture_gemeni.md section 5.1).
 *
 *   GET    /                            minimal control page (pentax_http_index_handler)
 *   GET    /api/pentax/status           JSON status + live camera parameters + capability matrix
 *   POST   /api/pentax/capture          take a photo, wait until it is in the camera buffer
 *   POST   /api/pentax/af               half-press / autofocus only, no exposure
 *   POST   /api/pentax/green-button     reset exposure to the camera's P-mode defaults
 *   POST   /api/pentax/dust-removal     trigger the sensor's ultrasonic cleaning cycle
 *   POST   /api/pentax/tether           body {"connect":true|false} (default true)
 *   POST   /api/pentax/bulb             body {"duration_ms":30000}, 1000..600000
 *   POST   /api/pentax/param            set one camera parameter, body {"param":"iso","value":400,...}
 *   GET    /api/pentax/photo            ?slot=0..9&type=full|preview|thumb (default slot=0, type=full):
 *                                        stream that RAM buffer slot (chunked, from the PSRAM queue)
 *   DELETE /api/pentax/photo            ?slot=0..9 (default 0): free that RAM buffer slot
 *   GET    /api/sd/list                 ?path=/DCIM/100PENTX (default "/"): JSON directory listing
 *   GET    /api/sd/file                 ?path=/DCIM/100PENTX/IMGP0001.JPG: stream a file, Range supported
 *   GET    /ws                          WebSocket: JSON status/progress pushes
 *
 * The /api/sd endpoints require the SD card to have mounted (see
 * pentax_sd.h); until then they return 409, same as any other
 * camera-busy/not-ready condition.
 */
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include "pentax_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Pass to pentax_service_start() as the progress callback. Safe to call
 * before pentax_http_register() (it then does nothing). */
void pentax_http_progress_cb(const pentax_status_summary_t *st, void *ctx);

/* Registers this backend's routes (everything above except "/") on an
 * already-started httpd server. */
esp_err_t pentax_http_register(httpd_handle_t server);

/* The "/" page content, exposed so pentaxhub_main.c's merged root handler
 * can pick it or web_server_index_handler() depending on which backend
 * currently has a camera attached. */
esp_err_t pentax_http_index_handler(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
