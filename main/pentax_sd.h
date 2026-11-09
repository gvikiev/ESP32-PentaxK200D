/*
 * Read-only FatFs access to the K200D's SD card over standard SCSI
 * READ(10), multiplexed with the Pentax vendor channel through
 * pentax_service_scsi_io() (same bot_t instance, same mutex) so the camera
 * never sees interleaved vendor/standard commands -- see
 * docs/pentax_k200d_architecture_gemeni.md section 3. Unverified on real
 * hardware (architecture doc hypothesis H-1): a card whose sector size
 * falls outside FF_MIN_SS..FF_MAX_SS, or that otherwise doesn't respond to
 * plain SCSI while the Pentax vendor interface is also in use, will simply
 * fail to mount (pentax_sd_ensure_mounted() returns an error) rather than
 * taking down the rest of the firmware.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "usb_msc_link.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Allocates the module's mutex. Call once from pentax_service_start();
 * registers no diskio driver and mounts nothing yet. */
esp_err_t pentax_sd_init(void);

/* Called by pentax_service.c's USB link callback. USB_MSC_CONNECTED marks
 * the card as mountable (the actual mount -- SCSI I/O -- happens lazily in
 * pentax_sd_ensure_mounted(), not here); any other state unmounts
 * immediately if currently mounted. */
void pentax_sd_on_link_change(usb_msc_state_t state);

/* Mounts the SD card (READ CAPACITY(10) + diskio registration + f_mount) if
 * not already mounted. Cheap to call before every SD operation once
 * mounted. ESP_ERR_INVALID_STATE if no camera is attached,
 * ESP_ERR_NOT_SUPPORTED if the card's reported sector size doesn't fit the
 * compiled-in FatFs sector size range. */
esp_err_t pentax_sd_ensure_mounted(void);

bool pentax_sd_is_mounted(void);

/* Base path the card is mounted at once pentax_sd_ensure_mounted()
 * succeeds (e.g. "/sdcard"), for building full paths with snprintf(). */
const char *pentax_sd_base_path(void);

#ifdef __cplusplus
}
#endif
