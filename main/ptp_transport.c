#include "ptp_transport.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb/usb_helpers.h"

#ifndef CONFIG_PENTAXHUB_USB_BULK_BUFFER_SIZE
#define CONFIG_PENTAXHUB_USB_BULK_BUFFER_SIZE 8192
#endif
#ifndef CONFIG_PENTAXHUB_USB_DAEMON_TASK_PRIO
#define CONFIG_PENTAXHUB_USB_DAEMON_TASK_PRIO 5
#endif

#define PTP_DEFAULT_TIMEOUT_MS   5000u
#define INTR_BUF_SIZE            64u

#define PENTAX_USB_VENDOR_ID     0x0a17u /* Pentax Corporation, per USB-IF */

static const char *TAG = "ptp_transport";

/* ------------------------------- Singleton state -------------------------------- */

static usb_host_client_handle_t s_client_hdl;
static usb_device_handle_t      s_dev_hdl;
static uint8_t  s_itf_num;
static bool     s_itf_claimed;

static uint8_t  s_ep_in_addr, s_ep_out_addr, s_ep_intr_addr;

static usb_transfer_t *s_xfer_out;
static usb_transfer_t *s_xfer_in;
static usb_transfer_t *s_xfer_intr;

static volatile ptp_link_state_t s_link_state = PTP_LINK_DISCONNECTED;
static ptp_link_event_cb_t s_link_cb;
static void *s_link_cb_ctx;

static uint32_t s_next_tid;

static TaskHandle_t s_daemon_task_hdl;

/* Pushback buffer for the bulk-IN pipe: lets us read a container's 12 byte
 * header separately from its payload/trailer without losing any bytes that
 * a single USB transfer happened to also contain. */
static uint8_t s_leftover[CONFIG_PENTAXHUB_USB_BULK_BUFFER_SIZE];
static size_t  s_leftover_len;
static size_t  s_leftover_off;

static ptp_event_t s_pending_event;
static volatile bool s_pending_event_valid;

/* --------------------------------- Helpers --------------------------------------- */

static void set_link_state(ptp_link_state_t st)
{
    if (s_link_state == st) {
        return;
    }
    s_link_state = st;
    if (s_link_cb) {
        s_link_cb(st, s_link_cb_ctx);
    }
}

ptp_link_state_t ptp_transport_get_link_state(void)
{
    return s_link_state;
}

bool ptp_transport_has_event_endpoint(void)
{
    return s_ep_intr_addr != 0;
}

/* ------------------------------ Daemon task --------------------------------------- */

static void usb_lib_daemon_task(void *arg)
{
    while (1) {
        uint32_t event_flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
    }
}

/* --------------------------- Synchronous transfer wait ----------------------------- */

typedef struct {
    volatile bool done;
} xfer_sync_t;

static void sync_xfer_cb(usb_transfer_t *transfer)
{
    xfer_sync_t *s = (xfer_sync_t *)transfer->context;
    s->done = true;
}

static esp_err_t submit_and_wait(usb_transfer_t *transfer, uint32_t timeout_ms)
{
    if (timeout_ms == 0) {
        timeout_ms = PTP_DEFAULT_TIMEOUT_MS;
    }
    xfer_sync_t sync = { .done = false };
    transfer->context = &sync;
    transfer->callback = sync_xfer_cb;

    esp_err_t err = usb_host_transfer_submit(transfer);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "transfer_submit failed: %s", esp_err_to_name(err));
        return err;
    }

    TickType_t start = xTaskGetTickCount();
    TickType_t limit = pdMS_TO_TICKS(timeout_ms);
    while (!sync.done) {
        usb_host_client_handle_events(s_client_hdl, pdMS_TO_TICKS(20));
        if (sync.done) {
            break;
        }
        if ((xTaskGetTickCount() - start) > limit) {
            ESP_LOGW(TAG, "transfer timed out");
            return ESP_ERR_TIMEOUT;
        }
    }
    if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGW(TAG, "transfer status=%d", transfer->status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ------------------------- Bulk-IN byte stream (with pushback) --------------------- */

static esp_err_t fill_in_buffer(uint32_t timeout_ms)
{
    s_xfer_in->num_bytes = s_xfer_in->data_buffer_size;
    s_xfer_in->bEndpointAddress = s_ep_in_addr;
    s_xfer_in->device_handle = s_dev_hdl;
    s_xfer_in->flags = 0;
    esp_err_t err = submit_and_wait(s_xfer_in, timeout_ms);
    if (err != ESP_OK) {
        return err;
    }
    s_leftover_len = (size_t)s_xfer_in->actual_num_bytes;
    s_leftover_off = 0;
    if (s_leftover_len > 0) {
        memcpy(s_leftover, s_xfer_in->data_buffer, s_leftover_len);
    }
    return ESP_OK;
}

/* Reads exactly `want` bytes from the bulk-IN pipe. If `sink` is non-NULL the
 * bytes are streamed to it (and `out` is ignored); otherwise they are copied
 * into `out`, which must have room for `want` bytes. */
static esp_err_t stream_read(uint8_t *out, size_t want, ptp_data_in_sink_t sink, void *sink_ctx,
                              uint32_t timeout_ms)
{
    size_t got = 0;
    while (got < want) {
        if (s_leftover_off >= s_leftover_len) {
            esp_err_t err = fill_in_buffer(timeout_ms);
            if (err != ESP_OK) {
                return err;
            }
            if (s_leftover_len == 0) {
                return ESP_ERR_INVALID_RESPONSE;
            }
        }
        size_t avail = s_leftover_len - s_leftover_off;
        size_t need = want - got;
        size_t take = (avail < need) ? avail : need;
        if (sink) {
            sink(s_leftover + s_leftover_off, take, sink_ctx);
        } else if (out) {
            memcpy(out + got, s_leftover + s_leftover_off, take);
        }
        s_leftover_off += take;
        got += take;
    }
    return ESP_OK;
}

/* ------------------------------- PTP phases ---------------------------------------- */

static esp_err_t send_command_phase(const ptp_transaction_req_t *req, uint32_t tid)
{
    size_t len = ptp_build_command(s_xfer_out->data_buffer, s_xfer_out->data_buffer_size,
                                    req->opcode, tid, req->params, req->nparams);
    if (len == 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    s_xfer_out->num_bytes = len;
    s_xfer_out->bEndpointAddress = s_ep_out_addr;
    s_xfer_out->device_handle = s_dev_hdl;
    s_xfer_out->flags = USB_TRANSFER_FLAG_ZERO_PACK;
    return submit_and_wait(s_xfer_out, req->timeout_ms);
}

static esp_err_t send_data_out_phase(const ptp_transaction_req_t *req, uint32_t tid)
{
    size_t total = PTP_CONTAINER_HEADER_SIZE + req->data_out_len;
    if (total > s_xfer_out->data_buffer_size) {
        ESP_LOGE(TAG, "data-out phase too large (%u bytes)", (unsigned)total);
        return ESP_ERR_INVALID_SIZE;
    }
    ptp_build_header(s_xfer_out->data_buffer, (uint32_t)total, PTP_CONTAINER_TYPE_DATA, req->opcode, tid);
    if (req->data_out_len > 0) {
        memcpy(s_xfer_out->data_buffer + PTP_CONTAINER_HEADER_SIZE, req->data_out, req->data_out_len);
    }
    s_xfer_out->num_bytes = total;
    s_xfer_out->bEndpointAddress = s_ep_out_addr;
    s_xfer_out->device_handle = s_dev_hdl;
    s_xfer_out->flags = USB_TRANSFER_FLAG_ZERO_PACK;
    return submit_and_wait(s_xfer_out, req->timeout_ms);
}

static esp_err_t recv_data_in_phase(const ptp_transaction_req_t *req, ptp_transaction_resp_t *resp)
{
    uint8_t hdr[PTP_CONTAINER_HEADER_SIZE];
    esp_err_t err = stream_read(hdr, sizeof(hdr), NULL, NULL, req->timeout_ms);
    if (err != ESP_OK) {
        return err;
    }
    uint32_t total_len = ptp_rd32(hdr);
    uint16_t type = ptp_rd16(hdr + 4);
    if (type != PTP_CONTAINER_TYPE_DATA || total_len < PTP_CONTAINER_HEADER_SIZE) {
        ESP_LOGW(TAG, "unexpected data container (type=%u len=%u)", type, (unsigned)total_len);
        return ESP_ERR_INVALID_RESPONSE;
    }
    size_t payload = total_len - PTP_CONTAINER_HEADER_SIZE;
    if (payload > 0) {
        err = stream_read(NULL, payload, req->data_in_cb, req->data_in_ctx, req->timeout_ms);
        if (err != ESP_OK) {
            return err;
        }
    }
    resp->data_in_total = payload;
    return ESP_OK;
}

static esp_err_t recv_response_phase(ptp_transaction_resp_t *resp, uint32_t timeout_ms)
{
    uint8_t hdr[PTP_CONTAINER_HEADER_SIZE];
    esp_err_t err = stream_read(hdr, sizeof(hdr), NULL, NULL, timeout_ms);
    if (err != ESP_OK) {
        return err;
    }
    uint32_t total_len = ptp_rd32(hdr);
    uint16_t type = ptp_rd16(hdr + 4);
    uint16_t code = ptp_rd16(hdr + 6);
    if (type != PTP_CONTAINER_TYPE_RESPONSE) {
        ESP_LOGW(TAG, "expected response container, got type=%u code=0x%04x", type, code);
        return ESP_ERR_INVALID_RESPONSE;
    }
    size_t params_bytes = (total_len > PTP_CONTAINER_HEADER_SIZE) ? (total_len - PTP_CONTAINER_HEADER_SIZE) : 0;
    int nparams = (int)(params_bytes / 4u);
    if (nparams > (int)PTP_MAX_PARAMS) {
        nparams = (int)PTP_MAX_PARAMS;
    }
    uint8_t pbuf[PTP_MAX_PARAMS * 4u];
    if (nparams > 0) {
        err = stream_read(pbuf, (size_t)nparams * 4u, NULL, NULL, timeout_ms);
        if (err != ESP_OK) {
            return err;
        }
    }
    resp->resp_code = code;
    resp->nparams = nparams;
    for (int i = 0; i < nparams; i++) {
        resp->params[i] = ptp_rd32(pbuf + i * 4);
    }
    return ESP_OK;
}

esp_err_t ptp_transaction(const ptp_transaction_req_t *req, ptp_transaction_resp_t *resp)
{
    if (s_link_state != PTP_LINK_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(resp, 0, sizeof(*resp));
    /* Defensive: don't carry over desynced bytes from a previous failed transaction. */
    s_leftover_len = 0;
    s_leftover_off = 0;

    uint32_t tid = s_next_tid++;

    esp_err_t err = send_command_phase(req, tid);
    if (err != ESP_OK) {
        return err;
    }
    if (req->data_out && req->data_out_len > 0) {
        err = send_data_out_phase(req, tid);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (req->data_in_cb) {
        err = recv_data_in_phase(req, resp);
        if (err != ESP_OK) {
            return err;
        }
    }
    return recv_response_phase(resp, req->timeout_ms);
}

/* --------------------------------- Event handling ----------------------------------- */

static void intr_xfer_cb(usb_transfer_t *transfer)
{
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED &&
        transfer->actual_num_bytes >= (int)PTP_CONTAINER_HEADER_SIZE) {
        uint16_t type = ptp_rd16(transfer->data_buffer + 4);
        uint16_t code = ptp_rd16(transfer->data_buffer + 6);
        if (type == PTP_CONTAINER_TYPE_EVENT) {
            size_t n = (size_t)transfer->actual_num_bytes;
            int nparams = (int)((n - PTP_CONTAINER_HEADER_SIZE) / 4u);
            if (nparams > 3) {
                nparams = 3;
            }
            s_pending_event.event_code = code;
            s_pending_event.nparams = nparams;
            for (int i = 0; i < nparams; i++) {
                s_pending_event.params[i] = ptp_rd32(transfer->data_buffer + PTP_CONTAINER_HEADER_SIZE + i * 4);
            }
            s_pending_event_valid = true;
        }
    }
    /* Keep listening as long as we still own this device. */
    if (s_link_state == PTP_LINK_CONNECTED) {
        usb_host_transfer_submit(transfer);
    }
}

bool ptp_transport_wait_event(ptp_event_t *out, uint32_t timeout_ms)
{
    TickType_t start = xTaskGetTickCount();
    TickType_t limit = pdMS_TO_TICKS(timeout_ms);
    do {
        if (s_pending_event_valid) {
            *out = s_pending_event;
            s_pending_event_valid = false;
            return true;
        }
        usb_host_client_handle_events(s_client_hdl, pdMS_TO_TICKS(50));
    } while ((xTaskGetTickCount() - start) < limit);
    if (s_pending_event_valid) {
        *out = s_pending_event;
        s_pending_event_valid = false;
        return true;
    }
    return false;
}

/* ------------------------------ Device (dis)connection -------------------------------- */

static const usb_intf_desc_t *find_still_image_interface(const usb_config_desc_t *cfg)
{
    int offset = 0;
    const usb_standard_desc_t *cur = (const usb_standard_desc_t *)cfg;
    for (int i = 0; i < cfg->bNumInterfaces; i++) {
        cur = usb_parse_next_descriptor_of_type(cur, cfg->wTotalLength, USB_B_DESCRIPTOR_TYPE_INTERFACE, &offset);
        if (!cur) {
            break;
        }
        const usb_intf_desc_t *intf = (const usb_intf_desc_t *)cur;
        if (intf->bInterfaceClass == USB_CLASS_STILL_IMAGE) {
            return intf;
        }
    }
    return NULL;
}

static void release_device_resources(void)
{
    if (s_xfer_intr) {
        usb_host_transfer_free(s_xfer_intr);
        s_xfer_intr = NULL;
    }
    if (s_xfer_in) {
        usb_host_transfer_free(s_xfer_in);
        s_xfer_in = NULL;
    }
    if (s_xfer_out) {
        usb_host_transfer_free(s_xfer_out);
        s_xfer_out = NULL;
    }
    if (s_itf_claimed) {
        usb_host_interface_release(s_client_hdl, s_dev_hdl, s_itf_num);
        s_itf_claimed = false;
    }
    if (s_dev_hdl) {
        usb_host_device_close(s_client_hdl, s_dev_hdl);
        s_dev_hdl = NULL;
    }
    s_ep_in_addr = s_ep_out_addr = s_ep_intr_addr = 0;
    s_leftover_len = s_leftover_off = 0;
    s_pending_event_valid = false;
    s_next_tid = 0;
}

static void try_open_device(uint8_t address)
{
    usb_device_handle_t dev_hdl;
    esp_err_t err = usb_host_device_open(s_client_hdl, address, &dev_hdl);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "device_open(%u) failed: %s", address, esp_err_to_name(err));
        return;
    }

    const usb_device_desc_t *dev_desc = NULL;
    usb_host_get_device_descriptor(dev_hdl, &dev_desc);
    const usb_config_desc_t *cfg_desc = NULL;
    err = usb_host_get_active_config_descriptor(dev_hdl, &cfg_desc);
    if (err != ESP_OK || !cfg_desc) {
        ESP_LOGW(TAG, "could not read config descriptor: %s", esp_err_to_name(err));
        usb_host_device_close(s_client_hdl, dev_hdl);
        return;
    }

    if (dev_desc) {
        ESP_LOGI(TAG, "USB device attached: VID=0x%04x PID=0x%04x",
                 dev_desc->idVendor, dev_desc->idProduct);
    }

    const usb_intf_desc_t *intf = find_still_image_interface(cfg_desc);
    s_dev_hdl = dev_hdl;
    if (!intf) {
        if (!dev_desc || dev_desc->idVendor != PENTAX_USB_VENDOR_ID) {
            ESP_LOGW(TAG, "attached device is not a Pentax camera (VID=0x%04x, expected 0x%04x) - "
                          "check that the K200D (not some other device) is plugged into the "
                          "USB-host connector, and that it is powered on",
                          dev_desc ? dev_desc->idVendor : 0, PENTAX_USB_VENDOR_ID);
        } else {
            ESP_LOGW(TAG, "no USB Still Image (PTP) interface found on this device - "
                          "on the K200D, set [USB Connection] to [PictBridge] (not [PC]) in the camera menu");
        }
        set_link_state(PTP_LINK_NOT_PTP);
        return;
    }

    err = usb_host_interface_claim(s_client_hdl, dev_hdl, intf->bInterfaceNumber, intf->bAlternateSetting);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "interface_claim failed: %s", esp_err_to_name(err));
        usb_host_device_close(s_client_hdl, dev_hdl);
        s_dev_hdl = NULL;
        return;
    }
    s_itf_num = intf->bInterfaceNumber;
    s_itf_claimed = true;

    int offset = 0;
    uint8_t ep_in = 0, ep_out = 0, ep_intr = 0;
    for (int i = 0; i < intf->bNumEndpoints; i++) {
        const usb_ep_desc_t *ep = usb_parse_endpoint_descriptor_by_index(intf, i, cfg_desc->wTotalLength, &offset);
        if (!ep) {
            break;
        }
        usb_transfer_type_t xfer_type = USB_EP_DESC_GET_XFERTYPE(ep);
        bool is_in = USB_EP_DESC_GET_EP_DIR(ep) != 0;
        if (xfer_type == USB_TRANSFER_TYPE_BULK && is_in) {
            ep_in = ep->bEndpointAddress;
        } else if (xfer_type == USB_TRANSFER_TYPE_BULK && !is_in) {
            ep_out = ep->bEndpointAddress;
        } else if (xfer_type == USB_TRANSFER_TYPE_INTR && is_in) {
            ep_intr = ep->bEndpointAddress;
        }
    }

    if (!ep_in || !ep_out) {
        ESP_LOGE(TAG, "Still Image interface is missing bulk IN/OUT endpoints");
        usb_host_interface_release(s_client_hdl, dev_hdl, s_itf_num);
        s_itf_claimed = false;
        /* Keep the device open (s_dev_hdl stays set) so a later physical
         * disconnect still reaches handle_device_gone() and resets our
         * state back to DISCONNECTED; see the find_still_image_interface()
         * failure path above for the same reasoning. */
        set_link_state(PTP_LINK_NOT_PTP);
        return;
    }
    s_ep_in_addr = ep_in;
    s_ep_out_addr = ep_out;
    s_ep_intr_addr = ep_intr;

    ESP_ERROR_CHECK(usb_host_transfer_alloc(CONFIG_PENTAXHUB_USB_BULK_BUFFER_SIZE, 0, &s_xfer_out));
    ESP_ERROR_CHECK(usb_host_transfer_alloc(CONFIG_PENTAXHUB_USB_BULK_BUFFER_SIZE, 0, &s_xfer_in));

    if (ep_intr) {
        ESP_ERROR_CHECK(usb_host_transfer_alloc(INTR_BUF_SIZE, 0, &s_xfer_intr));
        s_xfer_intr->device_handle = dev_hdl;
        s_xfer_intr->bEndpointAddress = ep_intr;
        s_xfer_intr->num_bytes = INTR_BUF_SIZE;
        s_xfer_intr->callback = intr_xfer_cb;
        s_xfer_intr->context = NULL;
        esp_err_t ierr = usb_host_transfer_submit(s_xfer_intr);
        if (ierr != ESP_OK) {
            ESP_LOGW(TAG, "could not submit interrupt-IN listener: %s", esp_err_to_name(ierr));
        }
    } else {
        ESP_LOGW(TAG, "device has no interrupt endpoint - capture-complete events will be polled instead");
    }

    s_next_tid = 0; /* ISO15740: the first command's Transaction ID in a session must be 0 (OpenSession) */
    ESP_LOGI(TAG, "PTP Still Image interface #%u claimed (EP IN=0x%02x OUT=0x%02x INTR=0x%02x)",
              s_itf_num, ep_in, ep_out, ep_intr);
    set_link_state(PTP_LINK_CONNECTED);
}

static void handle_device_gone(usb_device_handle_t dev_hdl)
{
    if (dev_hdl != s_dev_hdl) {
        return;
    }
    ESP_LOGI(TAG, "camera disconnected");
    release_device_resources();
    set_link_state(PTP_LINK_DISCONNECTED);
}

static void client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        if (s_dev_hdl == NULL) {
            try_open_device(event_msg->new_dev.address);
        } else {
            ESP_LOGW(TAG, "ignoring additional USB device; only one camera is supported at a time");
        }
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        handle_device_gone(event_msg->dev_gone.dev_hdl);
        break;
    default:
        break;
    }
}

/* ------------------------------------ Init -------------------------------------------- */

esp_err_t ptp_transport_init(ptp_link_event_cb_t cb, void *cb_ctx)
{
    s_link_cb = cb;
    s_link_cb_ctx = cb_ctx;

    usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        return err;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(usb_lib_daemon_task, "usb_daemon", 4096, NULL,
                                             CONFIG_PENTAXHUB_USB_DAEMON_TASK_PRIO, &s_daemon_task_hdl, 0);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = NULL,
        },
    };
    return usb_host_client_register(&client_config, &s_client_hdl);
}

void ptp_transport_poll(uint32_t timeout_ms)
{
    usb_host_client_handle_events(s_client_hdl, pdMS_TO_TICKS(timeout_ms));
}
