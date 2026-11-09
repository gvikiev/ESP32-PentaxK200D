#include "web_server.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"

#include "camera.h"
#include "ptp.h"

static const char *TAG = "web_server";

/* Embedded static UI assets (see main/CMakeLists.txt EMBED_FILES). */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");
extern const uint8_t app_js_start[] asm("_binary_app_js_start");
extern const uint8_t app_js_end[]   asm("_binary_app_js_end");
extern const uint8_t style_css_start[] asm("_binary_style_css_start");
extern const uint8_t style_css_end[]   asm("_binary_style_css_end");

#define OBJECTS_PAGE_MAX  60
#define PROPS_LIST_MAX    32

/* ------------------------------------- helpers ---------------------------------------- */

static esp_err_t send_binary(httpd_req_t *req, const char *type, const uint8_t *start, const uint8_t *end)
{
    httpd_resp_set_type(req, type);
    return httpd_resp_send(req, (const char *)start, end - start);
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *s = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, s ? s : "{}");
    if (s) {
        cJSON_free(s);
    }
    cJSON_Delete(root);
    return err;
}

static void drain_request_body(httpd_req_t *req)
{
    char tmp[64];
    int remaining = (int)req->content_len;
    while (remaining > 0) {
        int want = (remaining < (int)sizeof(tmp)) ? remaining : (int)sizeof(tmp);
        int r = httpd_req_recv(req, tmp, want);
        if (r <= 0) {
            break;
        }
        remaining -= r;
    }
}

static esp_err_t read_body(httpd_req_t *req, char *buf, size_t cap)
{
    if (req->content_len <= 0 || (size_t)req->content_len >= cap) {
        return ESP_FAIL;
    }
    size_t total = 0;
    while (total < (size_t)req->content_len) {
        int r = httpd_req_recv(req, buf + total, req->content_len - total);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            return ESP_FAIL;
        }
        total += (size_t)r;
    }
    buf[total] = '\0';
    return ESP_OK;
}

static long query_long(httpd_req_t *req, const char *key, long def)
{
    char q[96];
    char v[24];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) {
        return def;
    }
    if (httpd_query_key_value(q, key, v, sizeof(v)) != ESP_OK) {
        return def;
    }
    return atol(v);
}

static bool query_flag(httpd_req_t *req, const char *key)
{
    return query_long(req, key, 0) != 0;
}

static const char *content_type_for_filename(const char *filename)
{
    const char *dot = strrchr(filename, '.');
    if (!dot) {
        return "application/octet-stream";
    }
    if (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg")) return "image/jpeg";
    if (!strcasecmp(dot, ".png")) return "image/png";
    if (!strcasecmp(dot, ".tif") || !strcasecmp(dot, ".tiff")) return "image/tiff";
    if (!strcasecmp(dot, ".dng")) return "image/x-adobe-dng";
    if (!strcasecmp(dot, ".avi")) return "video/x-msvideo";
    return "application/octet-stream";
}

static const char *prop_name(uint16_t code)
{
    switch (code) {
    case PTP_DPC_BatteryLevel: return "Battery level";
    case PTP_DPC_WhiteBalance: return "White balance";
    case PTP_DPC_FNumber: return "Aperture (F-number)";
    case PTP_DPC_FocalLength: return "Focal length";
    case PTP_DPC_FocusMode: return "Focus mode";
    case PTP_DPC_ExposureMeteringMode: return "Metering mode";
    case PTP_DPC_FlashMode: return "Flash mode";
    case PTP_DPC_ExposureTime: return "Shutter speed";
    case PTP_DPC_ExposureProgramMode: return "Exposure program";
    case PTP_DPC_ExposureIndex: return "ISO";
    case PTP_DPC_ExposureBiasCompensation: return "Exposure compensation";
    case PTP_DPC_StillCaptureMode: return "Capture mode";
    case PTP_DPC_Contrast: return "Contrast";
    case PTP_DPC_Sharpness: return "Sharpness";
    case PTP_DPC_ImageSize: return "Image size";
    case PTP_DPC_CompressionSetting: return "Compression";
    case PTP_DPC_DateTime: return "Date/time";
    default: return NULL;
    }
}

static const char *cam_state_str(cam_state_t s)
{
    switch (s) {
    case CAM_STATE_DISCONNECTED: return "disconnected";
    case CAM_STATE_NOT_PTP: return "not_ptp";
    case CAM_STATE_READY: return "ready";
    case CAM_STATE_BUSY: return "busy";
    case CAM_STATE_ERROR: return "error";
    default: return "unknown";
    }
}

/* ---------------------------------- static assets -------------------------------------- */

esp_err_t web_server_index_handler(httpd_req_t *req)
{
    return send_binary(req, "text/html", index_html_start, index_html_end);
}
static esp_err_t app_js_handler(httpd_req_t *req)
{
    return send_binary(req, "application/javascript", app_js_start, app_js_end);
}
static esp_err_t style_css_handler(httpd_req_t *req)
{
    return send_binary(req, "text/css", style_css_start, style_css_end);
}

/* -------------------------------------- /api/status ------------------------------------ */

static esp_err_t api_status_handler(httpd_req_t *req)
{
    cam_status_t st;
    camera_get_status(&st);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "state", cam_state_str(st.state));
    cJSON_AddStringToObject(root, "manufacturer", st.manufacturer);
    cJSON_AddStringToObject(root, "model", st.model);
    cJSON_AddStringToObject(root, "version", st.version);
    cJSON_AddStringToObject(root, "serial", st.serial);
    cJSON_AddBoolToObject(root, "capture_supported", st.capture_supported);
    cJSON_AddBoolToObject(root, "has_event_endpoint", st.has_event_endpoint);
    cJSON_AddBoolToObject(root, "storage_info_valid", st.storage_info_valid);
    cJSON_AddNumberToObject(root, "storage_free_bytes", (double)st.storage_free_bytes);
    cJSON_AddNumberToObject(root, "storage_total_bytes", (double)st.storage_total_bytes);
    cJSON_AddStringToObject(root, "last_error", st.last_error);
    return send_json(req, root);
}

/* ------------------------------------- /api/capture ------------------------------------- */

static esp_err_t api_capture_handler(httpd_req_t *req)
{
    drain_request_body(req);
    uint32_t handle = 0;
    esp_err_t err = camera_capture(&handle, 15000);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    if (err == ESP_OK) {
        cJSON_AddNumberToObject(root, "handle", (double)handle);
        if (handle == 0) {
            cJSON_AddStringToObject(root, "warning",
                "capture command accepted, but the camera did not report a new image handle");
        }
    } else {
        cJSON_AddStringToObject(root, "error", esp_err_to_name(err));
    }
    return send_json(req, root);
}

/* ------------------------------------- /api/objects ------------------------------------- */

static esp_err_t api_objects_handler(httpd_req_t *req)
{
    if (query_flag(req, "refresh")) {
        esp_err_t err = camera_refresh_objects();
        if (err != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "camera not ready");
            return ESP_OK;
        }
    }

    long offset = query_long(req, "offset", 0);
    long limit = query_long(req, "limit", 40);
    if (offset < 0) offset = 0;
    if (limit <= 0) limit = 40;
    if (limit > OBJECTS_PAGE_MAX) limit = OBJECTS_PAGE_MAX;

    cam_object_t *items = calloc((size_t)limit, sizeof(cam_object_t));
    if (!items) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }
    size_t n = 0;
    esp_err_t err = camera_list_objects((size_t)offset, (size_t)limit, items, &n);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    cJSON_AddNumberToObject(root, "total", (double)camera_object_count());
    cJSON *arr = cJSON_AddArrayToObject(root, "items");
    for (size_t i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "handle", (double)items[i].handle);
        cJSON_AddStringToObject(o, "filename", items[i].filename);
        cJSON_AddNumberToObject(o, "size", (double)items[i].size);
        cJSON_AddNumberToObject(o, "format", items[i].format);
        cJSON_AddStringToObject(o, "capture_date", items[i].capture_date);
        cJSON_AddBoolToObject(o, "has_thumbnail", items[i].has_thumbnail);
        cJSON_AddItemToArray(arr, o);
    }
    free(items);
    return send_json(req, root);
}

/* -------------------------------------- /api/object -------------------------------------- */

static void http_stream_sink(const uint8_t *chunk, size_t len, void *ctx)
{
    httpd_req_t *req = (httpd_req_t *)ctx;
    httpd_resp_send_chunk(req, (const char *)chunk, len);
}

static esp_err_t api_object_get_handler(httpd_req_t *req)
{
    long handle = query_long(req, "handle", -1);
    if (handle <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ?handle=");
        return ESP_OK;
    }
    bool thumb = false;
    {
        char q[96], v[16];
        if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
            httpd_query_key_value(q, "kind", v, sizeof(v)) == ESP_OK) {
            thumb = (strcmp(v, "thumb") == 0);
        }
    }

    cam_object_t info;
    if (camera_get_object_info((uint32_t)handle, &info) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "object not found / camera not ready");
        return ESP_OK;
    }

    if (thumb) {
        httpd_resp_set_type(req, "image/jpeg");
    } else {
        httpd_resp_set_type(req, content_type_for_filename(info.filename));
        char hdr[96];
        snprintf(hdr, sizeof(hdr), "inline; filename=\"%s\"", info.filename);
        httpd_resp_set_hdr(req, "Content-Disposition", hdr);
    }

    uint32_t total = 0;
    esp_err_t err = thumb
        ? camera_stream_thumbnail((uint32_t)handle, http_stream_sink, req, &total)
        : camera_stream_object((uint32_t)handle, http_stream_sink, req, &total);
    httpd_resp_send_chunk(req, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "stream object %ld (thumb=%d) failed: %s", handle, thumb, esp_err_to_name(err));
    }
    return ESP_OK;
}

static esp_err_t api_object_delete_handler(httpd_req_t *req)
{
    long handle = query_long(req, "handle", -1);
    if (handle <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ?handle=");
        return ESP_OK;
    }
    esp_err_t err = camera_delete_object((uint32_t)handle);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    if (err != ESP_OK) {
        cJSON_AddStringToObject(root, "error", esp_err_to_name(err));
    }
    return send_json(req, root);
}

/* --------------------------------------- /api/props --------------------------------------- */

static esp_err_t api_props_get_handler(httpd_req_t *req)
{
    cam_prop_t *props = calloc(PROPS_LIST_MAX, sizeof(cam_prop_t));
    if (!props) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }
    size_t n = 0;
    esp_err_t err = camera_list_props(props, PROPS_LIST_MAX, &n);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    cJSON *arr = cJSON_AddArrayToObject(root, "props");
    for (size_t i = 0; i < n; i++) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddNumberToObject(p, "code", props[i].code);
        char namebuf[24];
        const char *nm = prop_name(props[i].code);
        if (!nm) {
            snprintf(namebuf, sizeof(namebuf), "Property 0x%04X", props[i].code);
            nm = namebuf;
        }
        cJSON_AddStringToObject(p, "name", nm);
        cJSON_AddBoolToObject(p, "read_only", props[i].read_only);
        cJSON_AddBoolToObject(p, "is_string", props[i].is_string);
        if (props[i].is_string) {
            cJSON_AddStringToObject(p, "value", props[i].current_str);
        } else {
            cJSON_AddNumberToObject(p, "value", (double)props[i].current);
            if (props[i].has_range) {
                cJSON_AddNumberToObject(p, "min", (double)props[i].range_min);
                cJSON_AddNumberToObject(p, "max", (double)props[i].range_max);
                cJSON_AddNumberToObject(p, "step", (double)props[i].range_step);
            }
            if (props[i].enum_count > 0) {
                cJSON *ev = cJSON_AddArrayToObject(p, "enum");
                for (int j = 0; j < props[i].enum_count; j++) {
                    cJSON_AddItemToArray(ev, cJSON_CreateNumber((double)props[i].enum_values[j]));
                }
            }
        }
        cJSON_AddItemToArray(arr, p);
    }
    free(props);
    return send_json(req, root);
}

static esp_err_t api_props_set_handler(httpd_req_t *req)
{
    char body[160];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large or missing");
        return ESP_OK;
    }
    cJSON *in = cJSON_Parse(body);
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (in) {
        cJSON *jcode = cJSON_GetObjectItem(in, "code");
        cJSON *jvalue = cJSON_GetObjectItem(in, "value");
        if (cJSON_IsNumber(jcode) && cJSON_IsNumber(jvalue)) {
            err = camera_set_prop((uint16_t)jcode->valueint, (int64_t)jvalue->valuedouble);
        }
        cJSON_Delete(in);
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    if (err != ESP_OK) {
        cJSON_AddStringToObject(root, "error", esp_err_to_name(err));
    }
    return send_json(req, root);
}

/* ------------------------------------------ startup ----------------------------------------- */

esp_err_t web_server_register(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        { .uri = "/app.js",       .method = HTTP_GET,    .handler = app_js_handler },
        { .uri = "/style.css",    .method = HTTP_GET,    .handler = style_css_handler },
        { .uri = "/api/status",   .method = HTTP_GET,    .handler = api_status_handler },
        { .uri = "/api/capture",  .method = HTTP_POST,   .handler = api_capture_handler },
        { .uri = "/api/objects",  .method = HTTP_GET,    .handler = api_objects_handler },
        { .uri = "/api/object",   .method = HTTP_GET,    .handler = api_object_get_handler },
        { .uri = "/api/object",   .method = HTTP_DELETE, .handler = api_object_delete_handler },
        { .uri = "/api/props",    .method = HTTP_GET,    .handler = api_props_get_handler },
        { .uri = "/api/props",    .method = HTTP_POST,   .handler = api_props_set_handler },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &routes[i]));
    }

    ESP_LOGI(TAG, "PTP web server routes registered");
    return ESP_OK;
}
