#include "wifi_ap.h"

#include <string.h>
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"

#ifndef CONFIG_PENTAXHUB_AP_SSID
#define CONFIG_PENTAXHUB_AP_SSID "PentaxHub"
#endif
#ifndef CONFIG_PENTAXHUB_AP_PASSWORD
#define CONFIG_PENTAXHUB_AP_PASSWORD "pentaxhub"
#endif
#ifndef CONFIG_PENTAXHUB_AP_CHANNEL
#define CONFIG_PENTAXHUB_AP_CHANNEL 6
#endif
#ifndef CONFIG_PENTAXHUB_AP_MAX_CONN
#define CONFIG_PENTAXHUB_AP_MAX_CONN 4
#endif

static const char *TAG = "wifi_ap";

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base != WIFI_EVENT) {
        return;
    }
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)data;
        ESP_LOGI(TAG, "station " MACSTR " joined, AID=%d", MAC2STR(e->mac), e->aid);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)data;
        ESP_LOGI(TAG, "station " MACSTR " left, AID=%d", MAC2STR(e->mac), e->aid);
    }
}

esp_err_t wifi_ap_start(void)
{
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                         &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid_len = strlen(CONFIG_PENTAXHUB_AP_SSID),
            .channel = CONFIG_PENTAXHUB_AP_CHANNEL,
            .max_connection = CONFIG_PENTAXHUB_AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = { .required = false },
        },
    };
    strncpy((char *)wifi_config.ap.ssid, CONFIG_PENTAXHUB_AP_SSID, sizeof(wifi_config.ap.ssid) - 1);
    strncpy((char *)wifi_config.ap.password, CONFIG_PENTAXHUB_AP_PASSWORD, sizeof(wifi_config.ap.password) - 1);
    if (strlen(CONFIG_PENTAXHUB_AP_PASSWORD) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP '%s' started on channel %d (192.168.4.1)",
             CONFIG_PENTAXHUB_AP_SSID, CONFIG_PENTAXHUB_AP_CHANNEL);
    return ESP_OK;
}
