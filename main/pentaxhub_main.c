/*
 * PentaxHub: an ESP32-S3 based USB/WiFi bridge for the Pentax K200D.
 *
 * The ESP32-S3 acts as a USB host and as a WiFi access point ("PentaxHub")
 * serving a web UI + JSON API for remote shutter release and browsing/
 * downloading photos. Both camera backends run at the same time:
 *
 *   - Pentax vendor SCSI over USB Mass Storage (pentax_service.c), used
 *     when the camera's [USB Connection] menu is set to [PC]. The more
 *     capable backend: full parameter control, RAM buffer access, SD card
 *     browsing (see docs/pentax_k200d_architecture_gemeni.md).
 *   - Standard PTP (camera.c), used when it is set to [PictBridge] instead.
 *
 * usb_host_shared.c lets both USB transports (usb_msc_link.c, ptp_transport.c)
 * coexist; whichever USB interface class the attached camera actually
 * exposes decides which backend has a camera to talk to. Switching the
 * camera's own menu setting (and replugging it) switches backends without
 * reflashing or rebooting the ESP32 -- see docs/pentax_k200d_architecture_gemeni.md
 * section 5.1 ("Device Classifier & Transport Multiplexer").
 */
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs_flash.h"

#include "wifi_ap.h"
#include "camera.h"
#include "web_server.h"
#include "pentax_service.h"
#include "pentax_http.h"
#include "ota_http.h"

#ifndef CONFIG_PENTAXHUB_HTTP_PORT
#define CONFIG_PENTAXHUB_HTTP_PORT 80
#endif

static const char *TAG = "pentaxhub";

/* "/" is the one route both backends want; everything else lives under
 * distinct prefixes (/api/pentax, /ws vs /api, /app.js, /style.css) and is
 * registered directly by web_server_register()/pentax_http_register().
 * This picks whichever backend currently has a camera attached, defaulting
 * to the Pentax SCSI page (the more capable backend) when neither does. */
static esp_err_t root_index_handler(httpd_req_t *req)
{
    pentax_status_summary_t pentax_st;
    pentax_service_get_status(&pentax_st);
    if (pentax_st.state != PS_STATE_DISCONNECTED) {
        return pentax_http_index_handler(req);
    }

    cam_status_t cam_st;
    camera_get_status(&cam_st);
    if (cam_st.state != CAM_STATE_DISCONNECTED) {
        return web_server_index_handler(req);
    }

    return pentax_http_index_handler(req);
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(wifi_ap_start());

    ESP_ERROR_CHECK(camera_manager_start());
    ESP_ERROR_CHECK(pentax_service_start(pentax_http_progress_cb, NULL));

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_PENTAXHUB_HTTP_PORT;
    config.stack_size = 8192;
    config.max_uri_handlers = 40; /* both backends' routes, OTA, and the shared "/" */
    config.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    static const httpd_uri_t root_route = {
        .uri = "/", .method = HTTP_GET, .handler = root_index_handler,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root_route));
    ESP_ERROR_CHECK(web_server_register(server));
    ESP_ERROR_CHECK(pentax_http_register(server));
    ESP_ERROR_CHECK(ota_http_register(server));

    ESP_LOGI(TAG, "PentaxHub ready - connect to WiFi '%s' and open http://192.168.4.1/", CONFIG_PENTAXHUB_AP_SSID);
}
