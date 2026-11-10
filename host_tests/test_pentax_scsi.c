/*
 * Pure-helper tests for pentax_scsi.c, plus integration tests that run
 * pentax_scsi_exec() all the way through the real bot.c against a scripted
 * mock bot_io_t -- including a simulated camera that stays "busy" forever,
 * to exercise the ready-timeout path deterministically via the fake clock
 * (no real 10-second wait).
 *
 * The mock auto-fills each scripted CSW's tag from the most recently seen
 * CBW (bot_transact's tag counter is internal, so a hand-picked tag would
 * have to be kept in lockstep with every transaction the test triggers --
 * fragile, and outright wrong once a step is replayed via the loop point).
 */
#include "tinytest.h"
#include "pentax_scsi.h"
#include "fake_clock.h"

#include <string.h>

/* ---------------------------- scripted mock bot_io_t ---------------------------- */

#define MOCK_MAX_IN_STEPS 32
#define MOCK_MAX_OUT_LOG  16

typedef struct {
    uint8_t data[512];
    size_t len;
    esp_err_t ret;
    bool autotag_csw; /* true: patch bytes [4..7] with the last CBW's tag before serving */
} mock_in_step_t;

static mock_in_step_t s_in_steps[MOCK_MAX_IN_STEPS];
static size_t s_in_step_count;
static size_t s_in_step_idx;
static size_t s_in_loop_point = (size_t)-1;

static uint8_t s_out_log[MOCK_MAX_OUT_LOG][512];
static size_t s_out_log_len[MOCK_MAX_OUT_LOG];
static size_t s_out_call_count;
static uint32_t s_last_cbw_tag;

static void mock_reset(void)
{
    s_in_step_count = 0;
    s_in_step_idx = 0;
    s_in_loop_point = (size_t)-1;
    s_out_call_count = 0;
    memset(s_out_log_len, 0, sizeof(s_out_log_len));
    s_last_cbw_tag = 0;
    fake_clock_reset();
}

static void mock_push_in_ex(const uint8_t *data, size_t len, esp_err_t ret, bool autotag_csw)
{
    mock_in_step_t *s = &s_in_steps[s_in_step_count++];
    if (data && len) {
        memcpy(s->data, data, len);
    }
    s->len = len;
    s->ret = ret;
    s->autotag_csw = autotag_csw;
}

static void mock_set_loop_point(size_t idx)
{
    s_in_loop_point = idx;
}

static esp_err_t mock_bulk_out(void *ctx, const uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    (void)ctx; (void)timeout_ms;
    size_t idx = s_out_call_count;
    if (idx < MOCK_MAX_OUT_LOG) {
        memcpy(s_out_log[idx], buf, len);
        s_out_log_len[idx] = len;
    }
    s_out_call_count++;
    if (len == BOT_CBW_SIZE && memcmp(buf, "USBC", 4) == 0) {
        s_last_cbw_tag = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8) |
                         ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);
    }
    return ESP_OK;
}

static esp_err_t mock_bulk_in(void *ctx, uint8_t *buf, size_t max_len, size_t *actual, uint32_t timeout_ms)
{
    (void)ctx; (void)max_len; (void)timeout_ms;
    if (s_in_step_idx >= s_in_step_count) {
        if (s_in_loop_point != (size_t)-1 && s_in_loop_point < s_in_step_count) {
            s_in_step_idx = s_in_loop_point;
        } else {
            *actual = 0;
            return ESP_FAIL; /* test bug: ran off the end of the script */
        }
    }
    mock_in_step_t *s = &s_in_steps[s_in_step_idx++];
    if (s->ret == ESP_OK) {
        memcpy(buf, s->data, s->len);
        if (s->autotag_csw && s->len >= 8) {
            buf[4] = (uint8_t)s_last_cbw_tag;
            buf[5] = (uint8_t)(s_last_cbw_tag >> 8);
            buf[6] = (uint8_t)(s_last_cbw_tag >> 16);
            buf[7] = (uint8_t)(s_last_cbw_tag >> 24);
        }
        *actual = s->len;
    } else {
        *actual = 0;
    }
    return s->ret;
}

static esp_err_t mock_noop(void *ctx, bool in_endpoint) { (void)ctx; (void)in_endpoint; return ESP_OK; }
static esp_err_t mock_noop2(void *ctx) { (void)ctx; return ESP_OK; }

static bot_io_t make_io(void)
{
    bot_io_t io = {
        .ctx = NULL, .mps_in = 64,
        .bulk_out = mock_bulk_out, .bulk_in = mock_bulk_in,
        .clear_halt = mock_noop, .reset_recovery = mock_noop2,
    };
    return io;
}

/* 8-byte "get status" reply: result_len (LE32), 2 unused bytes, ready, status. */
static void build_status_raw(uint8_t out[8], uint32_t result_len, uint8_t ready, uint8_t status)
{
    out[0] = (uint8_t)result_len; out[1] = (uint8_t)(result_len >> 8);
    out[2] = (uint8_t)(result_len >> 16); out[3] = (uint8_t)(result_len >> 24);
    out[4] = 0; out[5] = 0;
    out[6] = ready;
    out[7] = status;
}

static void build_csw_notag(uint8_t out[BOT_CSW_SIZE], uint8_t status)
{
    out[0] = 'U'; out[1] = 'S'; out[2] = 'B'; out[3] = 'S';
    out[4] = out[5] = out[6] = out[7] = 0; /* patched at serve time */
    out[8] = out[9] = out[10] = out[11] = 0;
    out[12] = status;
}

/* Scripts one "no data" transaction (SEND_ARGS/SEND_COMMAND): just a CSW. */
static void script_none_or_out(void)
{
    uint8_t csw[BOT_CSW_SIZE];
    build_csw_notag(csw, BOT_CSW_PASSED);
    mock_push_in_ex(csw, sizeof(csw), ESP_OK, true);
}

/* Scripts one "read" transaction (GET_STATUS/READ_RESULT): data then CSW. */
static void script_in(const uint8_t *data, size_t len)
{
    mock_push_in_ex(data, len, ESP_OK, false);
    uint8_t csw[BOT_CSW_SIZE];
    build_csw_notag(csw, BOT_CSW_PASSED);
    mock_push_in_ex(csw, sizeof(csw), ESP_OK, true);
}

/* ---------------------------------- pure helpers ---------------------------------- */

static void test_pentax_build_cdb_layout(void)
{
    uint8_t cdb[8];
    pentax_build_cdb(cdb, PENTAX_T_SEND_COMMAND, 0x18, 0x05, 0x0102, 0x0304);
    CHECK_EQ(cdb[0], PENTAX_SCSI_OPCODE);
    CHECK_EQ(cdb[1], PENTAX_T_SEND_COMMAND);
    CHECK_EQ(cdb[2], 0x18);
    CHECK_EQ(cdb[3], 0x05);
    CHECK_EQ(cdb[4], 0x02); CHECK_EQ(cdb[5], 0x01); /* w4 little-endian */
    CHECK_EQ(cdb[6], 0x04); CHECK_EQ(cdb[7], 0x03); /* w6 little-endian */
}

static void test_pentax_encode_args(void)
{
    uint32_t args[3] = {0x01020304u, 0x0u, 0xFFFFFFFFu};
    uint8_t out[12];

    CHECK_EQ(pentax_encode_args(out, args, 3, true), 12);
    CHECK_EQ(out[0], 0x01); CHECK_EQ(out[1], 0x02); CHECK_EQ(out[2], 0x03); CHECK_EQ(out[3], 0x04);
    CHECK_EQ(out[4], 0); CHECK_EQ(out[5], 0); CHECK_EQ(out[6], 0); CHECK_EQ(out[7], 0);
    CHECK_EQ(out[8], 0xFF); CHECK_EQ(out[9], 0xFF); CHECK_EQ(out[10], 0xFF); CHECK_EQ(out[11], 0xFF);

    CHECK_EQ(pentax_encode_args(out, args, 1, false), 4);
    CHECK_EQ(out[0], 0x04); CHECK_EQ(out[1], 0x03); CHECK_EQ(out[2], 0x02); CHECK_EQ(out[3], 0x01);

    CHECK_EQ(pentax_encode_args(out, args, 0, true), 0);
}

static void test_pentax_parse_status_and_fault_injection(void)
{
    uint8_t raw[8];
    build_status_raw(raw, 0x11223344, PENTAX_ST_READY, PENTAX_ST_OK);
    pentax_status_t st;
    pentax_parse_status(raw, &st);
    CHECK_EQ(st.result_len, 0x11223344u);
    CHECK_EQ(st.ready, PENTAX_ST_READY);
    CHECK_EQ(st.status, PENTAX_ST_OK);

    uint8_t zero[8]; memset(zero, 0, sizeof(zero));
    pentax_parse_status(zero, &st);
    CHECK_EQ(st.result_len, 0);
    CHECK_EQ(st.ready, 0);
    CHECK_EQ(st.status, 0);

    uint8_t allff[8]; memset(allff, 0xFF, sizeof(allff));
    pentax_parse_status(allff, &st);
    CHECK_EQ(st.result_len, 0xFFFFFFFFu);
    CHECK_EQ(st.ready, 0xFF);
    CHECK_EQ(st.status, 0xFF);
}

static void test_pentax_status_to_err_exhaustive(void)
{
    for (int s1 = 0; s1 <= 0xFF; s1++) {
        esp_err_t err = pentax_status_to_err((uint8_t)s1);
        esp_err_t expect;
        switch (s1) {
        case PENTAX_ST_OK:            expect = ESP_OK; break;
        case PENTAX_ST_NOT_AVAILABLE: expect = ESP_ERR_NOT_SUPPORTED; break;
        case PENTAX_ST_ILLEGAL_PARAM: expect = ESP_ERR_INVALID_ARG; break;
        default:                      expect = ESP_FAIL; break;
        }
        CHECK_EQ(err, expect);
    }
}

/* ------------------------------- pentax_scsi_exec: integration ------------------------------- */

static void test_exec_argument_validation(void)
{
    mock_reset();
    bot_t bot; uint8_t work[128];
    bot_io_t io = make_io();
    CHECK(bot_init(&bot, &io, work, sizeof(work), 0) == ESP_OK);
    pentax_scsi_t sc; pentax_scsi_init(&sc, &bot, true);

    size_t res_len;
    CHECK(pentax_scsi_exec(NULL, 0x10, 0x01, NULL, 0, NULL, 0, &res_len, NULL, NULL) == ESP_ERR_INVALID_ARG);

    uint32_t too_many[PENTAX_MAX_ARGS + 1] = {0};
    CHECK(pentax_scsi_exec(&sc, 0x10, 0x01, too_many, PENTAX_MAX_ARGS + 1, NULL, 0, &res_len, NULL, NULL)
          == ESP_ERR_INVALID_ARG);
    CHECK_EQ(s_out_call_count, 0); /* rejected before any IO */
}

static void test_exec_happy_path_with_args_and_result(void)
{
    mock_reset();
    bot_t bot; uint8_t work[512];
    bot_io_t io = make_io();
    CHECK(bot_init(&bot, &io, work, sizeof(work), 0) == ESP_OK);
    pentax_scsi_t sc; pentax_scsi_init(&sc, &bot, true);

    uint32_t args[2] = {1, 2};
    script_none_or_out(); /* SEND_ARGS CSW */
    script_none_or_out(); /* SEND_COMMAND CSW */

    uint8_t result_bytes[16];
    for (size_t i = 0; i < sizeof(result_bytes); i++) {
        result_bytes[i] = (uint8_t)(0x50 + i);
    }
    uint8_t status_raw[8];
    build_status_raw(status_raw, sizeof(result_bytes), PENTAX_ST_READY, PENTAX_ST_OK);
    script_in(status_raw, sizeof(status_raw)); /* GET_STATUS */
    script_in(result_bytes, sizeof(result_bytes)); /* READ_RESULT */

    uint8_t res[16];
    size_t res_len = 0;
    CHECK(pentax_scsi_exec(&sc, 0x18, 0x05, args, 2, res, sizeof(res), &res_len, NULL, NULL) == ESP_OK);
    CHECK_EQ(res_len, sizeof(result_bytes));
    CHECK(memcmp(res, result_bytes, sizeof(result_bytes)) == 0);
}

static void test_exec_polls_until_ready(void)
{
    mock_reset();
    bot_t bot; uint8_t work[512];
    bot_io_t io = make_io();
    CHECK(bot_init(&bot, &io, work, sizeof(work), 0) == ESP_OK);
    pentax_scsi_t sc; pentax_scsi_init(&sc, &bot, true);

    script_none_or_out(); /* SEND_COMMAND CSW (no args) */

    uint8_t busy_raw[8];
    build_status_raw(busy_raw, 0, 0 /* not ready */, 0x00);
    script_in(busy_raw, sizeof(busy_raw)); /* poll #1: still busy */
    script_in(busy_raw, sizeof(busy_raw)); /* poll #2: still busy */
    uint8_t ready_raw[8];
    build_status_raw(ready_raw, 0, PENTAX_ST_READY, PENTAX_ST_OK);
    script_in(ready_raw, sizeof(ready_raw)); /* poll #3: ready, no result */

    uint8_t res[4]; size_t res_len = 0xDEAD;
    CHECK(pentax_scsi_exec(&sc, 0x10, 0x00, NULL, 0, res, sizeof(res), &res_len, NULL, NULL) == ESP_OK);
    CHECK_EQ(res_len, 0); /* result_len was 0: no READ_RESULT issued */
}

static void test_exec_ready_timeout(void)
{
    mock_reset();
    bot_t bot; uint8_t work[512];
    bot_io_t io = make_io();
    CHECK(bot_init(&bot, &io, work, sizeof(work), 0) == ESP_OK);
    pentax_scsi_t sc; pentax_scsi_init(&sc, &bot, true);
    sc.ready_timeout_ms = 20; /* small on purpose: the fake clock still makes this instant */

    script_none_or_out(); /* SEND_COMMAND CSW */

    uint8_t busy_raw[8];
    build_status_raw(busy_raw, 0, 0 /* never becomes ready */, 0x00);
    mock_set_loop_point(s_in_step_count);
    script_in(busy_raw, sizeof(busy_raw)); /* this single poll result repeats forever */

    uint8_t res[4]; size_t res_len = 0;
    CHECK(pentax_scsi_exec(&sc, 0x10, 0x00, NULL, 0, res, sizeof(res), &res_len, NULL, NULL) == ESP_ERR_TIMEOUT);
}

static void test_exec_camera_rejects_command(void)
{
    mock_reset();
    bot_t bot; uint8_t work[512];
    bot_io_t io = make_io();
    CHECK(bot_init(&bot, &io, work, sizeof(work), 0) == ESP_OK);
    pentax_scsi_t sc; pentax_scsi_init(&sc, &bot, true);

    script_none_or_out(); /* SEND_COMMAND CSW */

    uint8_t rejected_raw[8];
    /* status&0x80 set -> wait_ready stops polling even though "ready" bit is 0 */
    build_status_raw(rejected_raw, 123, 0, PENTAX_ST_ILLEGAL_PARAM);
    script_in(rejected_raw, sizeof(rejected_raw));

    uint8_t res[4]; size_t res_len = 0;
    CHECK(pentax_scsi_exec(&sc, 0x18, 0x20, NULL, 0, res, sizeof(res), &res_len, NULL, NULL)
          == ESP_ERR_INVALID_ARG);
    CHECK_EQ(res_len, 0);
    /* must not have attempted READ_RESULT despite a nonzero result_len */
    CHECK_EQ(s_in_step_idx, s_in_step_count);
}

static void test_exec_truncates_result_to_caller_capacity(void)
{
    mock_reset();
    bot_t bot; uint8_t work[512];
    bot_io_t io = make_io();
    CHECK(bot_init(&bot, &io, work, sizeof(work), 0) == ESP_OK);
    pentax_scsi_t sc; pentax_scsi_init(&sc, &bot, true);

    script_none_or_out(); /* SEND_COMMAND CSW */
    uint8_t status_raw[8];
    build_status_raw(status_raw, 100 /* camera has 100 bytes available */, PENTAX_ST_READY, PENTAX_ST_OK);
    script_in(status_raw, sizeof(status_raw));
    uint8_t camera_data[100];
    memset(camera_data, 0x7E, sizeof(camera_data));
    script_in(camera_data, sizeof(camera_data));

    uint8_t res[16]; /* caller only has room for 16 */
    size_t res_len = 0;
    CHECK(pentax_scsi_exec(&sc, 0x18, 0x08, NULL, 0, res, sizeof(res), &res_len, NULL, NULL) == ESP_OK);

    /* The READ_RESULT CDB's requested length (bytes 19/20 of that CBW: offset
     * 15 + cdb-offset 4/5) must have asked for only res_cap bytes, not 100. */
    size_t read_result_out_idx = 2; /* SEND_COMMAND CBW(0), GET_STATUS CBW(1), READ_RESULT CBW(2) */
    uint16_t requested = (uint16_t)s_out_log[read_result_out_idx][19] |
                         ((uint16_t)s_out_log[read_result_out_idx][20] << 8);
    CHECK_EQ(requested, sizeof(res));
}

static bool sink_reject(const uint8_t *chunk, size_t len, void *ctx) { (void)chunk; (void)len; (void)ctx; return true; }

static void test_exec_oversized_result_with_sink_is_rejected(void)
{
    mock_reset();
    bot_t bot; uint8_t work[512];
    bot_io_t io = make_io();
    CHECK(bot_init(&bot, &io, work, sizeof(work), 0) == ESP_OK);
    pentax_scsi_t sc; pentax_scsi_init(&sc, &bot, true);

    script_none_or_out(); /* SEND_COMMAND CSW */
    uint8_t status_raw[8];
    build_status_raw(status_raw, 0x10001 /* > 64KiB, only possible with a sink in play */,
                      PENTAX_ST_READY, PENTAX_ST_OK);
    script_in(status_raw, sizeof(status_raw));

    size_t res_len = 0xDEAD;
    CHECK(pentax_scsi_exec(&sc, 0x18, 0x08, NULL, 0, NULL, 0, &res_len, sink_reject, NULL)
          == ESP_ERR_INVALID_SIZE);
    /* must not have gone on to request any data from a 64KiB+ transfer */
    CHECK_EQ(s_in_step_idx, s_in_step_count);
}

int main(void)
{
    RUN(test_pentax_build_cdb_layout);
    RUN(test_pentax_encode_args);
    RUN(test_pentax_parse_status_and_fault_injection);
    RUN(test_pentax_status_to_err_exhaustive);
    RUN(test_exec_argument_validation);
    RUN(test_exec_happy_path_with_args_and_result);
    RUN(test_exec_polls_until_ready);
    RUN(test_exec_ready_timeout);
    RUN(test_exec_camera_rejects_command);
    RUN(test_exec_truncates_result_to_caller_capacity);
    RUN(test_exec_oversized_result_with_sink_is_rejected);
    return tt_summary();
}
