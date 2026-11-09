/*
 * HTTP + WebSocket front end for the Pentax SCSI backend (stage 2).
 *
 *   GET    /                     minimal control page
 *   GET    /api/pentax/status    JSON status + live camera parameters + capability matrix
 *   POST   /api/pentax/capture   take a photo, wait until it is in the camera buffer
 *   POST   /api/pentax/param     set one camera parameter, body {"param":"iso","value":400,...}
 *   GET    /api/pentax/photo     stream the buffered photo (chunked, from the PSRAM queue)
 *   DELETE /api/pentax/photo     free the camera buffer
 *   GET    /ws                   WebSocket: JSON status/progress pushes
 */
#pragma once

#include "esp_err.h"
#include "pentax_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Pass to pentax_service_start() as the progress callback. Safe to call
 * before pentax_http_start() (it then does nothing). */
void pentax_http_progress_cb(const pentax_status_summary_t *st, void *ctx);

esp_err_t pentax_http_start(void);

#ifdef __cplusplus
}
#endif
