
#include "pentax_http.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"

#include "photo_stream.h"
#include "pentax_status.h"

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

static uint32_t s_http_request_id;

static const char INDEX_HTML[] =
    "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>PentaxHub K200D</title>"
    "<style>"
    ":root{--bg:#15171a;--panel:#1e2125;--panel2:#24282d;--edge:#34383e;--accent:#1c63d6;--accent-d:#123e8c;"
    "--text:#eef0f2;--muted:#8b919b;--warn:#e0a438;--bad:#d9453a;--lcd-bg:#c6cdb0;--lcd-fg:#262b1d;--lcd-dim:#8a9077}"
    "*{box-sizing:border-box}"
    "[hidden]{display:none!important}"
    "body{margin:0;background:var(--bg);color:var(--text);font-family:Arial,Helvetica,sans-serif;min-height:100vh}"
    "header{display:flex;align-items:center;justify-content:space-between;gap:.5em;padding:.8em 1em;"
    "background:linear-gradient(var(--panel2),var(--panel));border-bottom:2px solid var(--edge)}"
    ".brand{display:flex;align-items:baseline;gap:.5em}"
    ".brand b{color:var(--accent);font-size:1.3em;letter-spacing:.06em}"
    ".brand span{color:var(--muted);font-size:.85em}"
    "nav{display:flex;gap:.4em}"
    "nav button{background:var(--panel2);color:var(--muted);border:1px solid var(--edge);border-radius:999px;"
    "padding:.5em 1.1em;font-size:.95em;cursor:pointer}"
    "nav button.active{background:var(--accent);color:#fff;border-color:var(--accent)}"
    "main{max-width:30em;margin:0 auto;padding:1.2em 1em 2em}"
    "section{display:none}"
    "section.active{display:block}"
    ".card{background:var(--panel);border:1px solid var(--edge);border-radius:.8em;padding:1em;margin-bottom:1em}"
    ".lcd{background:var(--lcd-bg);color:var(--lcd-fg);border-radius:.4em;padding:.8em 1em;"
    "font-family:Consolas,Menlo,monospace;border:3px solid #3a3e32;box-shadow:inset 0 0 8px rgba(0,0,0,.35)}"
    ".lcd-row{display:flex;justify-content:space-between;padding:.15em 0}"
    ".lcd-row.err{color:var(--bad)}"
    ".lcd-label{color:var(--lcd-dim);font-size:.8em;text-transform:uppercase;letter-spacing:.08em}"
    ".lcd-value{font-weight:bold}"
    ".state-busy .lcd-value{color:var(--warn)}"
    ".state-bad .lcd-value{color:var(--bad)}"
    ".progress-wrap{margin-top:.7em}"
    "progress{width:100%;height:.6em;accent-color:var(--accent)}"
    "#progText{font-size:.8em;color:var(--muted)}"
    ".shutter{display:block;margin:1.4em auto;width:5.5em;height:5.5em;border-radius:50%;"
    "background:radial-gradient(circle at 35% 30%,#3a3d42,#17191c 70%);border:4px solid var(--edge);"
    "position:relative;cursor:pointer}"
    ".shutter::after{content:'';position:absolute;inset:.5em;border-radius:50%;background:var(--bad);"
    "box-shadow:inset 0 0 0 3px #00000033}"
    ".shutter:disabled{opacity:.4;cursor:not-allowed}"
    ".shutter:disabled::after{background:#555}"
    ".hint{color:var(--muted);font-size:.85em;text-align:center;line-height:1.4}"
    ".leds{display:flex;gap:.5em;justify-content:center;margin-bottom:1em}"
    ".led{width:1.4em;height:1.4em;border-radius:.3em;background:var(--panel2);border:1px solid var(--edge)}"
    ".led.on{background:var(--accent);box-shadow:0 0 .5em var(--accent)}"
    ".preview{background:#000;border:1px solid var(--edge);border-radius:.6em;min-height:12em;"
    "display:flex;align-items:center;justify-content:center;overflow:hidden;margin-bottom:1em}"
    ".preview img{max-width:100%;max-height:22em;display:block}"
    ".preview span{color:var(--muted);font-size:.85em;padding:1em;text-align:center}"
    ".actions{display:flex;gap:.6em;flex-wrap:wrap}"
    ".btn{flex:1;text-align:center;background:var(--accent);color:#fff;border:none;border-radius:.5em;"
    "padding:.7em;font-size:.95em;cursor:pointer;text-decoration:none}"
    ".btn.secondary{background:var(--panel2);color:var(--text);border:1px solid var(--edge)}"
    ".btn.danger{background:var(--bad)}"
    ".btn.is-disabled{opacity:.4;pointer-events:none}"
    "footer{text-align:center;color:var(--muted);font-size:.8em;padding:1em}"
    "</style>"
    "<header><div class=brand><b>PENTAX</b><span>K200D &middot; PentaxHub</span></div>"
    "<nav><button class='tab active' data-t=camera>Камера</button><button class=tab data-t=photos>Снимки</button></nav>"
    "</header>"
    "<main>"
    "<section id=camera class=active>"
    "<div class='card lcd' id=lcd>"
    "<div class=lcd-row><span class=lcd-label>Статус</span><span id=stState class=lcd-value>&mdash;</span></div>"
    "<div class=lcd-row><span class=lcd-label>Буфер камеры</span><span id=stBuf class=lcd-value>&mdash;</span></div>"
    "<div class='lcd-row err' id=errRow hidden><span class=lcd-label>Ошибка</span><span id=errText class=lcd-value></span></div>"
    "<div class=progress-wrap id=progWrap hidden><progress id=prog value=0 max=1></progress>"
    "<div id=progText>0 / 0 байт</div></div>"
    "</div>"
    "<button id=shutter class=shutter title='Спуск затвора'></button>"
    "<p class=hint>Удалённый спуск затвора по USB.<br>Live View и видеосъёмка камерой K200D не поддерживаются.</p>"
    "</section>"
    "<section id=photos>"
    "<div class=card>"
    "<div class=leds id=leds></div>"
    "<div class=preview id=preview><span id=previewMsg>Нет предпросмотра</span></div>"
    "<div class=actions>"
    "<button id=btnShow class=btn>Показать снимок</button>"
    "<a id=btnDownload class='btn secondary' href=/api/pentax/photo download=photo.bin>Скачать</a>"
    "<button id=btnDelete class='btn danger'>Удалить с камеры</button>"
    "</div>"
    "<p class=hint>Снимок хранится в буфере камеры. Предпросмотр загружает файл целиком и недоступен для RAW (PEF).</p>"
    "</div>"
    "</section>"
    "</main>"
    "<footer>PentaxHub (ESP32-S3) &middot; Pentax K200D &middot; USB SCSI backend</footer>"
    "<script>"
    "var tabs=document.querySelectorAll('.tab'),secs=document.querySelectorAll('section');"
    "tabs.forEach(function(t){t.onclick=function(){tabs.forEach(function(x){x.classList.remove('active')});"
    "secs.forEach(function(s){s.classList.remove('active')});t.classList.add('active');"
    "document.getElementById(t.dataset.t).classList.add('active')}});"
    "var lcd=document.getElementById('lcd'),stState=document.getElementById('stState'),stBuf=document.getElementById('stBuf'),"
    "errRow=document.getElementById('errRow'),errText=document.getElementById('errText'),"
    "progWrap=document.getElementById('progWrap'),prog=document.getElementById('prog'),progText=document.getElementById('progText'),"
    "shutter=document.getElementById('shutter'),leds=document.getElementById('leds'),"
    "btnShow=document.getElementById('btnShow'),btnDownload=document.getElementById('btnDownload'),btnDelete=document.getElementById('btnDelete'),"
    "preview=document.getElementById('preview'),previewMsg=document.getElementById('previewMsg');"
    "for(var i=0;i<4;i++){var d=document.createElement('div');d.className='led';leds.appendChild(d)}"
    "var STATE_RU={ready:'Готова',busy:'Съёмка/передача',wrong_usb_mode:'Неверный режим USB',disconnected:'Камера не подключена'};"
    "function renderStatus(st){"
    "stState.textContent=STATE_RU[st.state]||st.state;"
    "lcd.classList.remove('state-busy','state-bad');"
    "if(st.state=='busy')lcd.classList.add('state-busy');"
    "if(st.state=='wrong_usb_mode'||st.state=='disconnected')lcd.classList.add('state-bad');"
    "var n=0;for(var i=0;i<4;i++){var on=!!(st.buffer_mask&(1<<i));"
    "leds.children[i].classList.toggle('on',on);if(on)n++}"
    "stBuf.textContent=n?n+' снимок(ов)':'пусто';"
    "var hasPhoto=st.buffer_mask!=0;"
    "btnShow.classList.toggle('is-disabled',!hasPhoto);"
    "btnDownload.classList.toggle('is-disabled',!hasPhoto);"
    "btnDelete.classList.toggle('is-disabled',!hasPhoto);"
    "shutter.disabled=(st.state!='ready');"
    "if(st.last_error){errRow.hidden=false;errText.textContent=st.last_error}else{errRow.hidden=true}"
    "if(st.state=='busy'&&st.bytes_total>0){progWrap.hidden=false;prog.max=st.bytes_total;prog.value=st.bytes_done;"
    "progText.textContent=st.bytes_done+' / '+st.bytes_total+' байт'}else{progWrap.hidden=true}"
    "}"
    "function fetchStatus(){fetch('/api/pentax/status').then(function(r){return r.json()}).then(renderStatus).catch(function(){})}"
    "shutter.onclick=function(){shutter.disabled=true;fetch('/api/pentax/capture',{method:'POST'}).then(function(r){return r.json()})"
    ".then(function(j){if(j.state)renderStatus(j);else{errRow.hidden=false;errText.textContent=j.error||'Ошибка съёмки'}})"
    ".catch(function(){}).then(fetchStatus)};"
    "btnDelete.onclick=function(){if(btnDelete.classList.contains('is-disabled'))return;"
    "if(!confirm('Удалить снимок из буфера камеры?'))return;"
    "fetch('/api/pentax/photo',{method:'DELETE'}).then(function(r){return r.ok?r.json():null})"
    ".then(function(j){if(j)renderStatus(j)}).catch(function(){}).then(fetchStatus)};"
    "var curUrl=null;"
    "btnShow.onclick=function(){if(btnShow.classList.contains('is-disabled'))return;"
    "previewMsg.textContent='Загрузка...';previewMsg.style.display='block';"
    "var img=preview.querySelector('img');if(img)img.remove();"
    "fetch('/api/pentax/photo').then(function(r){if(!r.ok)throw 0;return r.blob()}).then(function(b){"
    "if(curUrl)URL.revokeObjectURL(curUrl);curUrl=URL.createObjectURL(b);"
    "var im=new Image();im.onload=function(){previewMsg.style.display='none';preview.appendChild(im)};"
    "im.onerror=function(){previewMsg.textContent='Предпросмотр недоступен (вероятно RAW/PEF)'};"
    "im.src=curUrl}).catch(function(){previewMsg.textContent='Нет снимка в буфере камеры'})};"
    "var ws,wsTimer;"
    "function connectWs(){try{ws=new WebSocket('ws://'+location.host+'/ws')}catch(e){return}"
    "ws.onmessage=function(e){try{renderStatus(JSON.parse(e.data))}catch(e){}};"
    "ws.onclose=function(){clearTimeout(wsTimer);wsTimer=setTimeout(connectWs,3000)}}"
    "fetchStatus();connectWs();setInterval(fetchStatus,8000);"
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

/* ---------------------------- full status (params) --------------------------- */

static void add_fraction(cJSON *parent, const char *key, double nom, double denom)
{
    cJSON *f = cJSON_AddObjectToObject(parent, key);
    cJSON_AddNumberToObject(f, "nom", nom);
    cJSON_AddNumberToObject(f, "denom", denom);
}

/* Static per-parameter availability matrix (docs/pentax_k200d_web_ui_specification.md
 * section 1 and docs/pentax_k200d_architecture_gemeni.md section 2.2): which
 * fields are confirmed read/write ("available"), settable but not readable
 * back from the camera ("write_only"), readable but without a known SCSI
 * setter ("read_only"), or not yet wired up on the ESP32 side at all. */
static cJSON *build_capabilities(void)
{
    cJSON *c = cJSON_CreateObject();
    cJSON_AddStringToObject(c, "shutter_speed", "available");
    cJSON_AddStringToObject(c, "aperture", "available");
    cJSON_AddStringToObject(c, "iso", "available");
    cJSON_AddStringToObject(c, "ev_comp", "available");
    cJSON_AddStringToObject(c, "image_format", "available");
    cJSON_AddStringToObject(c, "jpeg_quality", "available");
    cJSON_AddStringToObject(c, "jpeg_resolution", "available");
    cJSON_AddStringToObject(c, "raw_format", "available");
    cJSON_AddStringToObject(c, "custom_image_tone", "experimental");
    cJSON_AddStringToObject(c, "saturation", "experimental");
    cJSON_AddStringToObject(c, "sharpness", "experimental");
    cJSON_AddStringToObject(c, "contrast", "experimental");
    cJSON_AddStringToObject(c, "jpeg_hue", "experimental");
    cJSON_AddStringToObject(c, "af_mode", "available");
    cJSON_AddStringToObject(c, "af_area", "available");
    cJSON_AddStringToObject(c, "af_point", "available");
    cJSON_AddStringToObject(c, "drive_mode", "available");
    cJSON_AddStringToObject(c, "shake_reduction", "read_only");
    cJSON_AddStringToObject(c, "white_balance", "write_only");
    cJSON_AddStringToObject(c, "ae_metering", "write_only");
    cJSON_AddStringToObject(c, "flash_mode", "write_only");
    cJSON_AddStringToObject(c, "flash_ev_comp", "write_only");
    cJSON_AddStringToObject(c, "sd_card_browser", "unavailable");
    return c;
}

static cJSON *build_params(const k200d_status_t *fs)
{
    cJSON *p = cJSON_CreateObject();
    add_fraction(p, "shutter_speed", fs->set_shutter_nom, fs->set_shutter_denom);
    add_fraction(p, "aperture", fs->set_aperture_nom, fs->set_aperture_denom);
    add_fraction(p, "ev_comp", fs->ev_comp_nom, fs->ev_comp_denom);
    cJSON_AddNumberToObject(p, "iso", fs->iso);
    cJSON_AddNumberToObject(p, "auto_iso_min", fs->auto_iso_min);
    cJSON_AddNumberToObject(p, "auto_iso_max", fs->auto_iso_max);
    cJSON_AddNumberToObject(p, "image_format", fs->image_format);
    cJSON_AddNumberToObject(p, "jpeg_resolution_idx", fs->jpeg_res_idx);
    cJSON_AddNumberToObject(p, "jpeg_stars", fs->jpeg_stars);
    cJSON_AddNumberToObject(p, "raw_format", fs->raw_format);
    cJSON_AddNumberToObject(p, "custom_image_tone", fs->custom_image_tone);
    cJSON_AddNumberToObject(p, "saturation", fs->saturation);
    cJSON_AddNumberToObject(p, "sharpness", fs->sharpness);
    cJSON_AddNumberToObject(p, "contrast", fs->contrast);
    cJSON_AddNumberToObject(p, "jpeg_hue", fs->jpeg_hue);
    cJSON_AddNumberToObject(p, "exposure_mode", fs->exposure_mode);
    cJSON_AddNumberToObject(p, "af_mode", fs->af_mode);
    cJSON_AddNumberToObject(p, "af_area_select", fs->af_area_select);
    cJSON_AddNumberToObject(p, "af_point_mask", fs->af_point_mask);
    cJSON_AddNumberToObject(p, "focused_af_point", fs->focused_af_point);
    cJSON_AddNumberToObject(p, "drive_mode", fs->drive_mode);
    cJSON_AddBoolToObject(p, "shake_reduction", fs->shake_reduction);
    add_fraction(p, "lens_max_aperture", fs->lens_max_aperture_nom, fs->lens_max_aperture_denom);
    add_fraction(p, "lens_min_aperture", fs->lens_min_aperture_nom, fs->lens_min_aperture_denom);
    add_fraction(p, "focal_length", fs->focal_length_nom, fs->focal_length_denom);
    return p;
}

/* GET /api/pentax/status: summary + live camera parameters + capability
 * matrix. Capture/delete responses keep using the lighter send_status()
 * above so a write action isn't slowed down by an extra 408-byte read. */
static esp_err_t send_full_status(httpd_req_t *req)
{
    pentax_status_summary_t st;
    pentax_service_get_status(&st);

    k200d_status_t fs;
    bool fs_ok = (pentax_service_get_full_status(&fs) == ESP_OK);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "state", state_str(st.state));
    cJSON_AddNumberToObject(root, "buffer_mask", st.buffer_mask);
    cJSON_AddNumberToObject(root, "bytes_done", st.bytes_done);
    cJSON_AddNumberToObject(root, "bytes_total", st.bytes_total);
    cJSON_AddStringToObject(root, "last_error", st.last_error);
    cJSON_AddBoolToObject(root, "params_valid", fs_ok);
    if (fs_ok) {
        cJSON_AddItemToObject(root, "params", build_params(&fs));
    }
    cJSON_AddItemToObject(root, "capabilities", build_capabilities());

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json build failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, out);
    cJSON_free(out);
    return err;
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
        ESP_LOGI(TAG, "[WS] CONNECTED fd=%d slot=%d", fd, free_i);
    } else if (present) {
        ESP_LOGD(TAG, "[WS] fd=%d already registered", fd);
    } else {
        ESP_LOGW(TAG, "[WS] No free client slots for fd=%d", fd);
    }

    xSemaphoreGive(s_ws_lock);
}

static void ws_remove(int fd)
{
    bool removed = false;

    xSemaphoreTake(s_ws_lock, portMAX_DELAY);

    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_fds[i] == fd) {
            s_ws_fds[i] = -1;
            removed = true;
        }
    }

    xSemaphoreGive(s_ws_lock);

    if (removed) {
        ESP_LOGI(TAG, "[WS] DISCONNECTED fd=%d", fd);
    }
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);

    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "[WS] HANDSHAKE fd=%d", fd);
        ws_add(fd);
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT
    };

    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[WS] RECEIVE FAILED fd=%d err=%s",
                 fd, esp_err_to_name(err));
        ws_remove(fd);
        return err;
    }

    if (frame.len > 0 && frame.len <= 128) {
        uint8_t tmp[129];
        frame.payload = tmp;

        err = httpd_ws_recv_frame(req, &frame, frame.len);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "[WS] FRAME DRAIN FAILED fd=%d err=%s",
                     fd, esp_err_to_name(err));
            ws_remove(fd);
            return err;
        }
    } else if (frame.len > 128) {
        ESP_LOGW(TAG, "[WS] FRAME TOO LARGE fd=%d len=%u",
                 fd, (unsigned)frame.len);
        ws_remove(fd);
        return ESP_FAIL;
    }

    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        ESP_LOGI(TAG, "[WS] CLOSE FRAME fd=%d", fd);
        ws_remove(fd);
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
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)buf,
        .len = (size_t)n,
    };

    xSemaphoreTake(s_ws_lock, portMAX_DELAY);

    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_fds[i] >= 0 &&
            httpd_ws_send_frame_async(s_server, s_ws_fds[i], &frame) != ESP_OK) {
            ESP_LOGW(TAG, "[WS] SEND FAILED fd=%d", s_ws_fds[i]);
            s_ws_fds[i] = -1;
        }
    }

    xSemaphoreGive(s_ws_lock);
}

/* --------------------------------- handlers ---------------------------------- */

static esp_err_t index_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "[HTTP] GET /");

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, sizeof(INDEX_HTML) - 1);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    uint32_t id = ++s_http_request_id;

    ESP_LOGI(TAG, "[HTTP #%lu] GET /api/pentax/status",
             (unsigned long)id);

    esp_err_t err = send_full_status(req);

    ESP_LOGI(TAG, "[HTTP #%lu] STATUS RESPONSE -> %s",
             (unsigned long)id, esp_err_to_name(err));

    return err;
}

static esp_err_t capture_handler(httpd_req_t *req)
{
    uint32_t id = ++s_http_request_id;

    ESP_LOGI(TAG, "[HTTP #%lu] POST /api/pentax/capture START",
             (unsigned long)id);

    esp_err_t err = pentax_service_capture(CAPTURE_TIMEOUT_MS);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[HTTP #%lu] CAPTURE FAILED: %s",
                 (unsigned long)id, esp_err_to_name(err));

        httpd_resp_set_status(req,
            err == ESP_ERR_INVALID_STATE ? "409 Conflict" : "502 Bad Gateway");

        httpd_resp_set_type(req, "application/json");

        char buf[96];
        snprintf(buf, sizeof(buf),
                 "{\"ok\":false,\"error\":\"%s\"}",
                 esp_err_to_name(err));

        return httpd_resp_sendstr(req, buf);
    }

    ESP_LOGI(TAG, "[HTTP #%lu] CAPTURE SUCCESS",
             (unsigned long)id);

    return send_status(req);
}

static esp_err_t photo_get_handler(httpd_req_t *req)
{
    uint32_t id = ++s_http_request_id;

    ESP_LOGI(TAG, "[HTTP #%lu] GET /api/pentax/photo START",
             (unsigned long)id);

    photo_stream_t *stream = NULL;
    esp_err_t err = pentax_service_download_start(&stream);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[HTTP #%lu] DOWNLOAD START FAILED: %s",
                 (unsigned long)id, esp_err_to_name(err));

        httpd_resp_send_err(req,
            err == ESP_ERR_INVALID_STATE
                ? HTTPD_400_BAD_REQUEST
                : HTTPD_500_INTERNAL_SERVER_ERROR,
            "camera not ready");

        return ESP_OK;
    }

    ESP_LOGI(TAG, "[HTTP #%lu] DOWNLOAD TASK STARTED",
             (unsigned long)id);

    /* Wait for the first block before committing to a 200 response. */
    ps_block_t blk;
    esp_err_t first = ps_next(stream, BLOCK_WAIT_MS, &blk);

    if (first != ESP_OK || (blk.eof && blk.err != ESP_OK)) {
        esp_err_t why = first != ESP_OK ? first : blk.err;

        ESP_LOGE(TAG, "[HTTP #%lu] FIRST BLOCK FAILED: %s",
                 (unsigned long)id, esp_err_to_name(why));

        if (first == ESP_OK) {
            ps_release(stream, &blk);
        }

        pentax_service_download_end(stream);

        httpd_resp_send_err(req,
            why == ESP_ERR_NOT_FOUND
                ? HTTPD_404_NOT_FOUND
                : HTTPD_500_INTERNAL_SERVER_ERROR,
            why == ESP_ERR_NOT_FOUND
                ? "no photo in camera buffer"
                : "download failed");

        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=\"photo.bin\"");

    esp_err_t result = ESP_OK;
    uint32_t total_sent = 0;

    for (;;) {
        if (blk.len > 0) {
            esp_err_t send_err = httpd_resp_send_chunk(
                req, (const char *)blk.data, blk.len);

            if (send_err != ESP_OK) {
                result = ESP_FAIL;

                ESP_LOGW(TAG,
                    "[HTTP #%lu] CLIENT SEND FAILED: %s",
                    (unsigned long)id,
                    esp_err_to_name(send_err));
            } else {
                total_sent += blk.len;
            }
        }

        bool eof = blk.eof;
        esp_err_t producer_err = blk.err;

        ps_release(stream, &blk);

        if (result != ESP_OK) {
            break;
        }

        if (eof) {
            if (producer_err != ESP_OK) {
                result = ESP_FAIL;

                ESP_LOGE(TAG,
                    "[HTTP #%lu] DOWNLOAD PRODUCER FAILED: %s",
                    (unsigned long)id,
                    esp_err_to_name(producer_err));
            }
            break;
        }

        esp_err_t next_err = ps_next(stream, BLOCK_WAIT_MS, &blk);

        if (next_err != ESP_OK) {
            result = ESP_FAIL;

            ESP_LOGE(TAG,
                "[HTTP #%lu] WAIT NEXT BLOCK FAILED: %s",
                (unsigned long)id,
                esp_err_to_name(next_err));

            break;
        }
    }

    pentax_service_download_end(stream);

    if (result == ESP_OK) {
        ESP_LOGI(TAG,
            "[HTTP #%lu] DOWNLOAD COMPLETE: %lu bytes",
            (unsigned long)id,
            (unsigned long)total_sent);

        esp_err_t end_err = httpd_resp_send_chunk(req, NULL, 0);

        if (end_err != ESP_OK) {
            ESP_LOGW(TAG,
                "[HTTP #%lu] FINAL CHUNK FAILED: %s",
                (unsigned long)id,
                esp_err_to_name(end_err));
        }

        return end_err;
    }

    ESP_LOGW(TAG,
        "[HTTP #%lu] DOWNLOAD ABORTED: sent=%lu bytes",
        (unsigned long)id,
        (unsigned long)total_sent);

    return ESP_FAIL;
}

static esp_err_t photo_delete_handler(httpd_req_t *req)
{
    uint32_t id = ++s_http_request_id;

    ESP_LOGW(TAG,
        "[HTTP #%lu] DELETE /api/pentax/photo START",
        (unsigned long)id);

    esp_err_t err = pentax_service_delete_photo();

    if (err != ESP_OK) {
        ESP_LOGE(TAG,
            "[HTTP #%lu] DELETE FAILED: %s",
            (unsigned long)id,
            esp_err_to_name(err));

        httpd_resp_send_err(req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "delete failed");

        return ESP_OK;
    }

    ESP_LOGW(TAG,
        "[HTTP #%lu] DELETE SUCCESS",
        (unsigned long)id);

    return send_status(req);
}

/* ----------------------------------- start ------------------------------------ */

esp_err_t pentax_http_start(void)
{
    ESP_LOGI(TAG, "Starting Pentax HTTP server");

    s_ws_lock = xSemaphoreCreateMutex();

    if (!s_ws_lock) {
        ESP_LOGE(TAG, "Failed to create WebSocket mutex");
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
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
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

    ESP_LOGI(TAG,
        "Pentax HTTP/WebSocket server started on port %d",
        config.server_port);

    return ESP_OK;
}
