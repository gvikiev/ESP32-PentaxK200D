#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers the PTP backend's routes (static UI assets + JSON/streaming
 * API, everything except "/") on an already-started httpd server. Call
 * after wifi_ap_start() and camera_manager_start(). */
esp_err_t web_server_register(httpd_handle_t server);

/* The "/" page content, exposed so pentaxhub_main.c's merged root handler
 * can pick it or pentax_http_index_handler() depending on which backend
 * currently has a camera attached. */
esp_err_t web_server_index_handler(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
