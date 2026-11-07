#include "pentax_http.h"

#include <stdio.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "photo_stream.h"

#ifndef CONFIG_PENTAXHUB_HTTP_PORT
#define CONFIG_PENTAXHUB_HTTP_PORT 80
#endif

#define MAX_WS_CLIENTS      4
#define CAPTURE_TIMEOUT_MS  15000
#define BLOCK_WAIT_MS       20000

static const char *TAG = "pentax_http";

static httpd_handle_t s_server;
static SemaphoreHandle_t s_ws_lock;
static int s_ws_fds[MAX_WS_CLIENTS];

static const char INDEX_HTML[] =
    "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>PentaxHub</title><body style='font-family:sans-serif;max-width:32em;margin:1em auto;padding:0 1em'>"
    "<h2>PentaxHub (Pentax SCSI)</h2><pre id=s>connecting...</pre>"
    "<progress id=p value=0 max=1 style='width:100%'></progress><p>"
    "<button onclick=\"post('POST')\">Take photo</button> "
    "<a href=/api/pentax/photo download><button>Download photo</button></a> "
    "<button onclick=\"post('DELETE')\">Delete from camera</button></p>"
    "<script>"
    "function post(m){fetch(m=='POST'?'/api/pentax/capture':'/api/pentax/photo',{method:m})"
    ".then(r=>r.text()).then(t=>document.getElementById('s').textContent=t)}"
    "fetch('/api/pentax/status').then(r=>r.text()).then(t=>document.getElementById('s').textContent=t);"
    "var w=new WebSocket('ws://'+location.host+'/ws');"
    "w.onmessage=e=>{var j=JSON.parse(e.data);document.getElementById('s').textContent=JSON.stringify(j,null,1);"
    "var p=document.getElementById('p');p.max=j.bytes_total||j.bytes_done||1;p.value=j.bytes_done}"
    "</script>";

/* --------------------------------- helpers ---------------------------------- */

static const char *state_str(pentax_state_t s)
{
    switch (s) {
    case PS_STATE_READY:          return "ready";
    case PS_STATE_BUSY:           return "busy";
    case PS_STATE_WRONG_USB_MODE: return "wrong_usb_mode";
    default:                      return "disconnected";
    }
}

static int status_json(char *buf, size_t cap, const pentax_status_summary_t *st)
{
    /* last_error is built only from fixed strings and esp_err_to_name(): no quotes to escape. */
    return snprintf(buf, cap,
                    "{\"state\":\"%s\",\"buffer_mask\":%u,\"bytes_done\":%u,\"bytes_total\":%u,\"last_error\":\"%s\"}",
                    state_str(st->state), (unsigned)st->buffer_mask, (unsigned)st->bytes_done,
                    (unsigned)st->bytes_total, st->last_error);
}

static esp_err_t send_status(httpd_req_t *req)
{
    pentax_status_summary_t st;
    pentax_service_get_status(&st);
    char buf[256];
    status_json(buf, sizeof(buf), &st);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

/* ------------------------------- WebSocket ----------------------------------- */

static void ws_add(int fd)
{
    xSemaphoreTake(s_ws_lock, portMAX_DELAY);
    bool present = false;
    int free_i = -1;
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_fds[i] == fd) {
            present = true;
        } else if (s_ws_fds[i] < 0 && free_i < 0) {
            free_i = i;
        }
    }
    if (!present && free_i >= 0) {
        s_ws_fds[free_i] = fd;
    }
    xSemaphoreGive(s_ws_lock);
}

static void ws_remove(int fd)
{
    xSemaphoreTake(s_ws_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_fds[i] == fd) {
            s_ws_fds[i] = -1;
        }
    }
    xSemaphoreGive(s_ws_lock);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) { /* handshake done */
        ws_add(httpd_req_to_sockfd(req));
        return ESP_OK;
    }
    httpd_ws_frame_t frame = { .type = HTTPD_WS_TYPE_TEXT };
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) {
        ws_remove(httpd_req_to_sockfd(req));
        return err;
    }
    if (frame.len > 0 && frame.len <= 128) { /* clients send nothing we need; drain it */
        uint8_t tmp[129];
        frame.payload = tmp;
        httpd_ws_recv_frame(req, &frame, frame.len);
    } else if (frame.len > 128) {
        ws_remove(httpd_req_to_sockfd(req));
        return ESP_FAIL;
    }
    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        ws_remove(httpd_req_to_sockfd(req));
    }
    return ESP_OK;
}

void pentax_http_progress_cb(const pentax_status_summary_t *st, void *ctx)
{
    (void)ctx;
    if (!s_server || !s_ws_lock) {
        return;
    }
    char buf[256];
    int n = status_json(buf, sizeof(buf), st);
    if (n <= 0 || n >= (int)sizeof(buf)) {
        return;
    }
    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)buf, .len = (size_t)n,
    };
    xSemaphoreTake(s_ws_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_fds[i] >= 0 && httpd_ws_send_frame_async(s_server, s_ws_fds[i], &frame) != ESP_OK) {
            s_ws_fds[i] = -1;
        }
    }
    xSemaphoreGive(s_ws_lock);
}

/* --------------------------------- handlers ---------------------------------- */

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, sizeof(INDEX_HTML) - 1);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    return send_status(req);
}

static esp_err_t capture_handler(httpd_req_t *req)
{
    esp_err_t err = pentax_service_capture(CAPTURE_TIMEOUT_MS);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, err == ESP_ERR_INVALID_STATE ? "409 Conflict" : "502 Bad Gateway");
        httpd_resp_set_type(req, "application/json");
        char buf[96];
        snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
        return httpd_resp_sendstr(req, buf);
    }
    return send_status(req);
}

static esp_err_t photo_get_handler(httpd_req_t *req)
{
    photo_stream_t *stream = NULL;
    esp_err_t err = pentax_service_download_start(&stream);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, err == ESP_ERR_INVALID_STATE ? HTTPD_400_BAD_REQUEST : HTTPD_500_INTERNAL_SERVER_ERROR,
                            "camera not ready");
        return ESP_OK;
    }

    /* Wait for the first block before committing to a 200: the camera may have no photo. */
    ps_block_t blk;
    esp_err_t first = ps_next(stream, BLOCK_WAIT_MS, &blk);
    if (first != ESP_OK || (blk.eof && blk.err != ESP_OK)) {
        esp_err_t why = first != ESP_OK ? first : blk.err;
        if (first == ESP_OK) {
            ps_release(stream, &blk);
        }
        pentax_service_download_end(stream);
        httpd_resp_send_err(req, why == ESP_ERR_NOT_FOUND ? HTTPD_404_NOT_FOUND : HTTPD_500_INTERNAL_SERVER_ERROR,
                            why == ESP_ERR_NOT_FOUND ? "no photo in camera buffer" : "download failed");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"photo.bin\"");

    esp_err_t result = ESP_OK;
    for (;;) {
        if (blk.len > 0 && httpd_resp_send_chunk(req, (const char *)blk.data, blk.len) != ESP_OK) {
            result = ESP_FAIL; /* client went away */
        }
        bool eof = blk.eof;
        esp_err_t producer_err = blk.err;
        ps_release(stream, &blk);
        if (result != ESP_OK) {
            break;
        }
        if (eof) {
            if (producer_err != ESP_OK) {
                result = ESP_FAIL; /* truncated: close without the terminating chunk so the client sees an error */
            }
            break;
        }
        if (ps_next(stream, BLOCK_WAIT_MS, &blk) != ESP_OK) {
            result = ESP_FAIL;
            break;
        }
    }
    pentax_service_download_end(stream); /* aborts + drains if we left early */
    if (result == ESP_OK) {
        return httpd_resp_send_chunk(req, NULL, 0);
    }
    ESP_LOGW(TAG, "photo transfer aborted");
    return ESP_FAIL;
}

static esp_err_t photo_delete_handler(httpd_req_t *req)
{
    esp_err_t err = pentax_service_delete_photo();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "delete failed");
        return ESP_OK;
    }
    return send_status(req);
}

/* ----------------------------------- start ------------------------------------ */

esp_err_t pentax_http_start(void)
{
    s_ws_lock = xSemaphoreCreateMutex();
    if (!s_ws_lock) {
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        s_ws_fds[i] = -1;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_PENTAXHUB_HTTP_PORT;
    config.stack_size = 8192;
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        return err;
    }

    static const httpd_uri_t routes[] = {
        { .uri = "/",                  .method = HTTP_GET,    .handler = index_handler },
        { .uri = "/api/pentax/status", .method = HTTP_GET,    .handler = status_handler },
        { .uri = "/api/pentax/capture",.method = HTTP_POST,   .handler = capture_handler },
        { .uri = "/api/pentax/photo",  .method = HTTP_GET,    .handler = photo_get_handler },
        { .uri = "/api/pentax/photo",  .method = HTTP_DELETE, .handler = photo_delete_handler },
        { .uri = "/ws",                .method = HTTP_GET,    .handler = ws_handler, .is_websocket = true },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &routes[i]));
    }
    s_server = server;
    ESP_LOGI(TAG, "Pentax HTTP/WebSocket server on port %d", config.server_port);
    return ESP_OK;
}
