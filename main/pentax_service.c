#include "pentax_service.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_log.h"

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

/* Pentax command groups/numbers (see docs/REFERENCES.md). */
#define G_SYS 0x00
#define C_SET_CONNECT_MODE 0x00
#define C_GET_STATUS       0x01
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
    return usb_msc_link_init(link_cb, NULL);
}
