#include "ptp.h"
#include <string.h>

size_t ptp_build_header(uint8_t *out, uint32_t length, uint16_t type, uint16_t code,
                         uint32_t transaction_id)
{
    ptp_wr32(out + 0, length);
    ptp_wr16(out + 4, type);
    ptp_wr16(out + 6, code);
    ptp_wr32(out + 8, transaction_id);
    return PTP_CONTAINER_HEADER_SIZE;
}

size_t ptp_build_command(uint8_t *out, size_t out_cap, uint16_t opcode,
                          uint32_t transaction_id, const uint32_t *params, int nparams)
{
    if (nparams < 0) {
        nparams = 0;
    }
    if ((size_t)nparams > PTP_MAX_PARAMS) {
        nparams = PTP_MAX_PARAMS;
    }
    size_t total = PTP_CONTAINER_HEADER_SIZE + (size_t)nparams * 4u;
    if (out_cap < total) {
        return 0;
    }
    ptp_build_header(out, (uint32_t)total, PTP_CONTAINER_TYPE_COMMAND, opcode, transaction_id);
    for (int i = 0; i < nparams; i++) {
        ptp_wr32(out + PTP_CONTAINER_HEADER_SIZE + (size_t)i * 4u, params[i]);
    }
    return total;
}

bool ptp_decode_string(const uint8_t *buf, size_t buf_len, size_t *off,
                       char *out, size_t out_cap)
{
    if (*off >= buf_len) {
        return false;
    }
    uint8_t count = buf[*off]; /* number of UTF-16 code units, including terminating NUL */
    size_t need = 1u + (size_t)count * 2u;
    if (*off + need > buf_len) {
        return false;
    }
    const uint8_t *chars = buf + *off + 1;
    size_t out_i = 0;
    for (uint8_t i = 0; i < count; i++) {
        uint16_t ch = ptp_rd16(chars + (size_t)i * 2u);
        if (ch == 0) {
            break;
        }
        if (ch < 0x80) {
            if (out_i + 1 < out_cap) {
                out[out_i++] = (char)ch;
            }
        } else if (ch < 0x800) {
            if (out_i + 2 < out_cap) {
                out[out_i++] = (char)(0xC0 | (ch >> 6));
                out[out_i++] = (char)(0x80 | (ch & 0x3F));
            }
        } else {
            if (out_i + 3 < out_cap) {
                out[out_i++] = (char)(0xE0 | (ch >> 12));
                out[out_i++] = (char)(0x80 | ((ch >> 6) & 0x3F));
                out[out_i++] = (char)(0x80 | (ch & 0x3F));
            }
        }
    }
    if (out_cap > 0) {
        out[out_i < out_cap ? out_i : out_cap - 1] = '\0';
    }
    *off += need;
    return true;
}

bool ptp_skip_string(const uint8_t *buf, size_t buf_len, size_t *off)
{
    if (*off >= buf_len) {
        return false;
    }
    uint8_t count = buf[*off];
    size_t need = 1u + (size_t)count * 2u;
    if (*off + need > buf_len) {
        return false;
    }
    *off += need;
    return true;
}
