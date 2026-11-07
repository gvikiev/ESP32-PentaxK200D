#include "bot.h"

#include <string.h>

#define BOT_DEFAULT_TIMEOUT_MS 5000u

static inline void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static inline uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void bot_build_cbw(uint8_t out[BOT_CBW_SIZE], uint32_t tag, uint32_t data_len, bool dir_in,
                   uint8_t lun, const uint8_t *cdb, uint8_t cdb_len)
{
    memset(out, 0, BOT_CBW_SIZE);
    wr32(out + 0, BOT_CBW_SIG);
    wr32(out + 4, tag);
    wr32(out + 8, data_len);
    out[12] = dir_in ? 0x80 : 0x00;
    out[13] = lun & 0x0F;
    out[14] = cdb_len;
    memcpy(out + 15, cdb, cdb_len);
}

bool bot_parse_csw(const uint8_t *buf, size_t len, uint32_t expect_tag, uint8_t *status, uint32_t *residue)
{
    if (len != BOT_CSW_SIZE || rd32(buf) != BOT_CSW_SIG || rd32(buf + 4) != expect_tag) {
        return false;
    }
    *residue = rd32(buf + 8);
    *status = buf[12];
    return true;
}

esp_err_t bot_init(bot_t *bot, const bot_io_t *io, uint8_t *work, size_t work_cap, uint8_t lun)
{
    if (!bot || !io || !work || !io->bulk_in || !io->bulk_out || io->mps_in == 0 ||
        work_cap < 64 || (work_cap % io->mps_in) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(bot, 0, sizeof(*bot));
    bot->io = *io;
    bot->work = work;
    bot->work_cap = work_cap;
    bot->lun = lun;
    bot->next_tag = 1;
    return ESP_OK;
}

static esp_err_t fail_with_recovery(bot_t *bot, esp_err_t err)
{
    if (bot->io.reset_recovery) {
        bot->io.reset_recovery(bot->io.ctx);
    }
    return err;
}

static esp_err_t read_csw(bot_t *bot, uint32_t tag, uint32_t timeout_ms, bot_result_t *res)
{
    /* BOT 6.7.2: on a STALL while reading the CSW, clear the halt and try once more. */
    for (int attempt = 0; attempt < 2; attempt++) {
        size_t act = 0;
        esp_err_t err = bot->io.bulk_in(bot->io.ctx, bot->work, bot->io.mps_in, &act, timeout_ms);
        if (err == BOT_IO_STALL) {
            if (bot->io.clear_halt) {
                bot->io.clear_halt(bot->io.ctx, true);
            }
            continue;
        }
        if (err != ESP_OK) {
            return fail_with_recovery(bot, err);
        }
        if (!bot_parse_csw(bot->work, act, tag, &res->csw_status, &res->residue)) {
            return fail_with_recovery(bot, ESP_ERR_INVALID_RESPONSE);
        }
        if (res->csw_status == BOT_CSW_PHASE_ERROR) {
            return fail_with_recovery(bot, ESP_ERR_INVALID_RESPONSE);
        }
        return ESP_OK;
    }
    return fail_with_recovery(bot, ESP_ERR_INVALID_RESPONSE);
}

static esp_err_t data_stage_in(bot_t *bot, const bot_cmd_t *cmd, uint32_t timeout_ms, bot_result_t *res)
{
    size_t remaining = cmd->in_len;
    const size_t mps = bot->io.mps_in;

    while (remaining > 0) {
        size_t req = ((remaining + mps - 1) / mps) * mps;
        if (req > bot->work_cap) {
            req = bot->work_cap;
        }
        size_t act = 0;
        esp_err_t err = bot->io.bulk_in(bot->io.ctx, bot->work, req, &act, timeout_ms);
        if (err == BOT_IO_STALL) {
            /* Device ended the data stage early; CSW follows after clearing the halt. */
            if (bot->io.clear_halt) {
                bot->io.clear_halt(bot->io.ctx, true);
            }
            return ESP_OK;
        }
        if (err != ESP_OK) {
            return fail_with_recovery(bot, err);
        }
        if (act == 0) {
            break;
        }
        size_t use = act < remaining ? act : remaining;
        if (cmd->sink) {
            if (!cmd->sink(bot->work, use, cmd->sink_ctx)) {
                return fail_with_recovery(bot, ESP_ERR_INVALID_STATE);
            }
        } else if (cmd->in_buf) {
            memcpy(cmd->in_buf + res->in_received, bot->work, use);
        }
        res->in_received += use;
        remaining -= use;
        if (act < req) {
            break; /* short packet terminates the data stage */
        }
    }
    return ESP_OK;
}

esp_err_t bot_transact(bot_t *bot, const bot_cmd_t *cmd, bot_result_t *result)
{
    if (!bot || !cmd || !result || !cmd->cdb || cmd->cdb_len == 0 || cmd->cdb_len > BOT_MAX_CDB) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));
    uint32_t timeout_ms = cmd->timeout_ms ? cmd->timeout_ms : BOT_DEFAULT_TIMEOUT_MS;

    size_t data_len = 0;
    if (cmd->dir == BOT_DIR_OUT) {
        data_len = cmd->out_len;
    } else if (cmd->dir == BOT_DIR_IN) {
        data_len = cmd->in_len;
    }

    uint32_t tag = bot->next_tag++;
    uint8_t cbw[BOT_CBW_SIZE];
    bot_build_cbw(cbw, tag, (uint32_t)data_len, cmd->dir == BOT_DIR_IN, bot->lun, cmd->cdb, cmd->cdb_len);

    esp_err_t err = bot->io.bulk_out(bot->io.ctx, cbw, sizeof(cbw), timeout_ms);
    if (err != ESP_OK) {
        return fail_with_recovery(bot, err);
    }

    if (cmd->dir == BOT_DIR_OUT && cmd->out_len > 0) {
        err = bot->io.bulk_out(bot->io.ctx, cmd->out, cmd->out_len, timeout_ms);
        if (err == BOT_IO_STALL) {
            if (bot->io.clear_halt) {
                bot->io.clear_halt(bot->io.ctx, false);
            }
        } else if (err != ESP_OK) {
            return fail_with_recovery(bot, err);
        }
    } else if (cmd->dir == BOT_DIR_IN && cmd->in_len > 0) {
        err = data_stage_in(bot, cmd, timeout_ms, result);
        if (err != ESP_OK) {
            return err;
        }
    }

    return read_csw(bot, tag, timeout_ms, result);
}
