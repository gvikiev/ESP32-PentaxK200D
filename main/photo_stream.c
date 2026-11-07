#include "photo_stream.h"

#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"

typedef struct {
    int slot;
    size_t len;
    bool eof;
    esp_err_t err;
} item_t;

struct photo_stream {
    size_t block_size;
    size_t nblocks;
    uint8_t **blocks;
    QueueHandle_t free_q;   /* int slot */
    QueueHandle_t full_q;   /* item_t */
    volatile bool aborted;
    volatile bool finished;
};

photo_stream_t *ps_create(size_t block_size, size_t nblocks)
{
    if (block_size == 0 || nblocks < 2) {
        return NULL;
    }
    photo_stream_t *s = calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }
    s->block_size = block_size;
    s->nblocks = nblocks;
    s->blocks = calloc(nblocks, sizeof(uint8_t *));
    s->free_q = xQueueCreate((UBaseType_t)nblocks, sizeof(int));
    /* +1: a terminal item may be queued even when every block is in flight. */
    s->full_q = xQueueCreate((UBaseType_t)nblocks + 1, sizeof(item_t));
    if (!s->blocks || !s->free_q || !s->full_q) {
        ps_destroy(s);
        return NULL;
    }
    for (size_t i = 0; i < nblocks; i++) {
        s->blocks[i] = heap_caps_malloc(block_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s->blocks[i]) {
            s->blocks[i] = heap_caps_malloc(block_size, MALLOC_CAP_8BIT);
        }
        if (!s->blocks[i]) {
            ps_destroy(s);
            return NULL;
        }
        int slot = (int)i;
        xQueueSend(s->free_q, &slot, 0);
    }
    return s;
}

void ps_destroy(photo_stream_t *s)
{
    if (!s) {
        return;
    }
    if (s->blocks) {
        for (size_t i = 0; i < s->nblocks; i++) {
            heap_caps_free(s->blocks[i]);
        }
        free(s->blocks);
    }
    if (s->free_q) { vQueueDelete(s->free_q); }
    if (s->full_q) { vQueueDelete(s->full_q); }
    free(s);
}

size_t ps_block_size(const photo_stream_t *s)
{
    return s->block_size;
}

uint8_t *ps_acquire(photo_stream_t *s, uint32_t timeout_ms, int *slot)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (!s->aborted) {
        int sl;
        /* Poll in short slices so an abort is noticed promptly. */
        if (xQueueReceive(s->free_q, &sl, pdMS_TO_TICKS(50)) == pdTRUE) {
            *slot = sl;
            return s->blocks[sl];
        }
        if ((int32_t)(xTaskGetTickCount() - deadline) >= 0) {
            break;
        }
    }
    return NULL;
}

esp_err_t ps_submit(photo_stream_t *s, int slot, size_t len)
{
    item_t it = { .slot = slot, .len = len, .eof = false, .err = ESP_OK };
    return xQueueSend(s->full_q, &it, portMAX_DELAY) == pdTRUE ? ESP_OK : ESP_FAIL;
}

esp_err_t ps_finish(photo_stream_t *s, esp_err_t err)
{
    item_t it = { .slot = -1, .len = 0, .eof = true, .err = err };
    s->finished = true;
    return xQueueSend(s->full_q, &it, pdMS_TO_TICKS(1000)) == pdTRUE ? ESP_OK : ESP_FAIL;
}

esp_err_t ps_next(photo_stream_t *s, uint32_t timeout_ms, ps_block_t *out)
{
    item_t it;
    if (xQueueReceive(s->full_q, &it, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    out->slot = it.slot;
    out->len = it.len;
    out->eof = it.eof;
    out->err = it.err;
    out->data = it.slot >= 0 ? s->blocks[it.slot] : NULL;
    return ESP_OK;
}

void ps_release(photo_stream_t *s, const ps_block_t *blk)
{
    if (blk->slot >= 0) {
        int sl = blk->slot;
        xQueueSend(s->free_q, &sl, 0);
    }
}

void ps_abort(photo_stream_t *s)
{
    s->aborted = true;
}

bool ps_aborted(const photo_stream_t *s)
{
    return s->aborted;
}
