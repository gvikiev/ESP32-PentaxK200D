/*
 * Camera Service: shooting and photo transfer policy for the K200D over
 * Pentax SCSI. Owns the USB link, BOT and Pentax SCSI instances and
 * serialises all camera access behind one mutex. Contains no HTTP code: the
 * HTTP/WebSocket layer calls these functions and consumes the photo_stream.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "photo_stream.h"
#include "pentax_status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PS_STATE_DISCONNECTED = 0,
    PS_STATE_WRONG_USB_MODE, /* camera attached but not in [PC] / Mass Storage mode */
    PS_STATE_READY,
    PS_STATE_BUSY,
} pentax_state_t;

typedef struct {
    pentax_state_t state;
    uint32_t buffer_mask;      /* bit n set = camera image buffer n holds a photo */
    uint32_t bytes_done;       /* current/last download progress */
    uint32_t bytes_total;      /* 0 if unknown */
    char last_error[96];
} pentax_status_summary_t;

/* Progress notification (state/bytes change). Called from service tasks;
 * keep it short and non-blocking (e.g. post to a queue for the WebSocket). */
typedef void (*pentax_progress_cb_t)(const pentax_status_summary_t *st, void *ctx);

esp_err_t pentax_service_start(pentax_progress_cb_t cb, void *ctx);
void pentax_service_get_status(pentax_status_summary_t *out);

/* Fetches a fresh 408-byte status buffer ([00 08]) from the camera when the
 * link is free, or falls back to the last successfully parsed one when it
 * is busy (e.g. mid-capture/download). Returns ESP_ERR_INVALID_STATE if no
 * full status has ever been parsed yet (camera never connected). */
esp_err_t pentax_service_get_full_status(k200d_status_t *out);

/* Half-press + full-press, then waits until a photo appears in the camera's
 * buffer or timeout_ms elapses. */
esp_err_t pentax_service_capture(uint32_t timeout_ms);

/* Starts streaming the newest buffered photo. On success the caller owns
 * *out and must finish with pentax_service_download_end(). */
esp_err_t pentax_service_download_start(photo_stream_t **out);
/* Aborts (if still running), waits for the producer to finish, frees the stream. */
void pentax_service_download_end(photo_stream_t *s);

/* Frees the photo in camera buffer 0 (after a successful transfer). */
esp_err_t pentax_service_delete_photo(void);

#ifdef __cplusplus
}
#endif
