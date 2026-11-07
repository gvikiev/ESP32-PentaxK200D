/*
 * High level camera manager: owns the PTP session with the attached
 * camera and exposes a small, blocking API that the web server calls
 * directly from its request handlers.
 *
 * Every public function here is safe to call from any task (they all
 * serialize on an internal mutex), but esp_http_server's default
 * configuration only ever runs one request handler at a time anyway.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CAM_STATE_DISCONNECTED = 0, /* no camera plugged in */
    CAM_STATE_NOT_PTP,          /* a USB device is attached but isn't a PTP Still Image device
                                    (K200D: check [USB Connection] = [PictBridge] in the camera menu) */
    CAM_STATE_READY,            /* PTP session open, camera ready for commands */
    CAM_STATE_BUSY,             /* a command (e.g. capture) is in flight */
    CAM_STATE_ERROR,            /* camera was ready but the last command failed unexpectedly */
} cam_state_t;

typedef struct {
    cam_state_t state;
    char manufacturer[40];
    char model[40];
    char version[32];
    char serial[40];
    bool capture_supported;      /* InitiateCapture present in OperationsSupported */
    bool has_event_endpoint;     /* interrupt IN endpoint available for async events */
    uint64_t storage_free_bytes;
    uint64_t storage_total_bytes;
    bool storage_info_valid;
    char last_error[128];
} cam_status_t;

/* Starts the camera manager task. Call once from app_main(). */
esp_err_t camera_manager_start(void);

void camera_get_status(cam_status_t *out);

typedef struct {
    uint32_t handle;
    uint32_t format;        /* PTP object format code, e.g. PTP_OFC_EXIF_JPEG */
    uint32_t size;           /* compressed object size, bytes */
    bool has_thumbnail;
    char filename[64];
    char capture_date[24];   /* raw PTP date-time string, e.g. 20240102T153045 */
} cam_object_t;

/* Re-fetches the object handle list from the camera's storage (one round
 * trip). Must be called before camera_list_objects()/camera_object_count().
 * Objects are cached newest-first (by descending handle value). */
esp_err_t camera_refresh_objects(void);
size_t camera_object_count(void);

/* Fills `out[0..*out_n)` with info for up to `limit` cached objects starting
 * at `offset` (0 = newest). Performs one GetObjectInfo round trip per item. */
esp_err_t camera_list_objects(size_t offset, size_t limit, cam_object_t *out, size_t *out_n);

esp_err_t camera_get_object_info(uint32_t handle, cam_object_t *out);

typedef void (*camera_stream_cb_t)(const uint8_t *chunk, size_t len, void *ctx);

/* Streams the full object (GetObject) or its thumbnail (GetThumb) to `cb`.
 * *out_total_size is set to the declared size before the first chunk. */
esp_err_t camera_stream_object(uint32_t handle, camera_stream_cb_t cb, void *ctx, uint32_t *out_total_size);
esp_err_t camera_stream_thumbnail(uint32_t handle, camera_stream_cb_t cb, void *ctx, uint32_t *out_total_size);

esp_err_t camera_delete_object(uint32_t handle);

/* Triggers InitiateCapture and waits (up to timeout_ms) for the resulting
 * object. Falls back to diffing the object handle list if the camera does
 * not report an ObjectAdded/CaptureComplete event (not all Pentax firmware
 * does). *out_new_handle is 0 if a new object could not be identified even
 * though the capture command itself was accepted. */
esp_err_t camera_capture(uint32_t *out_new_handle, uint32_t timeout_ms);

#define CAM_PROP_MAX_ENUM 24

typedef struct {
    uint16_t code;
    uint16_t data_type;
    bool read_only;
    bool is_string;
    int64_t current;
    char current_str[48];
    bool has_range;
    int64_t range_min, range_max, range_step;
    int enum_count;
    int64_t enum_values[CAM_PROP_MAX_ENUM];
} cam_prop_t;

/* Enumerates the camera's DevicePropertiesSupported list (from GetDeviceInfo)
 * and fetches a GetDevicePropDesc for each, up to `max` entries. Properties
 * the camera reports as unsupported/busy mid-enumeration are skipped. */
esp_err_t camera_list_props(cam_prop_t *out, size_t max, size_t *out_n);
esp_err_t camera_set_prop(uint16_t code, int64_t value);

#ifdef __cplusplus
}
#endif
