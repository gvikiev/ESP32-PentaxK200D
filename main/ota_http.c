#include "ota_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define OTA_RECV_BUF_SIZE 4096u

static const char *TAG = "ota_http";

static esp_err_t send_ota_error(httpd_req_t *req, const char *what, esp_err_t err)
{
    ESP_LOGE(TAG, "%s: %s", what, esp_err_to_name(err));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "500 Internal Server Error");
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t ota_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing body");
        return ESP_OK;
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no free OTA partition");
        return ESP_OK;
    }

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        return send_ota_error(req, "esp_ota_begin", err);
    }

    uint8_t *buf = malloc(OTA_RECV_BUF_SIZE);
    if (!buf) {
        esp_ota_abort(ota_handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;
    }

    int remaining = req->content_len;
    size_t total_written = 0;
    bool failed = false;
    while (remaining > 0) {
        int want = remaining < (int)OTA_RECV_BUF_SIZE ? remaining : (int)OTA_RECV_BUF_SIZE;
        int r = httpd_req_recv(req, (char *)buf, want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            ESP_LOGE(TAG, "OTA body recv failed (%d)", r);
            failed = true;
            break;
        }
        err = esp_ota_write(ota_handle, buf, (size_t)r);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            failed = true;
            break;
        }
        total_written += (size_t)r;
        remaining -= r;
    }
    free(buf);

    if (failed) {
        esp_ota_abort(ota_handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "upload failed");
        return ESP_OK;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        /* A bad image (wrong app descriptor, corrupt, too large for the
         * partition) is a client mistake, not a server fault. */
        httpd_resp_set_status(req, "400 Bad Request");
        return send_ota_error(req, "esp_ota_end", err);
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        return send_ota_error(req, "esp_ota_set_boot_partition", err);
    }

    ESP_LOGI(TAG, "OTA update accepted (%u bytes); rebooting", (unsigned)total_written);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"rebooting\"}");

    vTaskDelay(pdMS_TO_TICKS(500)); /* give the response a moment to leave the socket */
    esp_restart();
    return ESP_OK; /* unreachable */
}

esp_err_t ota_http_register(httpd_handle_t server)
{
    static const httpd_uri_t route = {
        .uri = "/api/ota", .method = HTTP_POST, .handler = ota_post_handler,
    };
    return httpd_register_uri_handler(server, &route);
}
