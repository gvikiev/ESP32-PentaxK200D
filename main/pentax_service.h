/*
 * Camera Service: shooting and photo transfer policy for the K200D over
 * Pentax SCSI. Owns the USB link, BOT and Pentax SCSI instances and
 * serialises all camera access behind one mutex. Contains no HTTP code: the
 * HTTP/WebSocket layer calls these functions and consumes the photo_stream.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "photo_stream.h"
#include "pentax_status.h"
#include "bot.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PS_STATE_DISCONNECTED = 0,
    PS_STATE_WRONG_USB_MODE, /* camera attached but not in [PC] / Mass Storage mode */
    PS_STATE_READY,
    PS_STATE_BUSY,
} pentax_state_t;

/* K200D RAM buffer has 10 slots (bufmask bit n = buffer n holds a photo). */
#define PENTAX_RAM_SLOTS 10u

typedef enum {
    PENTAX_BUF_FULL = 0, /* full-resolution image */
    PENTAX_BUF_PREVIEW,  /* buffer type 8 */
    PENTAX_BUF_THUMB,    /* buffer type 9 */
} pentax_buf_kind_t;

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

/* Parameter writes ([18 xx], see docs/pentax_k200d_architecture_gemeni.md
 * section 2.3). Each is framed with DSPTaskWUReq internally. Returns
 * ESP_ERR_INVALID_ARG if the value is outside the range the architecture
 * doc documents for the K200D, ESP_ERR_TIMEOUT if the USB link was busy,
 * or the Pentax status byte mapped through pentax_status_to_err() for a
 * write the camera itself rejected. A successful write best-effort
 * refreshes the cache pentax_service_get_full_status() returns. */
esp_err_t pentax_service_set_shutter_speed(uint32_t nom, uint32_t denom);
esp_err_t pentax_service_set_aperture(uint32_t nom, uint32_t denom);
esp_err_t pentax_service_set_iso(uint32_t iso, uint32_t auto_min, uint32_t auto_max);
esp_err_t pentax_service_set_ev_comp(int32_t nom, int32_t denom);
esp_err_t pentax_service_set_exposure_mode(uint32_t mode);
esp_err_t pentax_service_set_file_format(uint32_t format);        /* 0:JPEG 1:RAW 2:RAW+JPEG */
esp_err_t pentax_service_set_jpeg_quality(uint32_t stars);        /* 1..3 */
esp_err_t pentax_service_set_jpeg_resolution(uint32_t res_idx);   /* 0:10M 1:6M 2:2M */
esp_err_t pentax_service_set_raw_format(uint32_t format);         /* 0:PEF 1:DNG */
esp_err_t pentax_service_set_custom_image_tone(uint32_t tone);    /* 0..5 */
esp_err_t pentax_service_set_af_mode(uint32_t mode);              /* 1:AF.S 2:AF.C */
esp_err_t pentax_service_set_af_area(uint32_t sel);               /* 0:Auto-5 1:Select 2:Spot 3:Auto-11 */
esp_err_t pentax_service_set_af_point(uint32_t point_mask);       /* 11-bit mask */
esp_err_t pentax_service_set_drive_mode(uint32_t mode);

/* Write-only on the K200D: no field in [00 08] reflects these back, so the
 * value passed is cached (and persisted in NVS) and echoed back by
 * pentax_service_get_writeonly_cache() instead of a status read-back. */
esp_err_t pentax_service_set_white_balance(uint32_t mode);
esp_err_t pentax_service_set_wb_tuning(uint32_t mode, int32_t mg, int32_t ba);
esp_err_t pentax_service_set_ae_metering(uint32_t mode);          /* 0:Multi 1:Center 2:Spot */
esp_err_t pentax_service_set_flash_mode(uint32_t mode);
esp_err_t pentax_service_set_flash_ev_comp(int32_t nom, int32_t denom);

typedef struct {
    bool wb_set;            uint32_t wb_mode;
    bool wb_tune_set;        uint32_t wb_tune_mode; int32_t wb_mg, wb_ba;
    bool ae_metering_set;    uint32_t ae_metering_mode;
    bool flash_mode_set;     uint32_t flash_mode;
    bool flash_ev_comp_set;  int32_t flash_ev_comp_nom, flash_ev_comp_denom;
} pentax_writeonly_cache_t;

void pentax_service_get_writeonly_cache(pentax_writeonly_cache_t *out);

/* Half-press + full-press, then waits until a photo appears in the camera's
 * buffer or timeout_ms elapses. */
esp_err_t pentax_service_capture(uint32_t timeout_ms);

/* Half-press / autofocus only (no exposure, nothing lands in the buffer). */
esp_err_t pentax_service_af(void);

/* Pentax "green button": instantly resets exposure to the camera's P-mode
 * defaults. */
esp_err_t pentax_service_green_button(void);

/* Triggers the sensor's ultrasonic dust-removal cycle. */
esp_err_t pentax_service_dust_removal(void);

/* Toggles the camera's own notion of being tethered, independent of the USB
 * Mass Storage link itself (see docs/pentax_k200d_architecture_gemeni.md
 * section 2.3, [10 0A]). */
esp_err_t pentax_service_set_tether(bool connect);

/* Opens the shutter, sleeps for duration_ms, closes it, then waits
 * (timeout_ms) for the resulting frame to appear in the camera's buffer the
 * same way pentax_service_capture() does. duration_ms is not bounded here;
 * callers should apply their own sanity limit (the HTTP layer caps it at
 * 10 minutes). */
esp_err_t pentax_service_bulb_capture(uint32_t duration_ms, uint32_t timeout_ms);

/* Starts streaming the photo in the given RAM buffer slot (0..9) as `kind`.
 * ESP_ERR_INVALID_ARG if slot is out of range, ESP_ERR_NOT_FOUND once the
 * download task finds that slot empty (reported through the stream, not
 * this return value -- see pentax_http.c's first-block handling). On
 * success the caller owns *out and must finish with
 * pentax_service_download_end(). */
esp_err_t pentax_service_download_start(uint32_t slot, pentax_buf_kind_t kind, photo_stream_t **out);
/* Aborts (if still running), waits for the producer to finish, frees the stream. */
void pentax_service_download_end(photo_stream_t *s);

/* Frees the photo in the given RAM buffer slot (0..9). */
esp_err_t pentax_service_delete_photo(uint32_t slot);

/* Raw standard-SCSI passthrough (INQUIRY, READ CAPACITY(10), READ(10), ...),
 * for the SD card FatFs block device driver in pentax_sd.c. Shares the same
 * mutex and bot_t instance as the Pentax vendor channel above, so camera
 * operations and SD reads never interleave on the wire (see
 * docs/pentax_k200d_architecture_gemeni.md section 3). Not for camera
 * control -- use the pentax_service_* functions above for that.
 *
 * `len` bytes are transferred in or out of `buf` depending on `dir`
 * (BOT_DIR_NONE for a command with no data stage). Returns the Pentax BOT
 * transport result; a clean SCSI-level rejection (bad LBA, etc.) comes back
 * as ESP_FAIL without forcing a Pentax session re-handshake, while a
 * transport-level failure does (same policy as every other command here). */
esp_err_t pentax_service_scsi_io(const uint8_t *cdb, uint8_t cdb_len, bot_dir_t dir,
                                  uint8_t *buf, size_t len, uint32_t timeout_ms, size_t *out_received);

#ifdef __cplusplus
}
#endif
