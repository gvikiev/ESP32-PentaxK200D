#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the HTTP server (static UI + JSON/streaming API). Call after
 * wifi_ap_start() and camera_manager_start(). */
esp_err_t web_server_start(void);

#ifdef __cplusplus
}
#endif
