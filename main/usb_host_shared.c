#include "usb_host_shared.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "usb/usb_host.h"

#ifndef CONFIG_PENTAXHUB_USB_DAEMON_TASK_PRIO
#define CONFIG_PENTAXHUB_USB_DAEMON_TASK_PRIO 5
#endif

static const char *TAG = "usb_host_shared";
static bool s_installed;

static void lib_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
    }
}

esp_err_t usb_host_shared_acquire(void)
{
    /* Called only from app_main() (sequentially, by both backends' start
     * functions), so a plain flag is enough -- no concurrent callers. */
    if (s_installed) {
        return ESP_OK;
    }

    usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    esp_err_t err = usb_host_install(&host_cfg);
    if (err != ESP_OK) {
        return err;
    }

    if (xTaskCreatePinnedToCore(lib_task, "usb_lib", 4096, NULL,
                                 CONFIG_PENTAXHUB_USB_DAEMON_TASK_PRIO, NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_installed = true;
    ESP_LOGI(TAG, "USB Host Library installed");
    return ESP_OK;
}
