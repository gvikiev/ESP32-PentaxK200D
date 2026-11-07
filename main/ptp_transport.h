/*
 * USB host transport for PTP (PIMA15740 / ISO15740) "Still Image" class
 * devices, built on top of the ESP-IDF USB Host Library (components/usb).
 *
 * This module owns exactly one attached device at a time (the camera).
 * All of its functions -- ptp_transport_poll(), ptp_transaction(),
 * ptp_transport_wait_event() -- must be called from a single task (the
 * camera manager task in camera.c). They are not thread-safe with respect
 * to each other.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "ptp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PTP_LINK_DISCONNECTED = 0, /* nothing plugged in */
    PTP_LINK_CONNECTED,        /* a Still Image (PTP) interface was found & claimed */
    PTP_LINK_NOT_PTP,          /* a USB device is attached but exposes no Still Image interface
                                   (e.g. the camera is set to Mass Storage / "PC" mode instead
                                   of PictBridge mode) */
} ptp_link_state_t;

typedef void (*ptp_link_event_cb_t)(ptp_link_state_t state, void *ctx);

/* Installs the USB Host Library, starts its daemon task and registers a
 * client. `cb` is invoked (from the calling task's context, during a later
 * ptp_transport_poll()/ptp_transaction() call) whenever the link state
 * changes. */
esp_err_t ptp_transport_init(ptp_link_event_cb_t cb, void *cb_ctx);

/* Pumps pending USB client events (device attach/detach, transfer
 * completions). Call this repeatedly from the owning task whenever it is
 * not in the middle of a ptp_transaction(). */
void ptp_transport_poll(uint32_t timeout_ms);

ptp_link_state_t ptp_transport_get_link_state(void);

/* True if the currently connected device exposes an interrupt IN endpoint
 * (used to deliver asynchronous PTP events such as ObjectAdded). */
bool ptp_transport_has_event_endpoint(void);

/* Streaming sink for an incoming PTP data phase (e.g. GetObject). Returning
 * a value is not required; the return type is reserved for future use. */
typedef size_t (*ptp_data_in_sink_t)(const uint8_t *chunk, size_t len, void *ctx);

typedef struct {
    uint16_t opcode;
    int nparams;
    uint32_t params[PTP_MAX_PARAMS];

    const uint8_t *data_out;       /* optional: data phase to send to the device */
    size_t data_out_len;

    ptp_data_in_sink_t data_in_cb; /* optional: sink for a data phase from the device */
    void *data_in_ctx;

    uint32_t timeout_ms;           /* per USB transfer; 0 = use a sane default */
} ptp_transaction_req_t;

typedef struct {
    uint16_t resp_code;
    int nparams;
    uint32_t params[PTP_MAX_PARAMS];
    size_t data_in_total;
} ptp_transaction_resp_t;

/* Performs one full PTP bulk transaction: Command -> [Data] -> Response.
 * Must be called from the same task that owns the transport. Blocks until
 * the device replies or the transaction times out. */
esp_err_t ptp_transaction(const ptp_transaction_req_t *req, ptp_transaction_resp_t *resp);

typedef struct {
    uint16_t event_code;
    int nparams;
    uint32_t params[3];
} ptp_event_t;

/* Waits up to timeout_ms for the next asynchronous event delivered over the
 * interrupt endpoint (e.g. PTP_EC_ObjectAdded after a capture). Returns
 * false on timeout or if the device has no interrupt endpoint. */
bool ptp_transport_wait_event(ptp_event_t *out, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
