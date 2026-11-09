#include "pentax_service.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "nvs.h"

#include "usb_msc_link.h"
#include "bot.h"
#include "pentax_scsi.h"

#ifndef CONFIG_PENTAXHUB_STREAM_BLOCK_SIZE
#define CONFIG_PENTAXHUB_STREAM_BLOCK_SIZE 16384
#endif
#ifndef CONFIG_PENTAXHUB_STREAM_BLOCKS
#define CONFIG_PENTAXHUB_STREAM_BLOCKS 8
#endif
#ifndef CONFIG_PENTAXHUB_PENTAX_BUF_TYPE
#define CONFIG_PENTAXHUB_PENTAX_BUF_TYPE 0
#endif
#ifndef CONFIG_PENTAXHUB_PENTAX_BUF_RES
#define CONFIG_PENTAXHUB_PENTAX_BUF_RES 0
#endif

/* Pentax command groups/numbers (see docs/REFERENCES.md and
 * docs/pentax_k200d_architecture_gemeni.md section 2.3). */
#define G_SYS 0x00
#define C_SET_CONNECT_MODE 0x00
#define C_GET_STATUS       0x01
#define C_GET_FULL_STATUS  0x08
#define C_DSP_WU_REQ       0x09
#define G_ACT 0x10
#define C_SHUTTER          0x05
#define G_BUF 0x02
#define C_BUF_STATUS       0x00
#define C_BUF_SELECT       0x01
#define C_BUF_DELETE       0x03
#define G_SEG 0x04
#define C_SEG_INFO         0x00
#define C_SEG_NEXT         0x01
#define G_DL  0x06
#define C_DL_PREPARE       0x00

/* Parameter group: every command here must be framed with
 * DSPTaskWUReq(1) before and DSPTaskWUReq(2) after (see write_param_locked). */
#define G_PARAM 0x18
#define C_EXPOSURE_MODE    0x01
#define C_AE_METERING      0x03
#define C_FLASH_MODE       0x04
#define C_AF_MODE          0x05
#define C_AF_AREA          0x06
#define C_AF_POINT         0x07
#define C_WHITE_BALANCE    0x10
#define C_WB_TUNING        0x11
#define C_FILE_FORMAT      0x12
#define C_JPEG_QUALITY     0x13
#define C_JPEG_RESOLUTION  0x14
#define C_ISO              0x15
#define C_SHUTTER_SPEED    0x16
#define C_APERTURE         0x17
#define C_EV_COMP          0x18
#define C_FLASH_EV_COMP    0x1A
#define C_CUSTOM_IMAGE     0x1B
#define C_DRIVE_MODE       0x1C
#define C_RAW_FORMAT       0x1F

#define SEG_LAST           2
#define SEG_DATA           3
#define SEG_FILE_LEN       4
#define MAX_SEGMENTS       256
#define DL_CHUNK           8192u

static const char *TAG = "pentax_service";

static SemaphoreHandle_t s_mutex;
static EventGroupHandle_t s_events;
#define EV_LINK_CHANGED BIT0

static uint8_t s_bot_work[512 * 4];
static bot_t s_bot;
static pentax_scsi_t s_scsi;
static bool s_session_open;

static pentax_status_summary_t s_status;
static pentax_progress_cb_t s_cb;
static void *s_cb_ctx;

static k200d_status_t s_full_status;
static bool s_full_status_valid;

static pentax_writeonly_cache_t s_wo_cache;

/* ------------------------- write-only parameter cache -------------------------- *
 * WB, WB tuning, AE metering, flash mode and flash EV comp have no field in
 * the [00 08] status buffer (see docs/pentax_k200d_architecture_gemeni.md
 * section 2.2), so the only record of "what did we last tell the camera" is
 * what the ESP32 itself remembers. Persisted in NVS so a reboot doesn't lose
 * it (the camera's own setting survives; only our UI's memory of it would
 * otherwise be forgotten). */
#define WO_NVS_NAMESPACE "pentax_wo"

static void wo_cache_load(void)
{
    memset(&s_wo_cache, 0, sizeof(s_wo_cache));
    nvs_handle_t h;
    if (nvs_open(WO_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return; /* namespace not created yet: nothing has ever been written */
    }
    uint32_t u32v;
    int32_t i32v;
    if (nvs_get_u32(h, "wb_mode", &u32v) == ESP_OK) {
        s_wo_cache.wb_mode = u32v;
        s_wo_cache.wb_set = true;
    }
    if (nvs_get_u32(h, "wbt_mode", &u32v) == ESP_OK && nvs_get_i32(h, "wbt_mg", &i32v) == ESP_OK) {
        s_wo_cache.wb_tune_mode = u32v;
        s_wo_cache.wb_mg = i32v;
        if (nvs_get_i32(h, "wbt_ba", &i32v) == ESP_OK) {
            s_wo_cache.wb_ba = i32v;
            s_wo_cache.wb_tune_set = true;
        }
    }
    if (nvs_get_u32(h, "ae_meter", &u32v) == ESP_OK) {
        s_wo_cache.ae_metering_mode = u32v;
        s_wo_cache.ae_metering_set = true;
    }
    if (nvs_get_u32(h, "flash_mode", &u32v) == ESP_OK) {
        s_wo_cache.flash_mode = u32v;
        s_wo_cache.flash_mode_set = true;
    }
    if (nvs_get_i32(h, "flash_ev_n", &i32v) == ESP_OK) {
        s_wo_cache.flash_ev_comp_nom = i32v;
        if (nvs_get_i32(h, "flash_ev_d", &i32v) == ESP_OK) {
            s_wo_cache.flash_ev_comp_denom = i32v;
            s_wo_cache.flash_ev_comp_set = true;
        }
    }
    nvs_close(h);
}

static void wo_cache_put_u32(const char *key, uint32_t value)
{
    nvs_handle_t h;
    if (nvs_open(WO_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u32(h, key, value);
    nvs_commit(h);
    nvs_close(h);
}

static void wo_cache_put_i32(const char *key, int32_t value)
{
    nvs_handle_t h;
    if (nvs_open(WO_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_i32(h, key, value);
    nvs_commit(h);
    nvs_close(h);
}

void pentax_service_get_writeonly_cache(pentax_writeonly_cache_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_wo_cache;
    xSemaphoreGive(s_mutex);
}

/* ---------------------------------- status ----------------------------------- */

static void notify(void)
{
    if (s_cb) {
        pentax_status_summary_t copy = s_status;
        s_cb(&copy, s_cb_ctx);
    }
}

static void set_error(const char *what, esp_err_t err)
{
    snprintf(s_status.last_error, sizeof(s_status.last_error), "%s: %s", what, esp_err_to_name(err));
    ESP_LOGW(TAG, "%s", s_status.last_error);
}

void pentax_service_get_status(pentax_status_summary_t *out)
{
    *out = s_status;
}

static void link_cb(usb_msc_state_t st, void *ctx)
{
    (void)ctx;
    s_session_open = false;
    s_full_status_valid = false;
    switch (st) {
    case USB_MSC_CONNECTED:    s_status.state = PS_STATE_READY; break;
    case USB_MSC_NOT_MSC:      s_status.state = PS_STATE_WRONG_USB_MODE; break;
    default:                   s_status.state = PS_STATE_DISCONNECTED; break;
    }
    xEventGroupSetBits(s_events, EV_LINK_CHANGED);
    notify();
}

/* ---------------------------------- helpers ---------------------------------- */

static uint32_t rd32(const uint8_t *p)
{
    if (s_scsi.big_endian_args) {
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    }
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Call with s_mutex held. */
static esp_err_t ensure_session(void)
{
    if (s_session_open) {
        return ESP_OK;
    }
    bot_io_t io;
    esp_err_t err = usb_msc_link_get_io(&io);
    if (err != ESP_OK) {
        return err;
    }
    err = bot_init(&s_bot, &io, s_bot_work, sizeof(s_bot_work), 0);
    if (err != ESP_OK) {
        return err;
    }
    /* K200D is a pre-K-3 body: big-endian arguments/results. */
    pentax_scsi_init(&s_scsi, &s_bot, true);

    uint32_t on = 1;
    err = pentax_scsi_exec(&s_scsi, G_SYS, C_SET_CONNECT_MODE, &on, 1, NULL, 0, NULL, NULL, NULL);
    if (err != ESP_OK) {
        set_error("connect mode", err);
        return err;
    }
    uint8_t status[28];
    size_t n = 0;
    err = pentax_scsi_exec(&s_scsi, G_SYS, C_GET_STATUS, NULL, 0, status, sizeof(status), &n, NULL, NULL);
    if (err != ESP_OK) {
        set_error("get status", err);
        return err;
    }
    ESP_LOGI(TAG, "camera session open (status %u bytes)", (unsigned)n);
    s_session_open = true;
    return ESP_OK;
}

static esp_err_t read_buffer_mask(uint32_t *mask)
{
    uint8_t res[8];
    size_t n = 0;
    esp_err_t err = pentax_scsi_exec(&s_scsi, G_BUF, C_BUF_STATUS, NULL, 0, res, sizeof(res), &n, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }
    if (n < 4) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *mask = rd32(res);
    return ESP_OK;
}

/* Call with s_mutex held. */
static esp_err_t fetch_full_status_locked(void)
{
    uint8_t buf[PENTAX_STATUS_LEN];
    size_t n = 0;
    esp_err_t err = pentax_scsi_exec(&s_scsi, G_SYS, C_GET_FULL_STATUS, NULL, 0, buf, sizeof(buf), &n, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }
    if (!pentax_parse_k200d_status(buf, n, &s_full_status)) {
        ESP_LOGW(TAG, "full status response too short (%u bytes)", (unsigned)n);
        return ESP_ERR_INVALID_RESPONSE;
    }
    s_full_status_valid = true;
    return ESP_OK;
}

esp_err_t pentax_service_get_full_status(k200d_status_t *out)
{
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(300)) == pdTRUE) {
        esp_err_t err = ensure_session();
        if (err == ESP_OK) {
            err = fetch_full_status_locked();
            if (err != ESP_OK) {
                set_error("full status", err);
                s_session_open = false; /* force re-handshake next time */
            }
        }
        xSemaphoreGive(s_mutex);
    }
    *out = s_full_status;
    return s_full_status_valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

/* ------------------------------- parameter writes ------------------------------ *
 * Every [18 xx] write is framed with DSPTaskWUReq ([00 09], flag 1 before,
 * flag 2 after) and, where the written field also appears in the [00 08]
 * status buffer, followed by a best-effort read-back so the cache the HTTP
 * layer serves is current. A mismatch between what we sent and what the
 * camera reports back is logged, not treated as a failure: the write
 * command's own Pentax status byte (via pentax_status_to_err()) is already
 * the authoritative success/failure signal, and some fields may simply be
 * encoded/rounded differently than we assume (see docs/pentax_k200d_architecture_gemeni.md
 * section 2.2 for the fields flagged there as unconfirmed). */

/* Call with s_mutex held. */
static esp_err_t write_param_locked(uint8_t cmd, const uint32_t *args, size_t nargs)
{
    esp_err_t err = ensure_session();
    if (err != ESP_OK) {
        return err;
    }
    uint32_t pre = 1;
    err = pentax_scsi_exec(&s_scsi, G_SYS, C_DSP_WU_REQ, &pre, 1, NULL, 0, NULL, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = pentax_scsi_exec(&s_scsi, G_PARAM, cmd, args, nargs, NULL, 0, NULL, NULL, NULL);
    uint32_t post = 2;
    esp_err_t post_err = pentax_scsi_exec(&s_scsi, G_SYS, C_DSP_WU_REQ, &post, 1, NULL, 0, NULL, NULL, NULL);
    return (err != ESP_OK) ? err : post_err;
}

/* Takes s_mutex, writes the parameter and (on success) refreshes the full
 * status cache, then releases s_mutex. `what` is used for error/log text. */
static esp_err_t set_param_and_verify(const char *what, uint8_t cmd, const uint32_t *args, size_t nargs)
{
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = write_param_locked(cmd, args, nargs);
    if (err != ESP_OK) {
        set_error(what, err);
        s_session_open = false; /* force re-handshake after a failure */
    } else if (fetch_full_status_locked() != ESP_OK) {
        ESP_LOGW(TAG, "%s: write accepted but post-write status refresh failed", what);
    }
    xSemaphoreGive(s_mutex);
    return err;
}

/* Takes s_mutex, writes the parameter and (on success, while still holding
 * s_mutex) calls update_cache(ctx) -- used by the write-only parameters
 * below to update their NVS-backed cache, since there is no [00 08] field
 * to read back and refresh from. Always balances the lock itself, even if
 * it timed out before acquiring it (the caller never has to). */
typedef void (*wo_cache_update_fn)(void *ctx);

static esp_err_t write_param_cached(const char *what, uint8_t cmd, const uint32_t *args, size_t nargs,
                                     wo_cache_update_fn update_cache, void *ctx)
{
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = write_param_locked(cmd, args, nargs);
    if (err != ESP_OK) {
        set_error(what, err);
        s_session_open = false;
    } else if (update_cache) {
        update_cache(ctx);
    }
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t pentax_service_set_shutter_speed(uint32_t nom, uint32_t denom)
{
    if (nom == 0 || denom == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    double seconds = (double)nom / (double)denom;
    if (seconds < (1.0 / 4000.0) - 1e-9 || seconds > 30.0 + 1e-9) {
        return ESP_ERR_INVALID_ARG; /* K200D range: 1/4000s .. 30s */
    }
    uint32_t args[2] = { nom, denom };
    esp_err_t err = set_param_and_verify("shutter_speed", C_SHUTTER_SPEED, args, 2);
    if (err == ESP_OK && (s_full_status.set_shutter_nom != nom || s_full_status.set_shutter_denom != denom)) {
        ESP_LOGW(TAG, "shutter_speed: wrote %u/%u, camera reports %u/%u", (unsigned)nom, (unsigned)denom,
                 (unsigned)s_full_status.set_shutter_nom, (unsigned)s_full_status.set_shutter_denom);
    }
    return err;
}

esp_err_t pentax_service_set_aperture(uint32_t nom, uint32_t denom)
{
    if (denom == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    double f = (double)nom / (double)denom;
    /* Validate against the attached lens's own range when known; otherwise
     * fall back to the K200D body's widest documented range (F1.0-F32). */
    double f_min = 1.0, f_max = 32.0;
    if (s_full_status_valid && s_full_status.lens_max_aperture_denom != 0 && s_full_status.lens_min_aperture_denom != 0) {
        f_min = (double)s_full_status.lens_max_aperture_nom / s_full_status.lens_max_aperture_denom;
        f_max = (double)s_full_status.lens_min_aperture_nom / s_full_status.lens_min_aperture_denom;
    }
    if (f < f_min - 0.05 || f > f_max + 0.05) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t args[3] = { nom, denom, 0 };
    esp_err_t err = set_param_and_verify("aperture", C_APERTURE, args, 3);
    if (err == ESP_OK && (s_full_status.set_aperture_nom != nom || s_full_status.set_aperture_denom != denom)) {
        ESP_LOGW(TAG, "aperture: wrote %u/%u, camera reports %u/%u", (unsigned)nom, (unsigned)denom,
                 (unsigned)s_full_status.set_aperture_nom, (unsigned)s_full_status.set_aperture_denom);
    }
    return err;
}

esp_err_t pentax_service_set_iso(uint32_t iso, uint32_t auto_min, uint32_t auto_max)
{
    if (iso < 100 || iso > 1600) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((auto_min != 0 && (auto_min < 100 || auto_min > 1600)) ||
        (auto_max != 0 && (auto_max < 100 || auto_max > 1600)) ||
        (auto_min != 0 && auto_max != 0 && auto_min > auto_max)) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t args[3] = { iso, auto_min, auto_max };
    esp_err_t err = set_param_and_verify("iso", C_ISO, args, 3);
    if (err == ESP_OK && s_full_status.iso != iso) {
        ESP_LOGW(TAG, "iso: wrote %u, camera reports %u", (unsigned)iso, (unsigned)s_full_status.iso);
    }
    return err;
}

esp_err_t pentax_service_set_ev_comp(int32_t nom, int32_t denom)
{
    if (denom == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    double ev = (double)nom / (double)denom;
    if (ev < -3.0 - 1e-9 || ev > 3.0 + 1e-9) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t args[2] = { (uint32_t)nom, (uint32_t)denom };
    esp_err_t err = set_param_and_verify("ev_comp", C_EV_COMP, args, 2);
    if (err == ESP_OK && (s_full_status.ev_comp_nom != nom || s_full_status.ev_comp_denom != denom)) {
        ESP_LOGW(TAG, "ev_comp: wrote %d/%d, camera reports %d/%d", (int)nom, (int)denom,
                 (int)s_full_status.ev_comp_nom, (int)s_full_status.ev_comp_denom);
    }
    return err;
}

esp_err_t pentax_service_set_exposure_mode(uint32_t mode)
{
    /* Encoding not confirmed on real hardware (see architecture doc section
     * 2.2); passed through as given, with a leading 1 per the command table. */
    uint32_t args[2] = { 1, mode };
    esp_err_t err = set_param_and_verify("exposure_mode", C_EXPOSURE_MODE, args, 2);
    if (err == ESP_OK && s_full_status.exposure_mode != mode) {
        ESP_LOGW(TAG, "exposure_mode: wrote %u, camera reports %u", (unsigned)mode, (unsigned)s_full_status.exposure_mode);
    }
    return err;
}

esp_err_t pentax_service_set_file_format(uint32_t format)
{
    if (format > 2) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t args[2] = { 1, format };
    esp_err_t err = set_param_and_verify("file_format", C_FILE_FORMAT, args, 2);
    if (err == ESP_OK && s_full_status.image_format != format) {
        ESP_LOGW(TAG, "file_format: wrote %u, camera reports %u", (unsigned)format, (unsigned)s_full_status.image_format);
    }
    return err;
}

esp_err_t pentax_service_set_jpeg_quality(uint32_t stars)
{
    if (stars < 1 || stars > 3) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t hw = 3u - stars;
    uint32_t args[2] = { 1, hw };
    esp_err_t err = set_param_and_verify("jpeg_quality", C_JPEG_QUALITY, args, 2);
    if (err == ESP_OK && s_full_status.jpeg_stars != stars) {
        ESP_LOGW(TAG, "jpeg_quality: wrote %u stars, camera reports %u", (unsigned)stars, (unsigned)s_full_status.jpeg_stars);
    }
    return err;
}

esp_err_t pentax_service_set_jpeg_resolution(uint32_t res_idx)
{
    if (res_idx > 2) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t args[2] = { 1, res_idx };
    esp_err_t err = set_param_and_verify("jpeg_resolution", C_JPEG_RESOLUTION, args, 2);
    if (err == ESP_OK && s_full_status.jpeg_res_idx != res_idx) {
        ESP_LOGW(TAG, "jpeg_resolution: wrote %u, camera reports %u", (unsigned)res_idx, (unsigned)s_full_status.jpeg_res_idx);
    }
    return err;
}

esp_err_t pentax_service_set_raw_format(uint32_t format)
{
    if (format > 1) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t args[2] = { 1, format };
    esp_err_t err = set_param_and_verify("raw_format", C_RAW_FORMAT, args, 2);
    if (err == ESP_OK && s_full_status.raw_format != format) {
        ESP_LOGW(TAG, "raw_format: wrote %u, camera reports %u", (unsigned)format, (unsigned)s_full_status.raw_format);
    }
    return err;
}

esp_err_t pentax_service_set_custom_image_tone(uint32_t tone)
{
    if (tone > 5) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t args[1] = { tone };
    esp_err_t err = set_param_and_verify("custom_image_tone", C_CUSTOM_IMAGE, args, 1);
    if (err == ESP_OK && s_full_status.custom_image_tone != tone) {
        ESP_LOGW(TAG, "custom_image_tone: wrote %u, camera reports %u", (unsigned)tone, (unsigned)s_full_status.custom_image_tone);
    }
    return err;
}

esp_err_t pentax_service_set_af_mode(uint32_t mode)
{
    if (mode != 1 && mode != 2) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t args[1] = { mode };
    esp_err_t err = set_param_and_verify("af_mode", C_AF_MODE, args, 1);
    if (err == ESP_OK && s_full_status.af_mode != mode) {
        ESP_LOGW(TAG, "af_mode: wrote %u, camera reports %u", (unsigned)mode, (unsigned)s_full_status.af_mode);
    }
    return err;
}

esp_err_t pentax_service_set_af_area(uint32_t sel)
{
    if (sel > 3) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t args[1] = { sel };
    esp_err_t err = set_param_and_verify("af_area", C_AF_AREA, args, 1);
    if (err == ESP_OK && s_full_status.af_area_select != sel) {
        ESP_LOGW(TAG, "af_area: wrote %u, camera reports %u", (unsigned)sel, (unsigned)s_full_status.af_area_select);
    }
    return err;
}

esp_err_t pentax_service_set_af_point(uint32_t point_mask)
{
    if (point_mask > 0x7FFu) {
        return ESP_ERR_INVALID_ARG; /* 11 AF points */
    }
    uint32_t args[1] = { point_mask };
    esp_err_t err = set_param_and_verify("af_point", C_AF_POINT, args, 1);
    if (err == ESP_OK && s_full_status.af_point_mask != point_mask) {
        ESP_LOGW(TAG, "af_point: wrote 0x%03x, camera reports 0x%03x", (unsigned)point_mask, (unsigned)s_full_status.af_point_mask);
    }
    return err;
}

esp_err_t pentax_service_set_drive_mode(uint32_t mode)
{
    /* No upper bound: the architecture doc's enum for this field is
     * explicitly partial ("0: Single, 1: Cont.Hi, 2: Self-Timer..."). */
    uint32_t args[1] = { mode };
    esp_err_t err = set_param_and_verify("drive_mode", C_DRIVE_MODE, args, 1);
    if (err == ESP_OK && s_full_status.drive_mode != mode) {
        ESP_LOGW(TAG, "drive_mode: wrote %u, camera reports %u", (unsigned)mode, (unsigned)s_full_status.drive_mode);
    }
    return err;
}

static void wo_update_wb(void *ctx)
{
    uint32_t mode = *(uint32_t *)ctx;
    s_wo_cache.wb_mode = mode;
    s_wo_cache.wb_set = true;
    wo_cache_put_u32("wb_mode", mode);
}

esp_err_t pentax_service_set_white_balance(uint32_t mode)
{
    uint32_t args[1] = { mode };
    return write_param_cached("white_balance", C_WHITE_BALANCE, args, 1, wo_update_wb, &mode);
}

typedef struct { uint32_t mode; int32_t mg, ba; } wb_tuning_ctx_t;

static void wo_update_wb_tuning(void *ctx)
{
    wb_tuning_ctx_t *c = ctx;
    s_wo_cache.wb_tune_mode = c->mode;
    s_wo_cache.wb_mg = c->mg;
    s_wo_cache.wb_ba = c->ba;
    s_wo_cache.wb_tune_set = true;
    wo_cache_put_u32("wbt_mode", c->mode);
    wo_cache_put_i32("wbt_mg", c->mg);
    wo_cache_put_i32("wbt_ba", c->ba);
}

esp_err_t pentax_service_set_wb_tuning(uint32_t mode, int32_t mg, int32_t ba)
{
    uint32_t args[3] = { mode, (uint32_t)mg, (uint32_t)ba };
    wb_tuning_ctx_t ctx = { mode, mg, ba };
    return write_param_cached("wb_tuning", C_WB_TUNING, args, 3, wo_update_wb_tuning, &ctx);
}

static void wo_update_ae_metering(void *ctx)
{
    uint32_t mode = *(uint32_t *)ctx;
    s_wo_cache.ae_metering_mode = mode;
    s_wo_cache.ae_metering_set = true;
    wo_cache_put_u32("ae_meter", mode);
}

esp_err_t pentax_service_set_ae_metering(uint32_t mode)
{
    if (mode > 2) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t args[1] = { mode };
    return write_param_cached("ae_metering", C_AE_METERING, args, 1, wo_update_ae_metering, &mode);
}

static void wo_update_flash_mode(void *ctx)
{
    uint32_t mode = *(uint32_t *)ctx;
    s_wo_cache.flash_mode = mode;
    s_wo_cache.flash_mode_set = true;
    wo_cache_put_u32("flash_mode", mode);
}

esp_err_t pentax_service_set_flash_mode(uint32_t mode)
{
    uint32_t args[1] = { mode };
    return write_param_cached("flash_mode", C_FLASH_MODE, args, 1, wo_update_flash_mode, &mode);
}

typedef struct { int32_t nom, denom; } flash_ev_ctx_t;

static void wo_update_flash_ev(void *ctx)
{
    flash_ev_ctx_t *c = ctx;
    s_wo_cache.flash_ev_comp_nom = c->nom;
    s_wo_cache.flash_ev_comp_denom = c->denom;
    s_wo_cache.flash_ev_comp_set = true;
    wo_cache_put_i32("flash_ev_n", c->nom);
    wo_cache_put_i32("flash_ev_d", c->denom);
}

esp_err_t pentax_service_set_flash_ev_comp(int32_t nom, int32_t denom)
{
    if (denom == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t args[2] = { (uint32_t)nom, (uint32_t)denom };
    flash_ev_ctx_t ctx = { nom, denom };
    return write_param_cached("flash_ev_comp", C_FLASH_EV_COMP, args, 2, wo_update_flash_ev, &ctx);
}

/* -------------------------------- capture ------------------------------------ */

esp_err_t pentax_service_capture(uint32_t timeout_ms)
{
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = ensure_session();
    if (err != ESP_OK) {
        xSemaphoreGive(s_mutex);
        return err;
    }
    s_status.state = PS_STATE_BUSY;
    notify();

    uint32_t before = 0;
    err = read_buffer_mask(&before);
    if (err == ESP_OK) {
        uint32_t full_press = 2;
        err = pentax_scsi_exec(&s_scsi, G_ACT, C_SHUTTER, &full_press, 1, NULL, 0, NULL, NULL, NULL);
    }
    TickType_t start = xTaskGetTickCount();
    while (err == ESP_OK) {
        uint32_t mask = 0;
        err = read_buffer_mask(&mask);
        if (err == ESP_OK && (mask & ~before) != 0) {
            s_status.buffer_mask = mask;
            break;
        }
        if (err == ESP_OK && (xTaskGetTickCount() - start) > pdMS_TO_TICKS(timeout_ms)) {
            err = ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (err != ESP_OK) {
        set_error("capture", err);
        s_session_open = false; /* force re-handshake after a failure */
    }
    s_status.state = (usb_msc_link_state() == USB_MSC_CONNECTED) ? PS_STATE_READY : s_status.state;
    notify();
    xSemaphoreGive(s_mutex);
    return err;
}

/* ------------------------------- download ------------------------------------ */

typedef struct {
    photo_stream_t *stream;
    uint8_t *blk;
    int slot;
    size_t fill;
    uint32_t done;
    TickType_t last_notify;
    esp_err_t err;
} dl_ctx_t;

static bool dl_flush(dl_ctx_t *d)
{
    if (d->blk && d->fill > 0) {
        ps_submit(d->stream, d->slot, d->fill);
        d->blk = NULL;
        d->fill = 0;
    }
    return true;
}

static bool dl_sink(const uint8_t *chunk, size_t len, void *ctx)
{
    dl_ctx_t *d = ctx;
    const size_t bs = ps_block_size(d->stream);
    while (len > 0) {
        if (!d->blk) {
            d->blk = ps_acquire(d->stream, 15000, &d->slot);
            if (!d->blk) {
                d->err = ps_aborted(d->stream) ? ESP_ERR_INVALID_STATE : ESP_ERR_TIMEOUT;
                return false; /* consumer gone or stalled */
            }
            d->fill = 0;
        }
        size_t take = bs - d->fill;
        if (take > len) {
            take = len;
        }
        memcpy(d->blk + d->fill, chunk, take);
        d->fill += take;
        chunk += take;
        len -= take;
        d->done += (uint32_t)take;
        if (d->fill == bs) {
            dl_flush(d);
        }
    }
    if ((xTaskGetTickCount() - d->last_notify) > pdMS_TO_TICKS(250)) {
        d->last_notify = xTaskGetTickCount();
        s_status.bytes_done = d->done;
        notify();
    }
    return true;
}

static esp_err_t download_range(dl_ctx_t *d, uint32_t addr, uint32_t length)
{
    while (length > 0) {
        uint32_t n = length > DL_CHUNK ? DL_CHUNK : length;
        uint32_t args[2] = { addr, n };
        esp_err_t err = pentax_scsi_exec(&s_scsi, G_DL, C_DL_PREPARE, args, 2, NULL, 0, NULL, dl_sink, d);
        if (err != ESP_OK) {
            return d->err != ESP_OK ? d->err : err;
        }
        addr += n;
        length -= n;
    }
    return ESP_OK;
}

static esp_err_t download_current_buffer(dl_ctx_t *d)
{
    esp_err_t err = ensure_session();
    if (err != ESP_OK) {
        return err;
    }
    uint32_t mask = 0;
    err = read_buffer_mask(&mask);
    if (err != ESP_OK) {
        return err;
    }
    if ((mask & 1u) == 0) {
        return ESP_ERR_NOT_FOUND; /* buffer 0 empty: nothing to download */
    }
    /* Older bodies (K200D) take only three select-buffer arguments. */
    uint32_t sel[3] = { 0, CONFIG_PENTAXHUB_PENTAX_BUF_TYPE, CONFIG_PENTAXHUB_PENTAX_BUF_RES };
    err = pentax_scsi_exec(&s_scsi, G_BUF, C_BUF_SELECT, sel, 3, NULL, 0, NULL, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }

    for (int i = 0; i < MAX_SEGMENTS; i++) {
        uint8_t info[16];
        size_t n = 0;
        err = pentax_scsi_exec(&s_scsi, G_SEG, C_SEG_INFO, NULL, 0, info, sizeof(info), &n, NULL, NULL);
        if (err != ESP_OK) {
            return err;
        }
        if (n < 16) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        uint32_t type = rd32(info + 4), addr = rd32(info + 8), length = rd32(info + 12);
        ESP_LOGD(TAG, "segment %d: type=%u addr=0x%x len=%u", i, (unsigned)type, (unsigned)addr, (unsigned)length);
        if (type == SEG_FILE_LEN) {
            s_status.bytes_total = length;
        } else if (type == SEG_DATA) {
            err = download_range(d, addr, length);
            if (err != ESP_OK) {
                return err;
            }
        }
        if (type == SEG_LAST) {
            return ESP_OK;
        }
        uint32_t zero = 0;
        err = pentax_scsi_exec(&s_scsi, G_SEG, C_SEG_NEXT, &zero, 1, NULL, 0, NULL, NULL, NULL);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_ERR_INVALID_RESPONSE; /* never saw a "last segment" marker */
}

static void download_task(void *arg)
{
    photo_stream_t *stream = (photo_stream_t *)arg;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.state = PS_STATE_BUSY;
    s_status.bytes_done = 0;
    s_status.bytes_total = 0;
    notify();

    dl_ctx_t d = { .stream = stream, .err = ESP_OK, .last_notify = xTaskGetTickCount() };
    esp_err_t err = download_current_buffer(&d);
    if (err == ESP_OK) {
        dl_flush(&d);
    } else {
        set_error("download", err);
        s_session_open = false;
    }
    s_status.bytes_done = d.done;
    s_status.state = (usb_msc_link_state() == USB_MSC_CONNECTED) ? PS_STATE_READY : s_status.state;
    notify();
    xSemaphoreGive(s_mutex);

    ps_finish(stream, err);
    vTaskDelete(NULL);
}

esp_err_t pentax_service_download_start(photo_stream_t **out)
{
    if (usb_msc_link_state() != USB_MSC_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }
    photo_stream_t *s = ps_create(CONFIG_PENTAXHUB_STREAM_BLOCK_SIZE, CONFIG_PENTAXHUB_STREAM_BLOCKS);
    if (!s) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(download_task, "pentax_dl", 6144, s, 5, NULL) != pdPASS) {
        ps_destroy(s);
        return ESP_ERR_NO_MEM;
    }
    *out = s;
    return ESP_OK;
}

void pentax_service_download_end(photo_stream_t *s)
{
    if (!s) {
        return;
    }
    ps_abort(s);
    /* The producer always ends with an EOF item; drain until we see it so it
     * is guaranteed to be done with the stream before we free it. */
    ps_block_t blk;
    for (;;) {
        esp_err_t e = ps_next(s, 20000, &blk);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "producer did not finish; leaking stream to stay safe");
            return;
        }
        bool eof = blk.eof;
        ps_release(s, &blk);
        if (eof) {
            break;
        }
    }
    ps_destroy(s);
}

esp_err_t pentax_service_delete_photo(void)
{
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = ensure_session();
    if (err == ESP_OK) {
        uint32_t buf0 = 0;
        err = pentax_scsi_exec(&s_scsi, G_BUF, C_BUF_DELETE, &buf0, 1, NULL, 0, NULL, NULL, NULL);
    }
    if (err != ESP_OK) {
        set_error("delete", err);
    }
    xSemaphoreGive(s_mutex);
    return err;
}

/* ---------------------------------- start ------------------------------------ */

esp_err_t pentax_service_start(pentax_progress_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_cb_ctx = ctx;
    s_mutex = xSemaphoreCreateMutex();
    s_events = xEventGroupCreate();
    if (!s_mutex || !s_events) {
        return ESP_ERR_NO_MEM;
    }
    s_status.state = PS_STATE_DISCONNECTED;
    wo_cache_load();
    return usb_msc_link_init(link_cb, NULL);
}
