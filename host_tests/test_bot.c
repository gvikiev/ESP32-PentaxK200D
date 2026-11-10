/*
 * bot.c is already designed for this: all real USB I/O goes through the
 * bot_io_t function-pointer table, so a scripted mock in place of real bulk
 * transfers lets us exercise fault paths (STALL, phase errors, corrupted or
 * truncated CSWs, short reads) that would be slow or flaky to reproduce
 * against a real camera.
 */
#include "tinytest.h"
#include "bot.h"

#include <string.h>

/* ---------------------------- scripted mock bot_io_t ---------------------------- */

#define MOCK_MAX_IN_STEPS 8
#define MOCK_MAX_OUT_LOG  4

typedef struct {
    uint8_t data[256];
    size_t len;
    esp_err_t ret; /* ESP_OK or BOT_IO_STALL */
} mock_in_step_t;

static mock_in_step_t s_in_steps[MOCK_MAX_IN_STEPS];
static size_t s_in_step_count;
static size_t s_in_step_idx;

static uint8_t s_out_log[MOCK_MAX_OUT_LOG][256];
static size_t s_out_log_len[MOCK_MAX_OUT_LOG];
static size_t s_out_call_count;
static esp_err_t s_out_rets[MOCK_MAX_OUT_LOG];

static int s_clear_halt_calls;
static bool s_clear_halt_last_in;
static int s_reset_recovery_calls;

static void mock_reset(void)
{
    s_in_step_count = 0;
    s_in_step_idx = 0;
    s_out_call_count = 0;
    memset(s_out_log_len, 0, sizeof(s_out_log_len));
    for (size_t i = 0; i < MOCK_MAX_OUT_LOG; i++) {
        s_out_rets[i] = ESP_OK;
    }
    s_clear_halt_calls = 0;
    s_clear_halt_last_in = false;
    s_reset_recovery_calls = 0;
}

static void mock_push_in(const uint8_t *data, size_t len, esp_err_t ret)
{
    mock_in_step_t *s = &s_in_steps[s_in_step_count++];
    if (data && len) {
        memcpy(s->data, data, len);
    }
    s->len = len;
    s->ret = ret;
}

static esp_err_t mock_bulk_out(void *ctx, const uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    (void)ctx; (void)timeout_ms;
    size_t idx = s_out_call_count;
    if (idx < MOCK_MAX_OUT_LOG) {
        memcpy(s_out_log[idx], buf, len);
        s_out_log_len[idx] = len;
    }
    esp_err_t ret = idx < MOCK_MAX_OUT_LOG ? s_out_rets[idx] : ESP_OK;
    s_out_call_count++;
    return ret;
}

static esp_err_t mock_bulk_in(void *ctx, uint8_t *buf, size_t max_len, size_t *actual, uint32_t timeout_ms)
{
    (void)ctx; (void)max_len; (void)timeout_ms;
    if (s_in_step_idx >= s_in_step_count) {
        *actual = 0;
        return ESP_FAIL; /* test bug: ran off the end of the script */
    }
    mock_in_step_t *s = &s_in_steps[s_in_step_idx++];
    if (s->ret == ESP_OK) {
        memcpy(buf, s->data, s->len);
        *actual = s->len;
    } else {
        *actual = 0;
    }
    return s->ret;
}

static esp_err_t mock_clear_halt(void *ctx, bool in_endpoint)
{
    (void)ctx;
    s_clear_halt_calls++;
    s_clear_halt_last_in = in_endpoint;
    return ESP_OK;
}

static esp_err_t mock_reset_recovery(void *ctx)
{
    (void)ctx;
    s_reset_recovery_calls++;
    return ESP_OK;
}

static bot_io_t make_io(uint16_t mps_in)
{
    bot_io_t io = {
        .ctx = NULL,
        .mps_in = mps_in,
        .bulk_out = mock_bulk_out,
        .bulk_in = mock_bulk_in,
        .clear_halt = mock_clear_halt,
        .reset_recovery = mock_reset_recovery,
    };
    return io;
}

static void build_csw(uint8_t out[BOT_CSW_SIZE], uint32_t tag, uint32_t residue, uint8_t status)
{
    out[0] = 'U'; out[1] = 'S'; out[2] = 'B'; out[3] = 'S';
    out[4] = (uint8_t)tag; out[5] = (uint8_t)(tag >> 8); out[6] = (uint8_t)(tag >> 16); out[7] = (uint8_t)(tag >> 24);
    out[8] = (uint8_t)residue; out[9] = (uint8_t)(residue >> 8); out[10] = (uint8_t)(residue >> 16); out[11] = (uint8_t)(residue >> 24);
    out[12] = status;
}

/* ---------------------------------- pure helpers ---------------------------------- */

static void test_bot_build_cbw_layout(void)
{
    uint8_t cdb[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    uint8_t out[BOT_CBW_SIZE];

    bot_build_cbw(out, 0x01020304, 0xAABBCCDDu, true, 0x07, cdb, sizeof(cdb));

    CHECK(memcmp(out, "USBC", 4) == 0);
    CHECK_EQ(out[4], 0x04); CHECK_EQ(out[5], 0x03); CHECK_EQ(out[6], 0x02); CHECK_EQ(out[7], 0x01);
    CHECK_EQ(out[8], 0xDD); CHECK_EQ(out[9], 0xCC); CHECK_EQ(out[10], 0xBB); CHECK_EQ(out[11], 0xAA);
    CHECK_EQ(out[12], 0x80); /* dir_in */
    CHECK_EQ(out[13], 0x07); /* lun */
    CHECK_EQ(out[14], sizeof(cdb));
    CHECK(memcmp(out + 15, cdb, sizeof(cdb)) == 0);
    for (size_t i = 15 + sizeof(cdb); i < BOT_CBW_SIZE; i++) {
        CHECK_EQ(out[i], 0);
    }

    bot_build_cbw(out, 1, 0, false, 0x1F /* masked to low nibble */, cdb, sizeof(cdb));
    CHECK_EQ(out[12], 0x00);
    CHECK_EQ(out[13], 0x0F);
}

static void test_bot_parse_csw_valid_and_fault_injection(void)
{
    uint8_t csw[BOT_CSW_SIZE];
    build_csw(csw, 42, 7, BOT_CSW_PASSED);

    uint8_t status; uint32_t residue;
    CHECK(bot_parse_csw(csw, sizeof(csw), 42, &status, &residue) == true);
    CHECK_EQ(status, BOT_CSW_PASSED);
    CHECK_EQ(residue, 7);

    CHECK(bot_parse_csw(csw, sizeof(csw) - 1, 42, &status, &residue) == false);  /* truncated */
    CHECK(bot_parse_csw(csw, sizeof(csw), 43, &status, &residue) == false);      /* wrong tag */

    uint8_t bad_sig[BOT_CSW_SIZE];
    memcpy(bad_sig, csw, sizeof(csw));
    bad_sig[0] = 'X';
    CHECK(bot_parse_csw(bad_sig, sizeof(bad_sig), 42, &status, &residue) == false);
}

static void test_bot_init_validation(void)
{
    bot_t bot;
    uint8_t work[64];
    bot_io_t good = make_io(64);

    CHECK(bot_init(NULL, &good, work, sizeof(work), 0) == ESP_ERR_INVALID_ARG);
    CHECK(bot_init(&bot, NULL, work, sizeof(work), 0) == ESP_ERR_INVALID_ARG);
    CHECK(bot_init(&bot, &good, NULL, sizeof(work), 0) == ESP_ERR_INVALID_ARG);

    bot_io_t no_in = good; no_in.bulk_in = NULL;
    CHECK(bot_init(&bot, &no_in, work, sizeof(work), 0) == ESP_ERR_INVALID_ARG);
    bot_io_t no_out = good; no_out.bulk_out = NULL;
    CHECK(bot_init(&bot, &no_out, work, sizeof(work), 0) == ESP_ERR_INVALID_ARG);
    bot_io_t zero_mps = good; zero_mps.mps_in = 0;
    CHECK(bot_init(&bot, &zero_mps, work, sizeof(work), 0) == ESP_ERR_INVALID_ARG);

    CHECK(bot_init(&bot, &good, work, 32 /* < 64 */, 0) == ESP_ERR_INVALID_ARG);
    CHECK(bot_init(&bot, &good, work, 100 /* not a multiple of mps_in */, 0) == ESP_ERR_INVALID_ARG);

    CHECK(bot_init(&bot, &good, work, sizeof(work), 3) == ESP_OK);
    CHECK_EQ(bot.lun, 3);
    CHECK_EQ(bot.next_tag, 1);
    CHECK(bot.work == work);
    CHECK_EQ(bot.work_cap, sizeof(work));
}

/* ------------------------------- bot_transact: happy paths ------------------------------- */

static void test_bot_transact_out_happy_path(void)
{
    mock_reset();
    bot_t bot;
    uint8_t work[64];
    bot_io_t io = make_io(64);
    CHECK(bot_init(&bot, &io, work, sizeof(work), 2) == ESP_OK);

    uint8_t cdb[8] = {0xF0, 0x4F, 0, 0, 4, 0, 0, 0};
    uint8_t payload[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    bot_cmd_t cmd = {
        .cdb = cdb, .cdb_len = sizeof(cdb), .dir = BOT_DIR_OUT,
        .out = payload, .out_len = sizeof(payload), .timeout_ms = 1000,
    };

    uint8_t csw[BOT_CSW_SIZE];
    build_csw(csw, 1, 0, BOT_CSW_PASSED);
    mock_push_in(csw, sizeof(csw), ESP_OK);

    bot_result_t res;
    CHECK(bot_transact(&bot, &cmd, &res) == ESP_OK);
    CHECK_EQ(res.csw_status, BOT_CSW_PASSED);
    CHECK_EQ(res.residue, 0);

    CHECK_EQ(s_out_call_count, 2);
    CHECK_EQ(s_out_log_len[0], BOT_CBW_SIZE);
    CHECK(memcmp(s_out_log[0], "USBC", 4) == 0);
    CHECK_EQ(s_out_log_len[1], sizeof(payload));
    CHECK(memcmp(s_out_log[1], payload, sizeof(payload)) == 0);
}

static void test_bot_transact_in_multi_chunk(void)
{
    mock_reset();
    bot_t bot;
    uint8_t work[64]; /* small work_cap forces several data-stage reads */
    bot_io_t io = make_io(64);
    CHECK(bot_init(&bot, &io, work, sizeof(work), 0) == ESP_OK);

    uint8_t cdb[8] = {0xF0, 0x49, 0, 0, 0, 0, 0, 0};
    uint8_t in_buf[150];
    memset(in_buf, 0, sizeof(in_buf));
    bot_cmd_t cmd = {
        .cdb = cdb, .cdb_len = sizeof(cdb), .dir = BOT_DIR_IN,
        .in_len = 150, .in_buf = in_buf, .timeout_ms = 1000,
    };

    uint8_t expected[150];
    for (size_t i = 0; i < sizeof(expected); i++) {
        expected[i] = (uint8_t)i;
    }
    /* 150 bytes over a 64-byte work buffer: two full 64-byte chunks, then a
     * final short 22-byte chunk that terminates the data stage. */
    mock_push_in(expected, 64, ESP_OK);
    mock_push_in(expected + 64, 64, ESP_OK);
    mock_push_in(expected + 128, 22, ESP_OK);
    uint8_t csw[BOT_CSW_SIZE];
    build_csw(csw, 1, 0, BOT_CSW_PASSED);
    mock_push_in(csw, sizeof(csw), ESP_OK);

    bot_result_t res;
    CHECK(bot_transact(&bot, &cmd, &res) == ESP_OK);
    CHECK_EQ(res.in_received, 150);
    CHECK(memcmp(in_buf, expected, sizeof(expected)) == 0);
}

static bool sink_collect(const uint8_t *chunk, size_t len, void *ctx)
{
    struct { uint8_t buf[256]; size_t len; } *acc = ctx;
    memcpy(acc->buf + acc->len, chunk, len);
    acc->len += len;
    return true;
}

static void test_bot_transact_in_via_sink(void)
{
    mock_reset();
    bot_t bot;
    uint8_t work[64];
    bot_io_t io = make_io(64);
    CHECK(bot_init(&bot, &io, work, sizeof(work), 0) == ESP_OK);

    uint8_t cdb[8] = {0xF0, 0x49, 0, 0, 0, 0, 0, 0};
    struct { uint8_t buf[256]; size_t len; } acc = {0};
    bot_cmd_t cmd = {
        .cdb = cdb, .cdb_len = sizeof(cdb), .dir = BOT_DIR_IN,
        .in_len = 70, .sink = sink_collect, .sink_ctx = &acc, .timeout_ms = 1000,
    };

    uint8_t expected[70];
    for (size_t i = 0; i < sizeof(expected); i++) {
        expected[i] = (uint8_t)(0xA0 + i);
    }
    mock_push_in(expected, 64, ESP_OK);
    mock_push_in(expected + 64, 6, ESP_OK);
    uint8_t csw[BOT_CSW_SIZE];
    build_csw(csw, 1, 0, BOT_CSW_PASSED);
    mock_push_in(csw, sizeof(csw), ESP_OK);

    bot_result_t res;
    CHECK(bot_transact(&bot, &cmd, &res) == ESP_OK);
    CHECK_EQ(res.in_received, 70);
    CHECK_EQ(acc.len, 70);
    CHECK(memcmp(acc.buf, expected, sizeof(expected)) == 0);
}

/* ------------------------------- bot_transact: fault injection ------------------------------- */

static void test_bot_transact_stall_during_data_stage(void)
{
    mock_reset();
    bot_t bot;
    uint8_t work[64];
    bot_io_t io = make_io(64);
    CHECK(bot_init(&bot, &io, work, sizeof(work), 0) == ESP_OK);

    uint8_t cdb[8] = {0xF0, 0x49, 0, 0, 0, 0, 0, 0};
    uint8_t in_buf[64];
    bot_cmd_t cmd = {
        .cdb = cdb, .cdb_len = sizeof(cdb), .dir = BOT_DIR_IN,
        .in_len = 64, .in_buf = in_buf, .timeout_ms = 1000,
    };

    /* Device ends the data stage early with a STALL; the CSW still follows. */
    mock_push_in(NULL, 0, BOT_IO_STALL);
    uint8_t csw[BOT_CSW_SIZE];
    build_csw(csw, 1, 0, BOT_CSW_PASSED);
    mock_push_in(csw, sizeof(csw), ESP_OK);

    bot_result_t res;
    CHECK(bot_transact(&bot, &cmd, &res) == ESP_OK);
    CHECK_EQ(res.in_received, 0);
    CHECK_EQ(s_clear_halt_calls, 1);
    CHECK(s_clear_halt_last_in == true);
}

static void test_bot_transact_stall_then_recover_on_csw(void)
{
    mock_reset();
    bot_t bot;
    uint8_t work[64];
    bot_io_t io = make_io(64);
    CHECK(bot_init(&bot, &io, work, sizeof(work), 0) == ESP_OK);

    uint8_t cdb[8] = {0xF0, 0x26, 0, 0, 0, 0, 0, 0};
    bot_cmd_t cmd = { .cdb = cdb, .cdb_len = sizeof(cdb), .dir = BOT_DIR_NONE, .timeout_ms = 1000 };

    mock_push_in(NULL, 0, BOT_IO_STALL);     /* first CSW read stalls */
    uint8_t csw[BOT_CSW_SIZE];
    build_csw(csw, 1, 0, BOT_CSW_PASSED);
    mock_push_in(csw, sizeof(csw), ESP_OK);  /* retry succeeds */

    bot_result_t res;
    CHECK(bot_transact(&bot, &cmd, &res) == ESP_OK);
    CHECK_EQ(s_clear_halt_calls, 1);
    CHECK_EQ(s_reset_recovery_calls, 0);
}

static void test_bot_transact_csw_stalls_both_attempts(void)
{
    mock_reset();
    bot_t bot;
    uint8_t work[64];
    bot_io_t io = make_io(64);
    CHECK(bot_init(&bot, &io, work, sizeof(work), 0) == ESP_OK);

    uint8_t cdb[8] = {0xF0, 0x26, 0, 0, 0, 0, 0, 0};
    bot_cmd_t cmd = { .cdb = cdb, .cdb_len = sizeof(cdb), .dir = BOT_DIR_NONE, .timeout_ms = 1000 };

    mock_push_in(NULL, 0, BOT_IO_STALL);
    mock_push_in(NULL, 0, BOT_IO_STALL);

    bot_result_t res;
    CHECK(bot_transact(&bot, &cmd, &res) == ESP_ERR_INVALID_RESPONSE);
    CHECK_EQ(s_clear_halt_calls, 2);
    CHECK_EQ(s_reset_recovery_calls, 1);
}

static void test_bot_transact_corrupted_csw_signature(void)
{
    mock_reset();
    bot_t bot;
    uint8_t work[64];
    bot_io_t io = make_io(64);
    CHECK(bot_init(&bot, &io, work, sizeof(work), 0) == ESP_OK);

    uint8_t cdb[8] = {0xF0, 0x26, 0, 0, 0, 0, 0, 0};
    bot_cmd_t cmd = { .cdb = cdb, .cdb_len = sizeof(cdb), .dir = BOT_DIR_NONE, .timeout_ms = 1000 };

    uint8_t garbage[BOT_CSW_SIZE];
    build_csw(garbage, 1, 0, BOT_CSW_PASSED);
    garbage[0] = 0x00; /* corrupt the "USBS" signature */
    mock_push_in(garbage, sizeof(garbage), ESP_OK);

    bot_result_t res;
    CHECK(bot_transact(&bot, &cmd, &res) == ESP_ERR_INVALID_RESPONSE);
    CHECK_EQ(s_clear_halt_calls, 0);     /* not a STALL, so no retry attempted */
    CHECK_EQ(s_reset_recovery_calls, 1);
}

static void test_bot_transact_phase_error(void)
{
    mock_reset();
    bot_t bot;
    uint8_t work[64];
    bot_io_t io = make_io(64);
    CHECK(bot_init(&bot, &io, work, sizeof(work), 0) == ESP_OK);

    uint8_t cdb[8] = {0xF0, 0x26, 0, 0, 0, 0, 0, 0};
    bot_cmd_t cmd = { .cdb = cdb, .cdb_len = sizeof(cdb), .dir = BOT_DIR_NONE, .timeout_ms = 1000 };

    uint8_t csw[BOT_CSW_SIZE];
    build_csw(csw, 1, 0, BOT_CSW_PHASE_ERROR);
    mock_push_in(csw, sizeof(csw), ESP_OK);

    bot_result_t res;
    CHECK(bot_transact(&bot, &cmd, &res) == ESP_ERR_INVALID_RESPONSE);
    CHECK_EQ(s_reset_recovery_calls, 1);
}

static void test_bot_transact_cbw_send_failure_aborts_early(void)
{
    mock_reset();
    bot_t bot;
    uint8_t work[64];
    bot_io_t io = make_io(64);
    CHECK(bot_init(&bot, &io, work, sizeof(work), 0) == ESP_OK);

    uint8_t cdb[8] = {0xF0, 0x26, 0, 0, 0, 0, 0, 0};
    bot_cmd_t cmd = { .cdb = cdb, .cdb_len = sizeof(cdb), .dir = BOT_DIR_NONE, .timeout_ms = 1000 };

    s_out_rets[0] = ESP_ERR_INVALID_STATE; /* simulated transport failure sending the CBW */

    bot_result_t res;
    CHECK(bot_transact(&bot, &cmd, &res) == ESP_ERR_INVALID_STATE);
    CHECK_EQ(s_reset_recovery_calls, 1);
    CHECK_EQ(s_in_step_idx, 0); /* never got as far as reading anything back */
}

static void test_bot_transact_argument_validation(void)
{
    mock_reset();
    bot_t bot;
    uint8_t work[64];
    bot_io_t io = make_io(64);
    CHECK(bot_init(&bot, &io, work, sizeof(work), 0) == ESP_OK);

    uint8_t cdb[8] = {0xF0, 0x26, 0, 0, 0, 0, 0, 0};
    bot_cmd_t cmd = { .cdb = cdb, .cdb_len = sizeof(cdb), .dir = BOT_DIR_NONE, .timeout_ms = 1000 };
    bot_result_t res;

    CHECK(bot_transact(NULL, &cmd, &res) == ESP_ERR_INVALID_ARG);
    CHECK(bot_transact(&bot, NULL, &res) == ESP_ERR_INVALID_ARG);
    CHECK(bot_transact(&bot, &cmd, NULL) == ESP_ERR_INVALID_ARG);

    bot_cmd_t no_cdb = cmd; no_cdb.cdb = NULL;
    CHECK(bot_transact(&bot, &no_cdb, &res) == ESP_ERR_INVALID_ARG);

    bot_cmd_t zero_len = cmd; zero_len.cdb_len = 0;
    CHECK(bot_transact(&bot, &zero_len, &res) == ESP_ERR_INVALID_ARG);

    bot_cmd_t too_long = cmd; too_long.cdb_len = BOT_MAX_CDB + 1;
    CHECK(bot_transact(&bot, &too_long, &res) == ESP_ERR_INVALID_ARG);

    CHECK_EQ(s_out_call_count, 0); /* rejected before any IO was attempted */
}

static void test_bot_transact_tag_increments_and_is_checked(void)
{
    mock_reset();
    bot_t bot;
    uint8_t work[64];
    bot_io_t io = make_io(64);
    CHECK(bot_init(&bot, &io, work, sizeof(work), 0) == ESP_OK);

    uint8_t cdb[8] = {0xF0, 0x26, 0, 0, 0, 0, 0, 0};
    bot_cmd_t cmd = { .cdb = cdb, .cdb_len = sizeof(cdb), .dir = BOT_DIR_NONE, .timeout_ms = 1000 };
    bot_result_t res;

    uint8_t csw1[BOT_CSW_SIZE]; build_csw(csw1, 1, 0, BOT_CSW_PASSED);
    mock_push_in(csw1, sizeof(csw1), ESP_OK);
    CHECK(bot_transact(&bot, &cmd, &res) == ESP_OK);

    uint8_t csw2[BOT_CSW_SIZE]; build_csw(csw2, 2, 0, BOT_CSW_PASSED);
    mock_push_in(csw2, sizeof(csw2), ESP_OK);
    CHECK(bot_transact(&bot, &cmd, &res) == ESP_OK);

    /* A CSW echoing a stale tag must be rejected, not silently accepted. */
    uint8_t csw_stale[BOT_CSW_SIZE]; build_csw(csw_stale, 2, 0, BOT_CSW_PASSED);
    mock_push_in(csw_stale, sizeof(csw_stale), ESP_OK);
    CHECK(bot_transact(&bot, &cmd, &res) == ESP_ERR_INVALID_RESPONSE);
}

int main(void)
{
    RUN(test_bot_build_cbw_layout);
    RUN(test_bot_parse_csw_valid_and_fault_injection);
    RUN(test_bot_init_validation);
    RUN(test_bot_transact_out_happy_path);
    RUN(test_bot_transact_in_multi_chunk);
    RUN(test_bot_transact_in_via_sink);
    RUN(test_bot_transact_stall_during_data_stage);
    RUN(test_bot_transact_stall_then_recover_on_csw);
    RUN(test_bot_transact_csw_stalls_both_attempts);
    RUN(test_bot_transact_corrupted_csw_signature);
    RUN(test_bot_transact_phase_error);
    RUN(test_bot_transact_cbw_send_failure_aborts_early);
    RUN(test_bot_transact_argument_validation);
    RUN(test_bot_transact_tag_increments_and_is_checked);
    return tt_summary();
}
