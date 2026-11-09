#include "usb_host_shared.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "usb/usb_host.h"

#ifndef CONFIG_PENTAXHUB_USB_DAEMON_TASK_PRIO
#define CONFIG_PENTAXHUB_USB_DAEMON_TASK_PRIO 5
#endif

static const char *TAG = "usb_host_shared";
static bool s_installed;
static SemaphoreHandle_t s_mutex;
static portMUX_TYPE s_mutex_init_lock = portMUX_INITIALIZER_UNLOCKED;

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
    /* camera_manager_start() hands PTP init off to a task on core 1 and
     * returns immediately, while pentax_service_start() calls into the SCSI
     * backend synchronously on core 0 -- so both backends can genuinely call
     * this at the same time. A mutex (not a plain flag) is what actually
     * serializes them; the spinlock here only guards the mutex's own
     * one-time, lazy creation. */
    portENTER_CRITICAL(&s_mutex_init_lock);
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
    }
    portEXIT_CRITICAL(&s_mutex_init_lock);
    if (!s_mutex) {
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (!s_installed) {
        usb_host_config_t host_cfg = {
            .skip_phy_setup = false,
            .intr_flags = ESP_INTR_FLAG_LEVEL1,
        };
        err = usb_host_install(&host_cfg);
        if (err == ESP_OK) {
            if (xTaskCreatePinnedToCore(lib_task, "usb_lib", 4096, NULL,
                                         CONFIG_PENTAXHUB_USB_DAEMON_TASK_PRIO, NULL, 0) != pdPASS) {
                err = ESP_ERR_NO_MEM;
            } else {
                s_installed = true;
                ESP_LOGI(TAG, "USB Host Library installed");
            }
        }
    }
    xSemaphoreGive(s_mutex);
    return err;
}
