#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Brings up the "PentaxHub" WiFi access point using the SSID/password/
 * channel configured via `idf.py menuconfig` (PentaxHub Configuration).
 * Must be called after nvs_flash_init()/esp_netif_init()/the default event
 * loop have been created. */
esp_err_t wifi_ap_start(void);

#ifdef __cplusplus
}
#endif
