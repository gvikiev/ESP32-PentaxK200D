/*
 * Over-the-air firmware update (architecture doc section 6.2, POST /api/ota):
 * accepts the new firmware image as the raw POST body, writes it to the
 * next OTA partition, and reboots into it on success.
 *
 * There is no authentication beyond the WiFi AP password already gating
 * access to the device, matching every other endpoint on this server;
 * anyone who can reach the HTTP server can reflash it.
 */
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/*   POST /api/ota   body: the new firmware .bin, raw bytes */
esp_err_t ota_http_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
