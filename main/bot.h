/*
 * USB Mass Storage Bulk-Only Transport (BOT) layer.
 *
 * Knows nothing about SCSI command semantics or about the USB stack: it
 * only builds CBWs, moves the optional data stage, reads/validates the CSW
 * and performs the BOT error recovery (clear halt / reset recovery). The
 * actual USB pipes are supplied by the caller through `bot_io_t`, which
 * lets this layer be unit-tested against a mock device.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOT_CBW_SIZE      31u
#define BOT_CSW_SIZE      13u
#define BOT_CBW_SIG       0x43425355u /* "USBC" */
#define BOT_CSW_SIG       0x53425355u /* "USBS" */
#define BOT_MAX_CDB       16u

/* Returned by bot_io_t callbacks when the endpoint answered with STALL. */
#define BOT_IO_STALL      ((esp_err_t)0x7F01)

#define BOT_CSW_PASSED       0
#define BOT_CSW_FAILED       1
#define BOT_CSW_PHASE_ERROR  2

typedef struct {
    void *ctx;
    uint16_t mps_in;   /* bulk IN max packet size (IN reads are rounded up to it) */
    /* Send exactly `len` bytes on bulk OUT (the callee splits into transfers). */
    esp_err_t (*bulk_out)(void *ctx, const uint8_t *buf, size_t len, uint32_t timeout_ms);
    /* One bulk IN transfer of up to `max_len` bytes (a multiple of mps_in);
     * ends on a short packet or when max_len is reached. */
    esp_err_t (*bulk_in)(void *ctx, uint8_t *buf, size_t max_len, size_t *actual, uint32_t timeout_ms);
    esp_err_t (*clear_halt)(void *ctx, bool in_endpoint);
    /* Class request "Bulk-Only Mass Storage Reset" + clear both halts. */
    esp_err_t (*reset_recovery)(void *ctx);
} bot_io_t;

typedef enum {
    BOT_DIR_NONE = 0,
    BOT_DIR_OUT,
    BOT_DIR_IN,
} bot_dir_t;

/* Receives data-stage bytes; return false to abort the transfer. */
typedef bool (*bot_sink_t)(const uint8_t *chunk, size_t len, void *ctx);

typedef struct {
    bot_io_t io;
    uint8_t *work;      /* scratch buffer for IN transfers; size multiple of mps_in, >= 64 */
    size_t   work_cap;
    uint8_t  lun;
    uint32_t next_tag;
} bot_t;

typedef struct {
    const uint8_t *cdb;
    uint8_t cdb_len;
    bot_dir_t dir;
    const uint8_t *out;    /* DIR_OUT payload */
    size_t out_len;
    size_t in_len;         /* DIR_IN: number of bytes the host expects */
    bot_sink_t sink;       /* DIR_IN: streaming sink, or NULL to copy into in_buf */
    void *sink_ctx;
    uint8_t *in_buf;       /* DIR_IN with sink==NULL: capacity must be >= in_len */
    uint32_t timeout_ms;
} bot_cmd_t;

typedef struct {
    uint8_t csw_status;    /* BOT_CSW_* */
    uint32_t residue;
    size_t in_received;
} bot_result_t;

esp_err_t bot_init(bot_t *bot, const bot_io_t *io, uint8_t *work, size_t work_cap, uint8_t lun);

/* Runs one Command/Data/Status transport. ESP_OK means a valid CSW was read
 * (inspect result->csw_status); any other value is a transport failure after
 * which recovery has already been attempted. */
esp_err_t bot_transact(bot_t *bot, const bot_cmd_t *cmd, bot_result_t *result);

/* Pure helpers (exposed for tests). */
void bot_build_cbw(uint8_t out[BOT_CBW_SIZE], uint32_t tag, uint32_t data_len, bool dir_in,
                   uint8_t lun, const uint8_t *cdb, uint8_t cdb_len);
bool bot_parse_csw(const uint8_t *buf, size_t len, uint32_t expect_tag, uint8_t *status, uint32_t *residue);

#ifdef __cplusplus
}
#endif
