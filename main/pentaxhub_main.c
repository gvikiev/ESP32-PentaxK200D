/*
 * PentaxHub: an ESP32-S3 based USB/WiFi bridge for the Pentax K200D.
 *
 * The ESP32-S3 acts as a USB host, speaking PTP (Picture Transfer
 * Protocol) to the camera over its USB port, and as a WiFi access point
 * ("PentaxHub") serving a small web UI + JSON API for remote shutter
 * release and browsing/downloading photos.
 *
 * IMPORTANT (see README.md): the K200D must have [USB Connection] set to
 * [PictBridge] in its menu, not [PC] -- [PC] mode exposes a USB Mass
 * Storage interface that this firmware cannot talk to. Whether the
 * K200D's PictBridge/PTP implementation supports remote capture (as
 * opposed to just browsing/downloading existing images) has not been
 * confirmed; see README.md for details and how to interpret /api/status.
 */
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "wifi_ap.h"
#include "camera.h"
#include "web_server.h"
#include "pentax_service.h"
#include "pentax_http.h"

static const char *TAG = "pentaxhub";

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
#if CONFIG_PENTAXHUB_BACKEND_PENTAX_SCSI
    /* Stage 2: USB MSC/BOT + Pentax SCSI. Only one backend may own the USB
     * host, so the PTP camera manager is not started in this configuration.
     * It has its own HTTP/WebSocket front end (pentax_http.c). */
    ESP_ERROR_CHECK(pentax_service_start(pentax_http_progress_cb, NULL));
    ESP_ERROR_CHECK(pentax_http_start());
#else
    ESP_ERROR_CHECK(camera_manager_start());
    ESP_ERROR_CHECK(web_server_start());
#endif

    ESP_LOGI(TAG, "PentaxHub ready - connect to WiFi '%s' and open http://192.168.4.1/", CONFIG_PENTAXHUB_AP_SSID);
}
