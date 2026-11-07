#include "camera.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "ptp.h"
#include "ptp_transport.h"

static const char *TAG = "camera";

#define MAX_OPS   64
#define MAX_PROPS 64
#define DEVINFO_BUF_SIZE   768
#define OBJINFO_BUF_SIZE    512
#define STORAGEINFO_BUF_SIZE 384
#define PROPDESC_BUF_SIZE   256
#define MAX_CACHED_OBJECTS 4096

typedef struct {
    char manufacturer[40];
    char model[40];
    char version[32];
    char serial[40];
    uint16_t ops_supported[MAX_OPS];
    int n_ops;
    uint16_t props_supported[MAX_PROPS];
    int n_props;
} device_info_t;

static SemaphoreHandle_t s_mutex;
static cam_state_t s_state = CAM_STATE_DISCONNECTED;
static char s_last_error[128];
static device_info_t s_info;
static bool s_capture_supported;
static bool s_has_event_endpoint;

static uint64_t s_storage_free_bytes, s_storage_total_bytes;
static bool s_storage_info_valid;
static uint32_t s_storage_id = 0xFFFFFFFFu;

static uint32_t *s_object_handles;
static size_t s_object_count;

static volatile bool s_need_handshake;
static volatile bool s_need_teardown;

/* --------------------------------- small helpers --------------------------------- */

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
    bool overflow;
} mem_sink_ctx_t;

static size_t mem_sink(const uint8_t *chunk, size_t len, void *ctx)
{
    mem_sink_ctx_t *c = (mem_sink_ctx_t *)ctx;
    size_t space = (c->len < c->cap) ? (c->cap - c->len) : 0;
    size_t take = (len < space) ? len : space;
    if (take > 0) {
        memcpy(c->buf + c->len, chunk, take);
        c->len += take;
    }
    if (take < len) {
        c->overflow = true;
    }
    return take;
}

static esp_err_t do_transact_to_mem(uint16_t opcode, const uint32_t *params, int nparams,
                                    uint8_t *buf, size_t cap, size_t *out_len, uint16_t *out_resp)
{
    mem_sink_ctx_t sink = { .buf = buf, .cap = cap, .len = 0, .overflow = false };
    ptp_transaction_req_t req = {
        .opcode = opcode,
        .nparams = nparams,
        .data_in_cb = mem_sink,
        .data_in_ctx = &sink,
        .timeout_ms = 8000,
    };
    if (nparams > 0) {
        memcpy(req.params, params, (size_t)nparams * sizeof(uint32_t));
    }
    ptp_transaction_resp_t resp;
    esp_err_t err = ptp_transaction(&req, &resp);
    if (err != ESP_OK) {
        return err;
    }
    if (sink.overflow) {
        ESP_LOGW(TAG, "opcode 0x%04x response truncated (buffer too small)", opcode);
    }
    *out_len = sink.len;
    *out_resp = resp.resp_code;
    return ESP_OK;
}

static bool read_u16_array(const uint8_t *buf, size_t len, size_t *off, uint16_t *out, int max, int *out_n)
{
    if (*off + 4 > len) {
        return false;
    }
    uint32_t n = ptp_rd32(buf + *off);
    *off += 4;
    int got = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (*off + 2 > len) {
            return false;
        }
        uint16_t v = ptp_rd16(buf + *off);
        *off += 2;
        if (got < max) {
            out[got++] = v;
        }
    }
    if (out_n) {
        *out_n = got;
    }
    return true;
}

static void safe_strcpy(char *dst, size_t cap, const char *src)
{
    if (cap == 0) {
        return;
    }
    snprintf(dst, cap, "%s", src ? src : "");
}

static bool array_contains(const uint16_t *arr, int n, uint16_t v)
{
    for (int i = 0; i < n; i++) {
        if (arr[i] == v) {
            return true;
        }
    }
    return false;
}

/* --------------------------------- state plumbing --------------------------------- */

static void reset_cached_info(void)
{
    memset(&s_info, 0, sizeof(s_info));
    s_capture_supported = false;
    s_has_event_endpoint = false;
    s_storage_info_valid = false;
    s_storage_free_bytes = s_storage_total_bytes = 0;
    s_storage_id = 0xFFFFFFFFu;
    free(s_object_handles);
    s_object_handles = NULL;
    s_object_count = 0;
    s_last_error[0] = '\0';
}

static void link_event_cb(ptp_link_state_t st, void *ctx)
{
    (void)ctx;
    switch (st) {
    case PTP_LINK_CONNECTED:
        s_need_handshake = true;
        break;
    case PTP_LINK_NOT_PTP:
        reset_cached_info();
        s_state = CAM_STATE_NOT_PTP;
        break;
    case PTP_LINK_DISCONNECTED:
        s_need_teardown = true;
        break;
    }
}

/* ------------------------------- device handshake ---------------------------------- */

static esp_err_t ptp_open_session(void)
{
    uint32_t params[1] = { 1 }; /* SessionID = 1 */
    ptp_transaction_req_t req = { .opcode = PTP_OC_OpenSession, .nparams = 1, .timeout_ms = 8000 };
    memcpy(req.params, params, sizeof(params));
    ptp_transaction_resp_t resp;
    esp_err_t err = ptp_transaction(&req, &resp);
    if (err != ESP_OK) {
        return err;
    }
    if (resp.resp_code != PTP_RC_OK && resp.resp_code != PTP_RC_SessionAlreadyOpen) {
        ESP_LOGE(TAG, "OpenSession rejected: 0x%04x", resp.resp_code);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool parse_device_info(const uint8_t *buf, size_t len)
{
    size_t off = 0;
    if (off + 2 > len) {
        return false;
    }
    off += 2; /* StandardVersion */
    if (off + 4 > len) {
        return false;
    }
    off += 4; /* VendorExtensionID */
    if (off + 2 > len) {
        return false;
    }
    off += 2; /* VendorExtensionVersion */
    if (!ptp_skip_string(buf, len, &off)) {
        return false; /* VendorExtensionDesc */
    }
    if (off + 2 > len) {
        return false;
    }
    off += 2; /* FunctionalMode */

    if (!read_u16_array(buf, len, &off, s_info.ops_supported, MAX_OPS, &s_info.n_ops)) return false;
    uint16_t scratch[MAX_OPS];
    int scratch_n;
    if (!read_u16_array(buf, len, &off, scratch, MAX_OPS, &scratch_n)) return false; /* EventsSupported */
    if (!read_u16_array(buf, len, &off, s_info.props_supported, MAX_PROPS, &s_info.n_props)) return false;
    if (!read_u16_array(buf, len, &off, scratch, MAX_OPS, &scratch_n)) return false; /* CaptureFormats */
    if (!read_u16_array(buf, len, &off, scratch, MAX_OPS, &scratch_n)) return false; /* ImageFormats */

    if (!ptp_decode_string(buf, len, &off, s_info.manufacturer, sizeof(s_info.manufacturer))) return false;
    if (!ptp_decode_string(buf, len, &off, s_info.model, sizeof(s_info.model))) return false;
    if (!ptp_decode_string(buf, len, &off, s_info.version, sizeof(s_info.version))) return false;
    if (!ptp_decode_string(buf, len, &off, s_info.serial, sizeof(s_info.serial))) return false;
    return true;
}

static uint8_t s_scratch_buf[DEVINFO_BUF_SIZE];

static esp_err_t fetch_device_info(void)
{
    size_t len = 0;
    uint16_t resp_code = 0;
    esp_err_t err = do_transact_to_mem(PTP_OC_GetDeviceInfo, NULL, 0, s_scratch_buf, sizeof(s_scratch_buf),
                                        &len, &resp_code);
    if (err != ESP_OK) {
        return err;
    }
    if (resp_code != PTP_RC_OK) {
        ESP_LOGE(TAG, "GetDeviceInfo rejected: 0x%04x", resp_code);
        return ESP_FAIL;
    }
    if (!parse_device_info(s_scratch_buf, len)) {
        ESP_LOGE(TAG, "GetDeviceInfo payload could not be parsed (%u bytes)", (unsigned)len);
        return ESP_ERR_INVALID_RESPONSE;
    }
    s_capture_supported = array_contains(s_info.ops_supported, s_info.n_ops, PTP_OC_InitiateCapture);
    s_has_event_endpoint = ptp_transport_has_event_endpoint();
    ESP_LOGI(TAG, "camera: %s %s (serial %s), capture %ssupported",
             s_info.manufacturer, s_info.model, s_info.serial, s_capture_supported ? "" : "NOT ");
    return ESP_OK;
}

static esp_err_t fetch_storage_info(void)
{
    size_t len = 0;
    uint16_t resp_code = 0;
    esp_err_t err = do_transact_to_mem(PTP_OC_GetStorageIDs, NULL, 0, s_scratch_buf, sizeof(s_scratch_buf),
                                        &len, &resp_code);
    if (err != ESP_OK || resp_code != PTP_RC_OK || len < 4) {
        return ESP_FAIL;
    }
    size_t off = 0;
    uint32_t n = ptp_rd32(s_scratch_buf + off);
    off += 4;
    if (n == 0 || off + 4 > len) {
        return ESP_FAIL;
    }
    uint32_t storage_id = ptp_rd32(s_scratch_buf + off);

    uint32_t params[1] = { storage_id };
    err = do_transact_to_mem(PTP_OC_GetStorageInfo, params, 1, s_scratch_buf, sizeof(s_scratch_buf), &len, &resp_code);
    if (err != ESP_OK || resp_code != PTP_RC_OK || len < 18) {
        return ESP_FAIL;
    }
    off = 0;
    off += 2; /* StorageType */
    off += 2; /* FilesystemType */
    off += 2; /* AccessCapability */
    uint64_t max_capacity = (uint64_t)ptp_rd32(s_scratch_buf + off) | ((uint64_t)ptp_rd32(s_scratch_buf + off + 4) << 32);
    off += 8;
    uint64_t free_bytes = (uint64_t)ptp_rd32(s_scratch_buf + off) | ((uint64_t)ptp_rd32(s_scratch_buf + off + 4) << 32);
    off += 8;

    s_storage_id = storage_id;
    s_storage_total_bytes = max_capacity;
    s_storage_free_bytes = free_bytes;
    s_storage_info_valid = true;
    return ESP_OK;
}

static void do_handshake(void)
{
    s_state = CAM_STATE_BUSY;
    esp_err_t err = ptp_open_session();
    if (err != ESP_OK) {
        snprintf(s_last_error, sizeof(s_last_error), "OpenSession failed (%s)", esp_err_to_name(err));
        s_state = CAM_STATE_ERROR;
        return;
    }
    err = fetch_device_info();
    if (err != ESP_OK) {
        snprintf(s_last_error, sizeof(s_last_error), "GetDeviceInfo failed (%s)", esp_err_to_name(err));
        s_state = CAM_STATE_ERROR;
        return;
    }
    /* Storage info is best-effort: some cameras in PictBridge mode restrict
     * which operations are allowed before a print job is started. */
    if (fetch_storage_info() != ESP_OK) {
        ESP_LOGW(TAG, "GetStorageIDs/GetStorageInfo failed (non-fatal)");
    }
    s_state = CAM_STATE_READY;
}

/* ------------------------------------- task ----------------------------------------- */

static void camera_task(void *arg)
{
    (void)arg;
    esp_err_t err = ptp_transport_init(link_event_cb, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ptp_transport_init failed: %s", esp_err_to_name(err));
    }
    for (;;) {
        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            ptp_transport_poll(50);
            if (s_need_handshake) {
                s_need_handshake = false;
                do_handshake();
            }
            if (s_need_teardown) {
                s_need_teardown = false;
                reset_cached_info();
                s_state = CAM_STATE_DISCONNECTED;
            }
            xSemaphoreGive(s_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

esp_err_t camera_manager_start(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        return ESP_ERR_NO_MEM;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(camera_task, "camera_mgr", 6144, NULL, 5, NULL, 1);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void camera_get_status(cam_status_t *out)
{
    memset(out, 0, sizeof(*out));
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    out->state = s_state;
    safe_strcpy(out->manufacturer, sizeof(out->manufacturer), s_info.manufacturer);
    safe_strcpy(out->model, sizeof(out->model), s_info.model);
    safe_strcpy(out->version, sizeof(out->version), s_info.version);
    safe_strcpy(out->serial, sizeof(out->serial), s_info.serial);
    out->capture_supported = s_capture_supported;
    out->has_event_endpoint = s_has_event_endpoint;
    out->storage_free_bytes = s_storage_free_bytes;
    out->storage_total_bytes = s_storage_total_bytes;
    out->storage_info_valid = s_storage_info_valid;
    safe_strcpy(out->last_error, sizeof(out->last_error), s_last_error);
    xSemaphoreGive(s_mutex);
}

/* ------------------------------------ objects ---------------------------------------- */

/* Callers always hold s_mutex for the full duration of a public camera_*
 * call, so CAM_STATE_BUSY (set by this same call) is never actually
 * observable by another caller -- only CAM_STATE_READY lets a new
 * operation start. */
static bool require_ready_locked(void)
{
    return s_state == CAM_STATE_READY;
}

esp_err_t camera_refresh_objects(void)
{
    esp_err_t ret = ESP_OK;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!require_ready_locked()) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    size_t cap = MAX_CACHED_OBJECTS * sizeof(uint32_t);
    uint8_t *buf = heap_caps_malloc(cap, MALLOC_CAP_8BIT);
    if (!buf) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }
    uint32_t params[3] = { 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu }; /* all storages, any format, any parent */
    size_t len = 0;
    uint16_t resp_code = 0;
    esp_err_t err = do_transact_to_mem(PTP_OC_GetObjectHandles, params, 3, buf, cap, &len, &resp_code);
    if (err != ESP_OK || resp_code != PTP_RC_OK || len < 4) {
        ESP_LOGW(TAG, "GetObjectHandles failed (err=%s resp=0x%04x)", esp_err_to_name(err), resp_code);
        free(buf);
        xSemaphoreGive(s_mutex);
        return ESP_FAIL;
    }
    size_t off = 0;
    uint32_t n = ptp_rd32(buf + off);
    off += 4;
    if (n > MAX_CACHED_OBJECTS) {
        ESP_LOGW(TAG, "camera reports %u objects; showing the most recent %u only", (unsigned)n, MAX_CACHED_OBJECTS);
        n = MAX_CACHED_OBJECTS;
    }

    free(s_object_handles);
    s_object_handles = heap_caps_malloc((size_t)n * sizeof(uint32_t), MALLOC_CAP_8BIT);
    if (!s_object_handles && n > 0) {
        s_object_count = 0;
        free(buf);
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }
    size_t got = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (off + 4 > len) {
            break;
        }
        /* Store newest-first: PTP handle order is usually (but not spec-guaranteed)
         * allocation order, so reversing gives a reasonable "most recent first" gallery. */
        s_object_handles[n - 1 - i] = ptp_rd32(buf + off);
        off += 4;
        got++;
    }
    s_object_count = got;
    free(buf);
    xSemaphoreGive(s_mutex);
    return ret;
}

size_t camera_object_count(void)
{
    size_t n;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    n = s_object_count;
    xSemaphoreGive(s_mutex);
    return n;
}

static bool parse_object_info(const uint8_t *buf, size_t len, cam_object_t *out)
{
    size_t off = 0;
    if (off + 4 > len) {
        return false;
    }
    off += 4; /* StorageID */
    if (off + 2 > len) return false;
    uint16_t format = ptp_rd16(buf + off); off += 2;
    off += 2; /* ProtectionStatus */
    if (off + 4 > len) return false;
    uint32_t size = ptp_rd32(buf + off); off += 4;
    if (off + 2 > len) return false;
    uint16_t thumb_format = ptp_rd16(buf + off); off += 2;
    (void)thumb_format;
    if (off + 4 > len) return false;
    uint32_t thumb_size = ptp_rd32(buf + off); off += 4;
    off += 4; /* ThumbPixWidth */
    off += 4; /* ThumbPixHeight */
    off += 4; /* ImagePixWidth */
    off += 4; /* ImagePixHeight */
    off += 4; /* ImageBitDepth */
    off += 4; /* ParentObject */
    off += 2; /* AssociationType */
    off += 4; /* AssociationDesc */
    off += 4; /* SequenceNumber */
    if (off > len) return false;

    if (!ptp_decode_string(buf, len, &off, out->filename, sizeof(out->filename))) return false;
    if (!ptp_decode_string(buf, len, &off, out->capture_date, sizeof(out->capture_date))) return false;
    /* ModificationDate, Keywords: not needed, skip if present. */

    out->format = format;
    out->size = size;
    out->has_thumbnail = thumb_size > 0;
    return true;
}

esp_err_t camera_get_object_info(uint32_t handle, cam_object_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!require_ready_locked()) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t buf[OBJINFO_BUF_SIZE];
    uint32_t params[1] = { handle };
    size_t len = 0;
    uint16_t resp_code = 0;
    esp_err_t err = do_transact_to_mem(PTP_OC_GetObjectInfo, params, 1, buf, sizeof(buf), &len, &resp_code);
    if (err == ESP_OK && resp_code == PTP_RC_OK) {
        memset(out, 0, sizeof(*out));
        out->handle = handle;
        if (!parse_object_info(buf, len, out)) {
            err = ESP_ERR_INVALID_RESPONSE;
        }
    } else if (err == ESP_OK) {
        err = ESP_FAIL;
    }
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t camera_list_objects(size_t offset, size_t limit, cam_object_t *out, size_t *out_n)
{
    size_t n = 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!require_ready_locked()) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    size_t total = s_object_count;
    for (size_t i = offset; i < total && n < limit; i++, n++) {
        uint32_t handle = s_object_handles[i];
        uint8_t buf[OBJINFO_BUF_SIZE];
        uint32_t params[1] = { handle };
        size_t len = 0;
        uint16_t resp_code = 0;
        esp_err_t err = do_transact_to_mem(PTP_OC_GetObjectInfo, params, 1, buf, sizeof(buf), &len, &resp_code);
        memset(&out[n], 0, sizeof(out[n]));
        out[n].handle = handle;
        if (err == ESP_OK && resp_code == PTP_RC_OK && parse_object_info(buf, len, &out[n])) {
            /* ok */
        } else {
            snprintf(out[n].filename, sizeof(out[n].filename), "(object %u)", (unsigned)handle);
        }
    }
    xSemaphoreGive(s_mutex);
    *out_n = n;
    return ESP_OK;
}

/* camera_stream_cb_t (void-returning) and ptp_data_in_sink_t (size_t-returning)
 * are intentionally different signatures (the public API doesn't need a
 * return value), so bridge them with a tiny adapter. */
typedef struct {
    camera_stream_cb_t cb;
    void *ctx;
} stream_adapter_t;

static size_t stream_adapter_sink(const uint8_t *chunk, size_t len, void *ctx)
{
    stream_adapter_t *a = (stream_adapter_t *)ctx;
    a->cb(chunk, len, a->ctx);
    return len;
}

esp_err_t camera_stream_object(uint32_t handle, camera_stream_cb_t cb, void *ctx, uint32_t *out_total_size)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!require_ready_locked()) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    s_state = CAM_STATE_BUSY;
    stream_adapter_t adapter = { .cb = cb, .ctx = ctx };
    ptp_transaction_req_t req = {
        .opcode = PTP_OC_GetObject,
        .nparams = 1,
        .params = { handle },
        .data_in_cb = stream_adapter_sink,
        .data_in_ctx = &adapter,
        .timeout_ms = 20000,
    };
    ptp_transaction_resp_t resp;
    esp_err_t err = ptp_transaction(&req, &resp);
    if (err == ESP_OK && resp.resp_code != PTP_RC_OK) {
        err = ESP_FAIL;
    }
    if (out_total_size) {
        *out_total_size = (uint32_t)resp.data_in_total;
    }
    s_state = CAM_STATE_READY;
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t camera_stream_thumbnail(uint32_t handle, camera_stream_cb_t cb, void *ctx, uint32_t *out_total_size)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!require_ready_locked()) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    s_state = CAM_STATE_BUSY;
    stream_adapter_t adapter = { .cb = cb, .ctx = ctx };
    ptp_transaction_req_t req = {
        .opcode = PTP_OC_GetThumb,
        .nparams = 1,
        .params = { handle },
        .data_in_cb = stream_adapter_sink,
        .data_in_ctx = &adapter,
        .timeout_ms = 10000,
    };
    ptp_transaction_resp_t resp;
    esp_err_t err = ptp_transaction(&req, &resp);
    if (err == ESP_OK && resp.resp_code != PTP_RC_OK) {
        err = ESP_FAIL;
    }
    if (out_total_size) {
        *out_total_size = (uint32_t)resp.data_in_total;
    }
    s_state = CAM_STATE_READY;
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t camera_delete_object(uint32_t handle)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!require_ready_locked()) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    ptp_transaction_req_t req = {
        .opcode = PTP_OC_DeleteObject,
        .nparams = 2,
        .params = { handle, 0 },
        .timeout_ms = 8000,
    };
    ptp_transaction_resp_t resp;
    esp_err_t err = ptp_transaction(&req, &resp);
    if (err == ESP_OK && resp.resp_code != PTP_RC_OK) {
        ESP_LOGW(TAG, "DeleteObject(%u) rejected: 0x%04x", (unsigned)handle, resp.resp_code);
        err = ESP_FAIL;
    }
    xSemaphoreGive(s_mutex);
    return err;
}

/* -------------------------------------- capture --------------------------------------- */

esp_err_t camera_capture(uint32_t *out_new_handle, uint32_t timeout_ms)
{
    if (out_new_handle) {
        *out_new_handle = 0;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_state != CAM_STATE_READY) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_capture_supported) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_state = CAM_STATE_BUSY;

    /* Snapshot the handle list before capture, so we can diff afterwards if
     * no ObjectAdded/CaptureComplete event shows up (not guaranteed on the
     * K200D -- see README). */
    size_t prev_count = s_object_count;

    ptp_transaction_req_t req = {
        .opcode = PTP_OC_InitiateCapture,
        .nparams = 2,
        .params = { 0xFFFFFFFFu, 0x00000000u }, /* any storage, device-selected format */
        .timeout_ms = 8000,
    };
    ptp_transaction_resp_t resp;
    esp_err_t err = ptp_transaction(&req, &resp);
    if (err != ESP_OK || resp.resp_code != PTP_RC_OK) {
        s_state = CAM_STATE_READY;
        xSemaphoreGive(s_mutex);
        return (err == ESP_OK) ? ESP_FAIL : err;
    }

    uint32_t new_handle = 0;
    ptp_event_t evt;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline) {
        uint32_t remaining_ms = (uint32_t)((deadline - xTaskGetTickCount()) * portTICK_PERIOD_MS);
        if (!ptp_transport_wait_event(&evt, remaining_ms > 500 ? 500 : remaining_ms)) {
            if (xTaskGetTickCount() >= deadline) {
                break;
            }
            continue;
        }
        if (evt.event_code == PTP_EC_ObjectAdded && evt.nparams >= 1) {
            new_handle = evt.params[0];
            break;
        }
        if (evt.event_code == PTP_EC_CaptureComplete) {
            break; /* fall through to the handle-list diff below */
        }
    }

    if (new_handle == 0) {
        /* Fallback: refresh the object list and look for a handle we hadn't
         * seen before. This covers cameras that don't emit ObjectAdded. */
        uint32_t params[3] = { 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu };
        size_t cap = MAX_CACHED_OBJECTS * sizeof(uint32_t);
        uint8_t *buf = heap_caps_malloc(cap, MALLOC_CAP_8BIT);
        if (buf) {
            size_t len = 0;
            uint16_t resp_code = 0;
            if (do_transact_to_mem(PTP_OC_GetObjectHandles, params, 3, buf, cap, &len, &resp_code) == ESP_OK &&
                resp_code == PTP_RC_OK && len >= 4) {
                size_t off = 0;
                uint32_t n = ptp_rd32(buf + off);
                off += 4;
                if (n > MAX_CACHED_OBJECTS) {
                    n = MAX_CACHED_OBJECTS;
                }
                uint32_t *fresh = heap_caps_malloc((size_t)n * sizeof(uint32_t), MALLOC_CAP_8BIT);
                size_t got = 0;
                if (fresh) {
                    for (uint32_t i = 0; i < n && off + 4 <= len; i++) {
                        fresh[n - 1 - i] = ptp_rd32(buf + off);
                        off += 4;
                        got++;
                    }
                    if (got > prev_count && got > 0) {
                        new_handle = fresh[0]; /* newest-first */
                    }
                    free(s_object_handles);
                    s_object_handles = fresh;
                    s_object_count = got;
                }
            }
            free(buf);
        }
    }

    s_state = CAM_STATE_READY;
    xSemaphoreGive(s_mutex);
    if (out_new_handle) {
        *out_new_handle = new_handle;
    }
    return ESP_OK;
}

/* ------------------------------------ properties --------------------------------------- */

static size_t ptp_datatype_size(uint16_t dt)
{
    switch (dt) {
    case PTP_DTC_INT8: case PTP_DTC_UINT8: return 1;
    case PTP_DTC_INT16: case PTP_DTC_UINT16: return 2;
    case PTP_DTC_INT32: case PTP_DTC_UINT32: return 4;
    case PTP_DTC_INT64: case PTP_DTC_UINT64: return 8;
    default: return 0;
    }
}

static bool read_scalar(const uint8_t *buf, size_t len, size_t *off, uint16_t dt, int64_t *out)
{
    size_t sz = ptp_datatype_size(dt);
    if (sz == 0 || *off + sz > len) {
        return false;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < sz; i++) {
        v |= ((uint64_t)buf[*off + i]) << (8 * i);
    }
    switch (dt) {
    case PTP_DTC_INT8:  *out = (int64_t)(int8_t)v; break;
    case PTP_DTC_INT16: *out = (int64_t)(int16_t)v; break;
    case PTP_DTC_INT32: *out = (int64_t)(int32_t)v; break;
    case PTP_DTC_INT64: *out = (int64_t)v; break;
    default: *out = (int64_t)v; break;
    }
    *off += sz;
    return true;
}

static bool write_scalar(uint8_t *buf, size_t cap, uint16_t dt, int64_t value, size_t *out_len)
{
    size_t sz = ptp_datatype_size(dt);
    if (sz == 0 || sz > cap) {
        return false;
    }
    uint64_t v = (uint64_t)value;
    for (size_t i = 0; i < sz; i++) {
        buf[i] = (uint8_t)((v >> (8 * i)) & 0xFF);
    }
    *out_len = sz;
    return true;
}

static bool parse_prop_desc(const uint8_t *buf, size_t len, cam_prop_t *out)
{
    size_t off = 0;
    if (off + 2 > len) return false;
    out->code = ptp_rd16(buf + off); off += 2;
    if (off + 2 > len) return false;
    out->data_type = ptp_rd16(buf + off); off += 2;
    if (off + 1 > len) return false;
    uint8_t get_set = buf[off]; off += 1;
    out->read_only = (get_set == 0);
    out->is_string = (out->data_type == PTP_DTC_STR);

    if (out->is_string) {
        char scratch[48];
        if (!ptp_skip_string(buf, len, &off)) return false; /* DefaultValue */
        if (!ptp_decode_string(buf, len, &off, out->current_str, sizeof(out->current_str))) return false;
        (void)scratch;
    } else {
        int64_t tmp;
        if (!read_scalar(buf, len, &off, out->data_type, &tmp)) return false; /* DefaultValue */
        if (!read_scalar(buf, len, &off, out->data_type, &out->current)) return false;
    }

    if (off + 1 > len) return false;
    uint8_t form = buf[off]; off += 1;
    out->has_range = false;
    out->enum_count = 0;
    if (!out->is_string && form == 1) { /* Range */
        int64_t lo, hi, step;
        if (!read_scalar(buf, len, &off, out->data_type, &lo)) return false;
        if (!read_scalar(buf, len, &off, out->data_type, &hi)) return false;
        if (!read_scalar(buf, len, &off, out->data_type, &step)) return false;
        out->has_range = true;
        out->range_min = lo;
        out->range_max = hi;
        out->range_step = step;
    } else if (!out->is_string && form == 2) { /* Enumeration */
        if (off + 2 > len) return false;
        uint16_t n = ptp_rd16(buf + off); off += 2;
        for (uint16_t i = 0; i < n; i++) {
            int64_t v;
            if (!read_scalar(buf, len, &off, out->data_type, &v)) return false;
            if (out->enum_count < CAM_PROP_MAX_ENUM) {
                out->enum_values[out->enum_count++] = v;
            }
        }
    }
    return true;
}

esp_err_t camera_list_props(cam_prop_t *out, size_t max, size_t *out_n)
{
    size_t n = 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!require_ready_locked()) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    for (int i = 0; i < s_info.n_props && n < max; i++) {
        uint16_t code = s_info.props_supported[i];
        uint8_t buf[PROPDESC_BUF_SIZE];
        uint32_t params[1] = { code };
        size_t len = 0;
        uint16_t resp_code = 0;
        esp_err_t err = do_transact_to_mem(PTP_OC_GetDevicePropDesc, params, 1, buf, sizeof(buf), &len, &resp_code);
        if (err != ESP_OK || resp_code != PTP_RC_OK) {
            continue;
        }
        memset(&out[n], 0, sizeof(out[n]));
        if (parse_prop_desc(buf, len, &out[n])) {
            n++;
        }
    }
    xSemaphoreGive(s_mutex);
    *out_n = n;
    return ESP_OK;
}

esp_err_t camera_set_prop(uint16_t code, int64_t value)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!require_ready_locked()) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    /* Need the property's data type to encode the value correctly. */
    uint8_t descbuf[PROPDESC_BUF_SIZE];
    uint32_t params[1] = { code };
    size_t len = 0;
    uint16_t resp_code = 0;
    esp_err_t err = do_transact_to_mem(PTP_OC_GetDevicePropDesc, params, 1, descbuf, sizeof(descbuf), &len, &resp_code);
    if (err != ESP_OK || resp_code != PTP_RC_OK) {
        xSemaphoreGive(s_mutex);
        return ESP_FAIL;
    }
    cam_prop_t desc;
    memset(&desc, 0, sizeof(desc));
    if (!parse_prop_desc(descbuf, len, &desc) || desc.is_string) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint8_t valbuf[8];
    size_t vlen = 0;
    if (!write_scalar(valbuf, sizeof(valbuf), desc.data_type, value, &vlen)) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_ARG;
    }

    ptp_transaction_req_t req = {
        .opcode = PTP_OC_SetDevicePropValue,
        .nparams = 1,
        .params = { code },
        .data_out = valbuf,
        .data_out_len = vlen,
        .timeout_ms = 8000,
    };
    ptp_transaction_resp_t resp;
    err = ptp_transaction(&req, &resp);
    if (err == ESP_OK && resp.resp_code != PTP_RC_OK) {
        ESP_LOGW(TAG, "SetDevicePropValue(0x%04x) rejected: 0x%04x", code, resp.resp_code);
        err = ESP_FAIL;
    }
    xSemaphoreGive(s_mutex);
    return err;
}
