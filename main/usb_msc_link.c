#include "usb_msc_link.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb/usb_helpers.h"

#ifndef CONFIG_PENTAXHUB_USB_DAEMON_TASK_PRIO
#define CONFIG_PENTAXHUB_USB_DAEMON_TASK_PRIO 5
#endif
#ifndef CONFIG_PENTAXHUB_USB_BULK_BUFFER_SIZE
#define CONFIG_PENTAXHUB_USB_BULK_BUFFER_SIZE 8192
#endif

#define MSC_CLASS      0x08
#define MSC_SUBCLASS   0x06  /* SCSI transparent */
#define MSC_PROTO_BOT  0x50

static const char *TAG = "usb_msc_link";

static usb_host_client_handle_t s_client;
static usb_device_handle_t s_dev;
static uint8_t s_itf;
static bool s_itf_claimed;
static uint8_t s_ep_in, s_ep_out;
static uint16_t s_mps_in = 64, s_mps_out = 64;

static usb_transfer_t *s_xfer_out;
static usb_transfer_t *s_xfer_in;
static usb_transfer_t *s_xfer_ctrl;
static SemaphoreHandle_t s_done_sem;

static volatile usb_msc_state_t s_state = USB_MSC_DISCONNECTED;
static usb_msc_event_cb_t s_cb;
static void *s_cb_ctx;

static void set_state(usb_msc_state_t st)
{
    if (s_state == st) {
        return;
    }
    s_state = st;
    if (s_cb) {
        s_cb(st, s_cb_ctx);
    }
}

usb_msc_state_t usb_msc_link_state(void)
{
    return s_state;
}

/* ------------------------------ synchronous transfers ----------------------------- */

static void xfer_done_cb(usb_transfer_t *t)
{
    (void)t; /* runs in the usb_cli task context (usb_host_client_handle_events) */
    xSemaphoreGive(s_done_sem);
}

/* Submits and waits. On timeout the endpoint is halted/flushed/cleared so the
 * transfer is cancelled and its callback fires before we return. */
static esp_err_t submit_wait(usb_transfer_t *t, uint32_t timeout_ms)
{
    xSemaphoreTake(s_done_sem, 0); /* drop any stale token */
    t->callback = xfer_done_cb;
    t->context = NULL;
    esp_err_t err = usb_host_transfer_submit(t);
    if (err != ESP_OK) {
        return err;
    }
    if (xSemaphoreTake(s_done_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        uint8_t ep = t->bEndpointAddress;
        usb_host_endpoint_halt(s_dev, ep);
        usb_host_endpoint_flush(s_dev, ep);
        xSemaphoreTake(s_done_sem, pdMS_TO_TICKS(1000)); /* canceled callback */
        usb_host_endpoint_clear(s_dev, ep);
        return ESP_ERR_TIMEOUT;
    }
    switch (t->status) {
    case USB_TRANSFER_STATUS_COMPLETED: return ESP_OK;
    case USB_TRANSFER_STATUS_STALL:     return BOT_IO_STALL;
    case USB_TRANSFER_STATUS_NO_DEVICE: return ESP_ERR_NOT_FOUND;
    default:                            return ESP_FAIL;
    }
}

static esp_err_t io_bulk_out(void *ctx, const uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    (void)ctx;
    if (!s_xfer_out) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t off = 0;
    while (off < len) {
        size_t n = len - off;
        if (n > s_xfer_out->data_buffer_size) {
            n = s_xfer_out->data_buffer_size;
        }
        memcpy(s_xfer_out->data_buffer, buf + off, n);
        s_xfer_out->num_bytes = (int)n;
        s_xfer_out->bEndpointAddress = s_ep_out;
        s_xfer_out->device_handle = s_dev;
        /* Terminate with a zero-length packet if the last chunk is a multiple of the MPS. */
        s_xfer_out->flags = (off + n == len) ? USB_TRANSFER_FLAG_ZERO_PACK : 0;
        esp_err_t err = submit_wait(s_xfer_out, timeout_ms);
        if (err != ESP_OK) {
            return err;
        }
        off += n;
    }
    return ESP_OK;
}

static esp_err_t io_bulk_in(void *ctx, uint8_t *buf, size_t max_len, size_t *actual, uint32_t timeout_ms)
{
    (void)ctx;
    if (!s_xfer_in) {
        return ESP_ERR_INVALID_STATE;
    }
    if (max_len > s_xfer_in->data_buffer_size) {
        max_len = s_xfer_in->data_buffer_size;
    }
    s_xfer_in->num_bytes = (int)max_len;
    s_xfer_in->bEndpointAddress = s_ep_in;
    s_xfer_in->device_handle = s_dev;
    s_xfer_in->flags = 0;
    esp_err_t err = submit_wait(s_xfer_in, timeout_ms);
    if (err != ESP_OK) {
        return err;
    }
    *actual = (size_t)s_xfer_in->actual_num_bytes;
    memcpy(buf, s_xfer_in->data_buffer, *actual);
    return ESP_OK;
}

static esp_err_t io_clear_halt(void *ctx, bool in_endpoint)
{
    (void)ctx;
    return usb_host_endpoint_clear(s_dev, in_endpoint ? s_ep_in : s_ep_out);
}

static esp_err_t io_reset_recovery(void *ctx)
{
    (void)ctx;
    if (!s_xfer_ctrl) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Bulk-Only Mass Storage Reset: class request, interface recipient, bRequest 0xFF. */
    usb_setup_packet_t *setup = (usb_setup_packet_t *)s_xfer_ctrl->data_buffer;
    setup->bmRequestType = 0x21;
    setup->bRequest = 0xFF;
    setup->wValue = 0;
    setup->wIndex = s_itf;
    setup->wLength = 0;
    s_xfer_ctrl->num_bytes = sizeof(usb_setup_packet_t);
    s_xfer_ctrl->bEndpointAddress = 0;
    s_xfer_ctrl->device_handle = s_dev;
    s_xfer_ctrl->flags = 0;
    esp_err_t err = submit_wait(s_xfer_ctrl, 2000);
    usb_host_endpoint_clear(s_dev, s_ep_in);
    usb_host_endpoint_clear(s_dev, s_ep_out);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "BOT reset recovery failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t usb_msc_link_get_io(bot_io_t *io)
{
    if (s_state != USB_MSC_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(io, 0, sizeof(*io));
    io->mps_in = s_mps_in;
    io->bulk_out = io_bulk_out;
    io->bulk_in = io_bulk_in;
    io->clear_halt = io_clear_halt;
    io->reset_recovery = io_reset_recovery;
    return ESP_OK;
}

/* ------------------------------- enumeration ----------------------------------- */

static const usb_intf_desc_t *find_bot_interface(const usb_config_desc_t *cfg)
{
    int offset = 0;
    const usb_standard_desc_t *cur = (const usb_standard_desc_t *)cfg;
    for (int i = 0; i < cfg->bNumInterfaces; i++) {
        cur = usb_parse_next_descriptor_of_type(cur, cfg->wTotalLength, USB_B_DESCRIPTOR_TYPE_INTERFACE, &offset);
        if (!cur) {
            break;
        }
        const usb_intf_desc_t *intf = (const usb_intf_desc_t *)cur;
        if (intf->bInterfaceClass == MSC_CLASS && intf->bInterfaceSubClass == MSC_SUBCLASS &&
            intf->bInterfaceProtocol == MSC_PROTO_BOT) {
            return intf;
        }
    }
    return NULL;
}

static void release_device(void)
{
    if (s_xfer_ctrl) { usb_host_transfer_free(s_xfer_ctrl); s_xfer_ctrl = NULL; }
    if (s_xfer_in)   { usb_host_transfer_free(s_xfer_in);   s_xfer_in = NULL; }
    if (s_xfer_out)  { usb_host_transfer_free(s_xfer_out);  s_xfer_out = NULL; }
    if (s_itf_claimed) {
        usb_host_interface_release(s_client, s_dev, s_itf);
        s_itf_claimed = false;
    }
    if (s_dev) {
        usb_host_device_close(s_client, s_dev);
        s_dev = NULL;
    }
    s_ep_in = s_ep_out = 0;
}

static void open_device(uint8_t address)
{
    usb_device_handle_t dev;
    if (usb_host_device_open(s_client, address, &dev) != ESP_OK) {
        return;
    }
    const usb_device_desc_t *dd = NULL;
    usb_host_get_device_descriptor(dev, &dd);
    const usb_config_desc_t *cfg = NULL;
    if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK || !cfg) {
        usb_host_device_close(s_client, dev);
        return;
    }
    if (dd) {
        ESP_LOGI(TAG, "USB device: VID=0x%04x PID=0x%04x", dd->idVendor, dd->idProduct);
    }
    s_dev = dev; /* keep open so DEV_GONE resets us even for unsupported devices */

    const usb_intf_desc_t *intf = find_bot_interface(cfg);
    if (!intf) {
        ESP_LOGW(TAG, "no Mass Storage BOT interface; set the camera USB mode to [PC]");
        set_state(USB_MSC_NOT_MSC);
        return;
    }
    if (usb_host_interface_claim(s_client, dev, intf->bInterfaceNumber, intf->bAlternateSetting) != ESP_OK) {
        ESP_LOGE(TAG, "interface claim failed");
        release_device();
        return;
    }
    s_itf = intf->bInterfaceNumber;
    s_itf_claimed = true;

    int offset = 0;
    uint8_t ep_in = 0, ep_out = 0;
    for (int i = 0; i < intf->bNumEndpoints; i++) {
        const usb_ep_desc_t *ep = usb_parse_endpoint_descriptor_by_index(intf, i, cfg->wTotalLength, &offset);
        if (!ep || USB_EP_DESC_GET_XFERTYPE(ep) != USB_TRANSFER_TYPE_BULK) {
            continue;
        }
        if (USB_EP_DESC_GET_EP_DIR(ep)) {
            ep_in = ep->bEndpointAddress;
            s_mps_in = USB_EP_DESC_GET_MPS(ep);
        } else {
            ep_out = ep->bEndpointAddress;
            s_mps_out = USB_EP_DESC_GET_MPS(ep);
        }
    }
    if (!ep_in || !ep_out) {
        ESP_LOGE(TAG, "BOT interface lacks bulk endpoints");
        usb_host_interface_release(s_client, dev, s_itf);
        s_itf_claimed = false;
        set_state(USB_MSC_NOT_MSC);
        return;
    }
    s_ep_in = ep_in;
    s_ep_out = ep_out;

    /* IN buffer must be a multiple of the IN MPS. */
    size_t in_sz = (CONFIG_PENTAXHUB_USB_BULK_BUFFER_SIZE / s_mps_in) * s_mps_in;
    if (usb_host_transfer_alloc(CONFIG_PENTAXHUB_USB_BULK_BUFFER_SIZE, 0, &s_xfer_out) != ESP_OK ||
        usb_host_transfer_alloc(in_sz, 0, &s_xfer_in) != ESP_OK ||
        usb_host_transfer_alloc(sizeof(usb_setup_packet_t), 0, &s_xfer_ctrl) != ESP_OK) {
        ESP_LOGE(TAG, "transfer alloc failed");
        release_device();
        return;
    }
    ESP_LOGI(TAG, "BOT interface #%u claimed (IN=0x%02x/%u OUT=0x%02x/%u)",
             s_itf, s_ep_in, s_mps_in, s_ep_out, s_mps_out);
    set_state(USB_MSC_CONNECTED);
}

static void client_event_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    (void)arg;
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        if (!s_dev) {
            open_device(msg->new_dev.address);
        }
    } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        if (msg->dev_gone.dev_hdl == s_dev) {
            ESP_LOGI(TAG, "device removed");
            set_state(USB_MSC_DISCONNECTED);
            release_device();
        }
    }
}

static void lib_task(void *arg)
{
    (void)arg;
    while (1) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
    }
}

static void client_task(void *arg)
{
    (void)arg;
    while (1) {
        usb_host_client_handle_events(s_client, portMAX_DELAY);
    }
}

esp_err_t usb_msc_link_init(usb_msc_event_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_cb_ctx = ctx;
    s_done_sem = xSemaphoreCreateBinary();
    if (!s_done_sem) {
        return ESP_ERR_NO_MEM;
    }
    usb_host_config_t host_cfg = { .skip_phy_setup = false, .intr_flags = ESP_INTR_FLAG_LEVEL1 };
    esp_err_t err = usb_host_install(&host_cfg);
    if (err != ESP_OK) {
        return err;
    }
    usb_host_client_config_t cc = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = { .client_event_callback = client_event_cb, .callback_arg = NULL },
    };
    err = usb_host_client_register(&cc, &s_client);
    if (err != ESP_OK) {
        return err;
    }
    if (xTaskCreatePinnedToCore(lib_task, "usb_lib", 4096, NULL, CONFIG_PENTAXHUB_USB_DAEMON_TASK_PRIO, NULL, 0) != pdPASS ||
        xTaskCreatePinnedToCore(client_task, "usb_cli", 6144, NULL, CONFIG_PENTAXHUB_USB_DAEMON_TASK_PRIO + 1, NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
