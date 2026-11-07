/*
 * Pentax vendor SCSI command layer (opcode 0xF0), on top of bot.h.
 *
 * Protocol reference: docs/REFERENCES.md (pkTriggerCord pentax_scsi_protocol.md).
 * One logical command = [4F send args] -> 24 send command -> 26 poll status
 * -> [49 read result]. This layer knows the framing only, not what any
 * particular command means to the user; see pentax_service.c for that.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "bot.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PENTAX_SCSI_OPCODE      0xF0
#define PENTAX_T_SEND_COMMAND   0x24
#define PENTAX_T_GET_STATUS     0x26
#define PENTAX_T_SEND_ARGS      0x4F
#define PENTAX_T_READ_RESULT    0x49

#define PENTAX_MAX_ARGS         8u

#define PENTAX_ST_READY         0x01  /* status byte S0 */
#define PENTAX_ST_OK            0x01  /* status byte S1 */
#define PENTAX_ST_NOT_AVAILABLE 0x81
#define PENTAX_ST_REJECTED      0x82
#define PENTAX_ST_ILLEGAL_PARAM 0x83

typedef struct {
    bot_t *bot;
    bool big_endian_args;        /* K200D and other pre-K-3 bodies: true */
    uint32_t cmd_timeout_ms;     /* per BOT transaction; 0 = default */
    uint32_t ready_timeout_ms;   /* how long to poll "get status" for completion */
} pentax_scsi_t;

typedef struct {
    uint32_t result_len;
    uint8_t ready;
    uint8_t status;
} pentax_status_t;

typedef bool (*pentax_sink_t)(const uint8_t *chunk, size_t len, void *ctx);

void pentax_scsi_init(pentax_scsi_t *p, bot_t *bot, bool big_endian_args);

/* Pure helpers (exposed for tests). */
void pentax_build_cdb(uint8_t cdb[8], uint8_t type, uint8_t b2, uint8_t b3, uint16_t w4, uint16_t w6_unused_zero);
size_t pentax_encode_args(uint8_t *out, const uint32_t *args, size_t nargs, bool big_endian);
void pentax_parse_status(const uint8_t raw[8], pentax_status_t *out);
esp_err_t pentax_status_to_err(uint8_t s1);

/* Runs one logical command. If `sink` is non-NULL, the result bytes are
 * streamed to it; otherwise they are copied to `res` (capacity `res_cap`).
 * *res_len receives the number of result bytes. */
esp_err_t pentax_scsi_exec(pentax_scsi_t *p, uint8_t group, uint8_t cmd,
                           const uint32_t *args, size_t nargs,
                           uint8_t *res, size_t res_cap, size_t *res_len,
                           pentax_sink_t sink, void *sink_ctx);

#ifdef __cplusplus
}
#endif
