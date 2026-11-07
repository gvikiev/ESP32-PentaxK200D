#include "pentax_scsi.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "pentax_scsi";

#define STATUS_POLL_INTERVAL_MS 5u

void pentax_scsi_init(pentax_scsi_t *p, bot_t *bot, bool big_endian_args)
{
    memset(p, 0, sizeof(*p));
    p->bot = bot;
    p->big_endian_args = big_endian_args;
    p->cmd_timeout_ms = 5000;
    p->ready_timeout_ms = 10000;
}

void pentax_build_cdb(uint8_t cdb[8], uint8_t type, uint8_t b2, uint8_t b3, uint16_t w4, uint16_t w6_unused_zero)
{
    cdb[0] = PENTAX_SCSI_OPCODE;
    cdb[1] = type;
    cdb[2] = b2;
    cdb[3] = b3;
    cdb[4] = (uint8_t)w4;
    cdb[5] = (uint8_t)(w4 >> 8);
    cdb[6] = (uint8_t)w6_unused_zero;
    cdb[7] = (uint8_t)(w6_unused_zero >> 8);
}

size_t pentax_encode_args(uint8_t *out, const uint32_t *args, size_t nargs, bool big_endian)
{
    for (size_t i = 0; i < nargs; i++) {
        uint32_t v = args[i];
        uint8_t *d = out + i * 4;
        if (big_endian) {
            d[0] = (uint8_t)(v >> 24); d[1] = (uint8_t)(v >> 16); d[2] = (uint8_t)(v >> 8); d[3] = (uint8_t)v;
        } else {
            d[0] = (uint8_t)v; d[1] = (uint8_t)(v >> 8); d[2] = (uint8_t)(v >> 16); d[3] = (uint8_t)(v >> 24);
        }
    }
    return nargs * 4;
}

void pentax_parse_status(const uint8_t raw[8], pentax_status_t *out)
{
    out->result_len = (uint32_t)raw[0] | ((uint32_t)raw[1] << 8) | ((uint32_t)raw[2] << 16) | ((uint32_t)raw[3] << 24);
    out->ready = raw[6];
    out->status = raw[7];
}

esp_err_t pentax_status_to_err(uint8_t s1)
{
    switch (s1) {
    case PENTAX_ST_OK:            return ESP_OK;
    case PENTAX_ST_NOT_AVAILABLE: return ESP_ERR_NOT_SUPPORTED;
    case PENTAX_ST_ILLEGAL_PARAM: return ESP_ERR_INVALID_ARG;
    default:                      return ESP_FAIL;
    }
}

/* One no-data or small-data BOT command; maps a failed CSW to ESP_FAIL. */
static esp_err_t bot_cmd(pentax_scsi_t *p, const bot_cmd_t *c, bot_result_t *r)
{
    esp_err_t err = bot_transact(p->bot, c, r);
    if (err != ESP_OK) {
        return err;
    }
    if (r->csw_status != BOT_CSW_PASSED) {
        ESP_LOGW(TAG, "BOT command failed: CSW status=%u", r->csw_status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t send_args(pentax_scsi_t *p, const uint32_t *args, size_t nargs)
{
    uint8_t buf[PENTAX_MAX_ARGS * 4];
    size_t len = pentax_encode_args(buf, args, nargs, p->big_endian_args);
    uint8_t cdb[8];
    pentax_build_cdb(cdb, PENTAX_T_SEND_ARGS, 0, 0, 0, 0);
    /* F0 4F X0 X1 Y0 Y1: X = offset (LE16, 0), Y = size (LE16) */
    cdb[2] = 0; cdb[3] = 0;
    cdb[4] = (uint8_t)len; cdb[5] = (uint8_t)(len >> 8);
    bot_cmd_t c = {
        .cdb = cdb, .cdb_len = sizeof(cdb), .dir = BOT_DIR_OUT,
        .out = buf, .out_len = len, .timeout_ms = p->cmd_timeout_ms,
    };
    bot_result_t r;
    return bot_cmd(p, &c, &r);
}

static esp_err_t send_command(pentax_scsi_t *p, uint8_t group, uint8_t cmd, size_t nargs)
{
    uint8_t cdb[8];
    /* F0 24 C0 C1 X0 X1: X = length of previous args (unused by newer firmware) */
    pentax_build_cdb(cdb, PENTAX_T_SEND_COMMAND, group, cmd, (uint16_t)(nargs * 4), 0);
    bot_cmd_t c = { .cdb = cdb, .cdb_len = sizeof(cdb), .dir = BOT_DIR_NONE, .timeout_ms = p->cmd_timeout_ms };
    bot_result_t r;
    return bot_cmd(p, &c, &r);
}

static esp_err_t read_status(pentax_scsi_t *p, pentax_status_t *st)
{
    uint8_t cdb[8], raw[8];
    pentax_build_cdb(cdb, PENTAX_T_GET_STATUS, 0, 0, 0, 0);
    bot_cmd_t c = {
        .cdb = cdb, .cdb_len = sizeof(cdb), .dir = BOT_DIR_IN,
        .in_len = sizeof(raw), .in_buf = raw, .timeout_ms = p->cmd_timeout_ms,
    };
    bot_result_t r;
    esp_err_t err = bot_cmd(p, &c, &r);
    if (err != ESP_OK) {
        return err;
    }
    if (r.in_received != sizeof(raw)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    pentax_parse_status(raw, st);
    return ESP_OK;
}

static esp_err_t wait_ready(pentax_scsi_t *p, pentax_status_t *st)
{
    TickType_t start = xTaskGetTickCount();
    TickType_t limit = pdMS_TO_TICKS(p->ready_timeout_ms);
    while (1) {
        esp_err_t err = read_status(p, st);
        if (err != ESP_OK) {
            return err;
        }
        if (st->ready == PENTAX_ST_READY || (st->status & 0x80)) {
            return ESP_OK; /* done, or finished with an error code in S1 */
        }
        if ((xTaskGetTickCount() - start) > limit) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(STATUS_POLL_INTERVAL_MS));
    }
}

esp_err_t pentax_scsi_exec(pentax_scsi_t *p, uint8_t group, uint8_t cmd,
                           const uint32_t *args, size_t nargs,
                           uint8_t *res, size_t res_cap, size_t *res_len,
                           pentax_sink_t sink, void *sink_ctx)
{
    if (!p || !p->bot || nargs > PENTAX_MAX_ARGS) {
        return ESP_ERR_INVALID_ARG;
    }
    if (res_len) {
        *res_len = 0;
    }

    esp_err_t err;
    if (nargs > 0) {
        err = send_args(p, args, nargs);
        if (err != ESP_OK) {
            return err;
        }
    }
    err = send_command(p, group, cmd, nargs);
    if (err != ESP_OK) {
        return err;
    }

    pentax_status_t st;
    err = wait_ready(p, &st);
    if (err != ESP_OK) {
        return err;
    }
    err = pentax_status_to_err(st.status);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cmd %02x/%02x status=0x%02x", group, cmd, st.status);
        return err;
    }

    if (st.result_len == 0 || (!sink && (!res || res_cap == 0))) {
        return ESP_OK;
    }
    size_t want = st.result_len;
    if (!sink && want > res_cap) {
        want = res_cap;
    }

    uint8_t cdb[8];
    pentax_build_cdb(cdb, PENTAX_T_READ_RESULT, 0, 0, 0, 0);
    /* F0 49 X0 X1 L0 L1: X = offset (0), L = read size (LE16) */
    if (want > 0xFFFF) {
        return ESP_ERR_INVALID_SIZE; /* callers must request results in <=64 KiB pieces */
    }
    cdb[4] = (uint8_t)want; cdb[5] = (uint8_t)(want >> 8);
    bot_cmd_t c = {
        .cdb = cdb, .cdb_len = sizeof(cdb), .dir = BOT_DIR_IN, .in_len = want,
        .sink = sink, .sink_ctx = sink_ctx, .in_buf = res, .timeout_ms = p->cmd_timeout_ms,
    };
    bot_result_t r;
    err = bot_cmd(p, &c, &r);
    if (err == ESP_OK && res_len) {
        *res_len = r.in_received;
    }
    return err;
}
