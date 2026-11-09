
#include "pentax_http.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <strings.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"

#include "photo_stream.h"
#include "pentax_status.h"
#include "pentax_sd.h"

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

static int json_get_int(const cJSON *obj, const char *key, int def)
{
    cJSON *v = obj ? cJSON_GetObjectItem(obj, key) : NULL;
    return cJSON_IsNumber(v) ? v->valueint : def;
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

/* Query values aren't percent-decoded; callers are expected to pass plain
 * paths (slashes and all) in ?path=, matching the examples in
 * docs/pentax_k200d_architecture_gemeni.md section 6.2. */
static void query_str(httpd_req_t *req, const char *key, char *out, size_t out_cap, const char *def)
{
    char q[192];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, key, out, out_cap) == ESP_OK) {
        return;
    }
    snprintf(out, out_cap, "%s", def);
}

/* ?type=full|preview|thumb (default: full). */
static pentax_buf_kind_t query_buf_kind(httpd_req_t *req)
{
    char q[96];
    char v[16];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "type", v, sizeof(v)) == ESP_OK) {
        if (!strcmp(v, "preview")) {
            return PENTAX_BUF_PREVIEW;
        }
        if (!strcmp(v, "thumb") || !strcmp(v, "thumbnail")) {
            return PENTAX_BUF_THUMB;
        }
    }
    return PENTAX_BUF_FULL;
}

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
static cJSON *build_capabilities(bool sd_mounted)
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
    /* 🔶 VERIFY (H-1): SD access over standard SCSI READ(10) alongside the
     * Pentax vendor channel is unconfirmed on real hardware -- see
     * docs/pentax_k200d_architecture_gemeni.md section 10. */
    cJSON_AddStringToObject(c, "sd_card_browser", sd_mounted ? "available" : "unavailable");
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

/* Last value we told the camera for a write-only parameter (no field in
 * [00 08] reflects these back; see pentax_service_get_writeonly_cache()). */
static cJSON *build_write_only(const pentax_writeonly_cache_t *wo)
{
    cJSON *w = cJSON_CreateObject();

    cJSON *wb = cJSON_AddObjectToObject(w, "white_balance");
    cJSON_AddBoolToObject(wb, "set", wo->wb_set);
    cJSON_AddNumberToObject(wb, "mode", wo->wb_mode);

    cJSON *wbt = cJSON_AddObjectToObject(w, "wb_tuning");
    cJSON_AddBoolToObject(wbt, "set", wo->wb_tune_set);
    cJSON_AddNumberToObject(wbt, "mode", wo->wb_tune_mode);
    cJSON_AddNumberToObject(wbt, "mg", wo->wb_mg);
    cJSON_AddNumberToObject(wbt, "ba", wo->wb_ba);

    cJSON *ae = cJSON_AddObjectToObject(w, "ae_metering");
    cJSON_AddBoolToObject(ae, "set", wo->ae_metering_set);
    cJSON_AddNumberToObject(ae, "mode", wo->ae_metering_mode);

    cJSON *fm = cJSON_AddObjectToObject(w, "flash_mode");
    cJSON_AddBoolToObject(fm, "set", wo->flash_mode_set);
    cJSON_AddNumberToObject(fm, "mode", wo->flash_mode);

    cJSON *fe = cJSON_AddObjectToObject(w, "flash_ev_comp");
    cJSON_AddBoolToObject(fe, "set", wo->flash_ev_comp_set);
    cJSON_AddNumberToObject(fe, "nom", wo->flash_ev_comp_nom);
    cJSON_AddNumberToObject(fe, "denom", wo->flash_ev_comp_denom);

    return w;
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

    pentax_writeonly_cache_t wo;
    pentax_service_get_writeonly_cache(&wo);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "state", state_str(st.state));
    cJSON_AddNumberToObject(root, "buffer_mask", st.buffer_mask);
    cJSON_AddNumberToObject(root, "bytes_done", st.bytes_done);
    cJSON_AddNumberToObject(root, "bytes_total", st.bytes_total);
    cJSON_AddStringToObject(root, "last_error", st.last_error);

    cJSON *buffers = cJSON_AddArrayToObject(root, "buffers");
    for (uint32_t i = 0; i < PENTAX_RAM_SLOTS; i++) {
        cJSON_AddItemToArray(buffers, cJSON_CreateBool((st.buffer_mask & (1u << i)) != 0));
    }

    cJSON_AddBoolToObject(root, "params_valid", fs_ok);
    if (fs_ok) {
        cJSON_AddItemToObject(root, "params", build_params(&fs));
    }
    cJSON_AddItemToObject(root, "write_only", build_write_only(&wo));

    bool sd_mounted = pentax_sd_is_mounted();
    cJSON *sd = cJSON_AddObjectToObject(root, "sd");
    cJSON_AddBoolToObject(sd, "mounted", sd_mounted);

    cJSON_AddItemToObject(root, "capabilities", build_capabilities(sd_mounted));

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

esp_err_t pentax_http_index_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "[HTTP] GET / (pentax)");

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

/* Shared tail for the simple action endpoints below: error -> JSON
 * {"ok":false,"error":...} with a status code, success -> the lightweight
 * status body (same as capture_handler's own response). */
static esp_err_t send_action_result(httpd_req_t *req, esp_err_t err, const char *what, uint32_t id)
{
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[HTTP #%lu] %s FAILED: %s", (unsigned long)id, what, esp_err_to_name(err));
        httpd_resp_set_status(req, err == ESP_ERR_INVALID_STATE ? "409 Conflict" : "502 Bad Gateway");
        httpd_resp_set_type(req, "application/json");
        char buf[96];
        snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
        return httpd_resp_sendstr(req, buf);
    }
    ESP_LOGI(TAG, "[HTTP #%lu] %s SUCCESS", (unsigned long)id, what);
    return send_status(req);
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

static esp_err_t af_handler(httpd_req_t *req)
{
    uint32_t id = ++s_http_request_id;
    ESP_LOGI(TAG, "[HTTP #%lu] POST /api/pentax/af", (unsigned long)id);
    return send_action_result(req, pentax_service_af(), "AF", id);
}

static esp_err_t green_button_handler(httpd_req_t *req)
{
    uint32_t id = ++s_http_request_id;
    ESP_LOGI(TAG, "[HTTP #%lu] POST /api/pentax/green-button", (unsigned long)id);
    return send_action_result(req, pentax_service_green_button(), "GREEN BUTTON", id);
}

static esp_err_t dust_removal_handler(httpd_req_t *req)
{
    uint32_t id = ++s_http_request_id;
    ESP_LOGI(TAG, "[HTTP #%lu] POST /api/pentax/dust-removal", (unsigned long)id);
    return send_action_result(req, pentax_service_dust_removal(), "DUST REMOVAL", id);
}

static esp_err_t tether_handler(httpd_req_t *req)
{
    uint32_t id = ++s_http_request_id;
    bool connect = true;
    char body[64];
    if (read_body(req, body, sizeof(body)) == ESP_OK) {
        cJSON *in = cJSON_Parse(body);
        if (in) {
            cJSON *jc = cJSON_GetObjectItem(in, "connect");
            if (cJSON_IsBool(jc)) {
                connect = cJSON_IsTrue(jc);
            }
            cJSON_Delete(in);
        }
    }
    ESP_LOGI(TAG, "[HTTP #%lu] POST /api/pentax/tether connect=%d", (unsigned long)id, (int)connect);
    return send_action_result(req, pentax_service_set_tether(connect), "TETHER", id);
}

#define BULB_MIN_DURATION_MS 1000u
#define BULB_MAX_DURATION_MS 600000u /* 10 minutes */
#define BULB_WAIT_TIMEOUT_MS 15000u  /* for the resulting frame, once the shutter closes */

static esp_err_t bulb_handler(httpd_req_t *req)
{
    uint32_t id = ++s_http_request_id;
    char body[64];
    int duration_ms = 30000;
    if (read_body(req, body, sizeof(body)) == ESP_OK) {
        cJSON *in = cJSON_Parse(body);
        duration_ms = json_get_int(in, "duration_ms", 30000);
        if (in) {
            cJSON_Delete(in);
        }
    }
    if (duration_ms < (int)BULB_MIN_DURATION_MS || duration_ms > (int)BULB_MAX_DURATION_MS) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "duration_ms out of range (1000..600000)");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "[HTTP #%lu] POST /api/pentax/bulb duration_ms=%d START", (unsigned long)id, duration_ms);
    esp_err_t err = pentax_service_bulb_capture((uint32_t)duration_ms, BULB_WAIT_TIMEOUT_MS);
    return send_action_result(req, err, "BULB", id);
}

static esp_err_t photo_get_handler(httpd_req_t *req)
{
    uint32_t id = ++s_http_request_id;

    uint32_t slot = (uint32_t)query_long(req, "slot", 0);
    pentax_buf_kind_t kind = query_buf_kind(req);

    ESP_LOGI(TAG, "[HTTP #%lu] GET /api/pentax/photo?slot=%u START",
             (unsigned long)id, (unsigned)slot);

    photo_stream_t *stream = NULL;
    esp_err_t err = pentax_service_download_start(slot, kind, &stream);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[HTTP #%lu] DOWNLOAD START FAILED: %s",
                 (unsigned long)id, esp_err_to_name(err));

        httpd_resp_send_err(req,
            (err == ESP_ERR_INVALID_STATE || err == ESP_ERR_INVALID_ARG)
                ? HTTPD_400_BAD_REQUEST
                : HTTPD_500_INTERNAL_SERVER_ERROR,
            err == ESP_ERR_INVALID_ARG ? "slot must be 0..9" : "camera not ready");

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

    uint32_t slot = (uint32_t)query_long(req, "slot", 0);

    ESP_LOGW(TAG,
        "[HTTP #%lu] DELETE /api/pentax/photo?slot=%u START",
        (unsigned long)id, (unsigned)slot);

    esp_err_t err = pentax_service_delete_photo(slot);

    if (err != ESP_OK) {
        ESP_LOGE(TAG,
            "[HTTP #%lu] DELETE FAILED: %s",
            (unsigned long)id,
            esp_err_to_name(err));

        httpd_resp_send_err(req,
            err == ESP_ERR_INVALID_ARG ? HTTPD_400_BAD_REQUEST : HTTPD_500_INTERNAL_SERVER_ERROR,
            err == ESP_ERR_INVALID_ARG ? "slot must be 0..9" : "delete failed");

        return ESP_OK;
    }

    ESP_LOGW(TAG,
        "[HTTP #%lu] DELETE SUCCESS",
        (unsigned long)id);

    return send_status(req);
}

/* ---------------------------------- SD card ----------------------------------- *
 * 🔶 VERIFY (H-1): unconfirmed on real hardware whether the K200D answers
 * plain SCSI READ(10) in [PC] mode without a menu switch to [PictBridge].
 * See docs/pentax_k200d_architecture_gemeni.md sections 3 and 10. */

/* Rejects anything that isn't an absolute, traversal-free path and builds
 * the full VFS path (mount base + rel) into out. */
static esp_err_t sd_full_path(const char *rel, char *out, size_t out_cap)
{
    if (!rel || rel[0] != '/' || strstr(rel, "..")) {
        return ESP_ERR_INVALID_ARG;
    }
    int n = snprintf(out, out_cap, "%s%s", pentax_sd_base_path(), rel);
    if (n < 0 || (size_t)n >= out_cap) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static const char *sd_content_type(const char *filename)
{
    const char *dot = strrchr(filename, '.');
    if (!dot) {
        return "application/octet-stream";
    }
    if (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg")) return "image/jpeg";
    if (!strcasecmp(dot, ".png")) return "image/png";
    if (!strcasecmp(dot, ".tif") || !strcasecmp(dot, ".tiff")) return "image/tiff";
    if (!strcasecmp(dot, ".dng")) return "image/x-adobe-dng";
    return "application/octet-stream";
}

static esp_err_t sd_unavailable_err(httpd_req_t *req, esp_err_t err)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, err == ESP_ERR_INVALID_STATE ? "409 Conflict" : "500 Internal Server Error");
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t sd_list_handler(httpd_req_t *req)
{
    uint32_t id = ++s_http_request_id;

    char rel[160];
    query_str(req, "path", rel, sizeof(rel), "/");

    esp_err_t err = pentax_sd_ensure_mounted();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[HTTP #%lu] GET /api/sd/list not mounted: %s", (unsigned long)id, esp_err_to_name(err));
        return sd_unavailable_err(req, err);
    }

    char full[200];
    if (sd_full_path(rel, full, sizeof(full)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid path");
        return ESP_OK;
    }

    DIR *dir = opendir(full);
    if (!dir) {
        ESP_LOGW(TAG, "[HTTP #%lu] GET /api/sd/list '%s' failed (errno %d)", (unsigned long)id, full, errno);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "path not found");
        return ESP_OK;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "path", rel);
    cJSON *entries = cJSON_AddArrayToObject(root, "entries");

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) {
            continue;
        }
        char entry_full[512]; /* full[200] + '/' + up to NAME_MAX bytes of d_name */
        snprintf(entry_full, sizeof(entry_full), "%s/%s", full, de->d_name);
        struct stat st;
        bool is_dir = false;
        long size = 0;
        if (stat(entry_full, &st) == 0) {
            is_dir = S_ISDIR(st.st_mode);
            size = (long)st.st_size;
        }
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", de->d_name);
        cJSON_AddBoolToObject(e, "is_dir", is_dir);
        cJSON_AddNumberToObject(e, "size", (double)size);
        cJSON_AddItemToArray(entries, e);
    }
    closedir(dir);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json build failed");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t send_err = httpd_resp_sendstr(req, out);
    cJSON_free(out);
    return send_err;
}

static esp_err_t sd_file_handler(httpd_req_t *req)
{
    uint32_t id = ++s_http_request_id;

    char rel[160];
    query_str(req, "path", rel, sizeof(rel), "");

    esp_err_t err = pentax_sd_ensure_mounted();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[HTTP #%lu] GET /api/sd/file not mounted: %s", (unsigned long)id, esp_err_to_name(err));
        return sd_unavailable_err(req, err);
    }

    char full[200];
    if (rel[0] == '\0' || sd_full_path(rel, full, sizeof(full)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid path");
        return ESP_OK;
    }

    struct stat st;
    if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "file not found");
        return ESP_OK;
    }
    FILE *f = fopen(full, "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed");
        return ESP_OK;
    }

    long total = (long)st.st_size;
    long start = 0, end = total - 1;
    bool partial = false;

    char range[64];
    if (httpd_req_get_hdr_value_str(req, "Range", range, sizeof(range)) == ESP_OK) {
        long r_start = 0, r_end = 0;
        if (sscanf(range, "bytes=%ld-%ld", &r_start, &r_end) == 2) {
            start = r_start;
            end = r_end;
            partial = true;
        } else if (sscanf(range, "bytes=%ld-", &r_start) == 1) {
            start = r_start;
            end = total - 1;
            partial = true;
        }
        if (total == 0 || start < 0 || end >= total || start > end) {
            fclose(f);
            httpd_resp_set_status(req, "416 Range Not Satisfiable");
            char cr[32];
            snprintf(cr, sizeof(cr), "bytes */%ld", total);
            httpd_resp_set_hdr(req, "Content-Range", cr);
            return httpd_resp_send(req, NULL, 0);
        }
    }

    long send_len = end - start + 1;
    const char *filename = strrchr(full, '/');
    filename = filename ? filename + 1 : full;

    httpd_resp_set_type(req, sd_content_type(filename));
    char disp[256]; /* filename is a suffix of full[200]; room for the longest possible one plus the literal text */
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", filename);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");
    if (partial) {
        httpd_resp_set_status(req, "206 Partial Content");
        char cr[64];
        snprintf(cr, sizeof(cr), "bytes %ld-%ld/%ld", start, end, total);
        httpd_resp_set_hdr(req, "Content-Range", cr);
    }

    fseek(f, start, SEEK_SET);
    uint8_t buf[2048];
    long remaining = send_len;
    esp_err_t result = ESP_OK;
    while (remaining > 0) {
        size_t want = remaining < (long)sizeof(buf) ? (size_t)remaining : sizeof(buf);
        size_t got = fread(buf, 1, want, f);
        if (got == 0) {
            break;
        }
        if (httpd_resp_send_chunk(req, (const char *)buf, got) != ESP_OK) {
            result = ESP_FAIL;
            break;
        }
        remaining -= (long)got;
    }
    fclose(f);
    if (result == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0);
    }

    ESP_LOGI(TAG, "[HTTP #%lu] GET /api/sd/file '%s' -> %s (%ld/%ld bytes%s)",
             (unsigned long)id, full, esp_err_to_name(result), send_len, total, partial ? ", partial" : "");
    return result;
}

static esp_err_t param_post_handler(httpd_req_t *req)
{
    uint32_t id = ++s_http_request_id;

    char body[160];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large or missing");
        return ESP_OK;
    }

    cJSON *in = cJSON_Parse(body);
    cJSON *jparam = in ? cJSON_GetObjectItem(in, "param") : NULL;
    const char *param = cJSON_IsString(jparam) ? jparam->valuestring : NULL;
    esp_err_t err = ESP_ERR_INVALID_ARG;

    if (param) {
        if (!strcmp(param, "shutter_speed")) {
            err = pentax_service_set_shutter_speed((uint32_t)json_get_int(in, "nom", 0),
                                                    (uint32_t)json_get_int(in, "denom", 0));
        } else if (!strcmp(param, "aperture")) {
            err = pentax_service_set_aperture((uint32_t)json_get_int(in, "nom", 0),
                                               (uint32_t)json_get_int(in, "denom", 0));
        } else if (!strcmp(param, "iso")) {
            err = pentax_service_set_iso((uint32_t)json_get_int(in, "value", 0),
                                          (uint32_t)json_get_int(in, "auto_min", 0),
                                          (uint32_t)json_get_int(in, "auto_max", 0));
        } else if (!strcmp(param, "ev_comp")) {
            err = pentax_service_set_ev_comp(json_get_int(in, "nom", 0), json_get_int(in, "denom", 1));
        } else if (!strcmp(param, "exposure_mode")) {
            err = pentax_service_set_exposure_mode((uint32_t)json_get_int(in, "value", 0));
        } else if (!strcmp(param, "file_format")) {
            err = pentax_service_set_file_format((uint32_t)json_get_int(in, "value", 0));
        } else if (!strcmp(param, "jpeg_quality")) {
            err = pentax_service_set_jpeg_quality((uint32_t)json_get_int(in, "value", 0));
        } else if (!strcmp(param, "jpeg_resolution")) {
            err = pentax_service_set_jpeg_resolution((uint32_t)json_get_int(in, "value", 0));
        } else if (!strcmp(param, "raw_format")) {
            err = pentax_service_set_raw_format((uint32_t)json_get_int(in, "value", 0));
        } else if (!strcmp(param, "custom_image_tone")) {
            err = pentax_service_set_custom_image_tone((uint32_t)json_get_int(in, "value", 0));
        } else if (!strcmp(param, "af_mode")) {
            err = pentax_service_set_af_mode((uint32_t)json_get_int(in, "value", 0));
        } else if (!strcmp(param, "af_area")) {
            err = pentax_service_set_af_area((uint32_t)json_get_int(in, "value", 0));
        } else if (!strcmp(param, "af_point")) {
            err = pentax_service_set_af_point((uint32_t)json_get_int(in, "value", 0));
        } else if (!strcmp(param, "drive_mode")) {
            err = pentax_service_set_drive_mode((uint32_t)json_get_int(in, "value", 0));
        } else if (!strcmp(param, "white_balance")) {
            err = pentax_service_set_white_balance((uint32_t)json_get_int(in, "value", 0));
        } else if (!strcmp(param, "wb_tuning")) {
            err = pentax_service_set_wb_tuning((uint32_t)json_get_int(in, "mode", 0),
                                                json_get_int(in, "mg", 0), json_get_int(in, "ba", 0));
        } else if (!strcmp(param, "ae_metering")) {
            err = pentax_service_set_ae_metering((uint32_t)json_get_int(in, "value", 0));
        } else if (!strcmp(param, "flash_mode")) {
            err = pentax_service_set_flash_mode((uint32_t)json_get_int(in, "value", 0));
        } else if (!strcmp(param, "flash_ev_comp")) {
            err = pentax_service_set_flash_ev_comp(json_get_int(in, "nom", 0), json_get_int(in, "denom", 1));
        } else {
            err = ESP_ERR_NOT_SUPPORTED;
        }
    }

    ESP_LOGI(TAG, "[HTTP #%lu] POST /api/pentax/param '%s' -> %s",
             (unsigned long)id, param ? param : "?", esp_err_to_name(err));

    if (in) {
        cJSON_Delete(in);
    }

    if (err != ESP_OK) {
        httpd_resp_set_status(req,
            err == ESP_ERR_INVALID_ARG    ? "400 Bad Request" :
            err == ESP_ERR_NOT_SUPPORTED  ? "404 Not Found" :
            err == ESP_ERR_INVALID_STATE  ? "409 Conflict" : "502 Bad Gateway");
        httpd_resp_set_type(req, "application/json");
        char buf[96];
        snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
        return httpd_resp_sendstr(req, buf);
    }

    return send_full_status(req);
}

/* ----------------------------------- start ------------------------------------ */

esp_err_t pentax_http_register(httpd_handle_t server)
{
    ESP_LOGI(TAG, "Registering Pentax HTTP/WebSocket routes");

    s_ws_lock = xSemaphoreCreateMutex();

    if (!s_ws_lock) {
        ESP_LOGE(TAG, "Failed to create WebSocket mutex");
        return ESP_ERR_NO_MEM;
    }

    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        s_ws_fds[i] = -1;
    }

    static const httpd_uri_t routes[] = {
        { .uri = "/api/pentax/status", .method = HTTP_GET,    .handler = status_handler },
        { .uri = "/api/pentax/capture",.method = HTTP_POST,   .handler = capture_handler },
        { .uri = "/api/pentax/af",     .method = HTTP_POST,   .handler = af_handler },
        { .uri = "/api/pentax/green-button", .method = HTTP_POST, .handler = green_button_handler },
        { .uri = "/api/pentax/dust-removal", .method = HTTP_POST, .handler = dust_removal_handler },
        { .uri = "/api/pentax/tether", .method = HTTP_POST,   .handler = tether_handler },
        { .uri = "/api/pentax/bulb",   .method = HTTP_POST,   .handler = bulb_handler },
        { .uri = "/api/pentax/param",  .method = HTTP_POST,   .handler = param_post_handler },
        { .uri = "/api/pentax/photo",  .method = HTTP_GET,    .handler = photo_get_handler },
        { .uri = "/api/pentax/photo",  .method = HTTP_DELETE, .handler = photo_delete_handler },
        { .uri = "/api/sd/list",       .method = HTTP_GET,    .handler = sd_list_handler },
        { .uri = "/api/sd/file",       .method = HTTP_GET,    .handler = sd_file_handler },
        { .uri = "/ws",                .method = HTTP_GET,    .handler = ws_handler, .is_websocket = true },
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &routes[i]));
    }

    s_server = server;

    ESP_LOGI(TAG, "Pentax HTTP/WebSocket routes registered");

    return ESP_OK;
}
