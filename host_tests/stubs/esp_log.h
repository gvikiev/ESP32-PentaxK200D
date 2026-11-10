/* Host-test stand-in for esp_log.h: logging is a no-op here so test output
 * stays limited to the test framework's own PASS/FAIL lines. */
#pragma once

#define ESP_LOGE(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGD(tag, ...) ((void)(tag))
#define ESP_LOGV(tag, ...) ((void)(tag))
