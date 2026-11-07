/*
 * Bounded producer/consumer block queue used to move a photo from the USB
 * task to the HTTP task without holding the whole file in RAM.
 *
 *   producer (USB):  ps_acquire() -> fill -> ps_submit()   ... ps_finish()
 *   consumer (HTTP): ps_next() -> send -> ps_release()     ... until EOF/error
 *
 * Memory is fixed at creation: `nblocks * block_size`, taken from PSRAM when
 * available (falls back to internal heap). Back-pressure is natural: when the
 * HTTP client is slow, ps_acquire() blocks the USB reader.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct photo_stream photo_stream_t;

typedef struct {
    uint8_t *data;
    size_t len;
    bool eof;      /* no more blocks after this one (data may be empty) */
    esp_err_t err; /* non-OK: producer failed; stream is terminated */
    int slot;      /* internal: pass back to ps_release() */
} ps_block_t;

photo_stream_t *ps_create(size_t block_size, size_t nblocks);
void ps_destroy(photo_stream_t *s);

size_t ps_block_size(const photo_stream_t *s);

/* Producer side. ps_acquire returns NULL on timeout or if the consumer aborted. */
uint8_t *ps_acquire(photo_stream_t *s, uint32_t timeout_ms, int *slot);
esp_err_t ps_submit(photo_stream_t *s, int slot, size_t len);
/* Ends the stream; err != ESP_OK marks it failed. */
esp_err_t ps_finish(photo_stream_t *s, esp_err_t err);

/* Consumer side. */
esp_err_t ps_next(photo_stream_t *s, uint32_t timeout_ms, ps_block_t *out);
void ps_release(photo_stream_t *s, const ps_block_t *blk);
/* Consumer gives up (e.g. HTTP client went away); wakes the producer. */
void ps_abort(photo_stream_t *s);
bool ps_aborted(const photo_stream_t *s);

#ifdef __cplusplus
}
#endif
