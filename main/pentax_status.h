/*
 * Parser for the Pentax K200D "Get Full Status" buffer ([00 08], 408 bytes,
 * big-endian). Offset map and field meanings: docs/pentax_k200d_architecture_gemeni.md
 * section 2.2. Knows nothing about SCSI/BOT transport; pentax_service.c
 * fetches the raw bytes and hands them to pentax_parse_k200d_status().
 *
 * A few fields (exposure_mode encoding, af_area_select encoding, the 0x104/
 * 0x108 metering pair) are flagged in the architecture doc itself as
 * unconfirmed on real hardware; they are exposed here as the raw camera
 * value with no reinterpretation, so callers must treat them as opaque
 * until verified.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PENTAX_STATUS_LEN 408u

typedef struct {
    uint16_t bufmask;

    uint32_t set_shutter_nom, set_shutter_denom;
    uint32_t set_aperture_nom, set_aperture_denom;
    int32_t  ev_comp_nom, ev_comp_denom;

    uint32_t iso;
    uint32_t auto_iso_min, auto_iso_max;

    uint32_t image_format;       /* 0:JPEG 1:RAW 2:RAW+JPEG */
    uint32_t jpeg_res_idx;       /* 0:10M 1:6M 2:2M */
    uint32_t jpeg_stars;         /* 1..3, converted from the camera's hw value (3 - hw) */
    uint32_t raw_format;         /* 0:PEF 1:DNG */
    uint32_t custom_image_tone;  /* 0:Bright 1:Natural ... 5:Mono */
    int32_t  saturation, sharpness, contrast; /* -4..+4, converted from hw value (hw - 4) */
    int32_t  jpeg_hue;           /* -4..+4, converted from hw value (hw - 4) */

    uint32_t exposure_mode;      /* raw camera value; mapping to P/Tv/Av/M/Sv unconfirmed */
    uint32_t af_mode;            /* 1:AF.S 2:AF.C */
    uint32_t af_area_select;     /* raw camera value; see header comment */
    uint32_t af_point_mask;      /* 11-bit mask, bit i = AF point i selected */
    uint32_t drive_mode;
    bool     shake_reduction;

    uint32_t meter_value_nom, meter_value_denom; /* current metering reading; meaning unconfirmed */

    uint32_t lens_max_aperture_nom, lens_max_aperture_denom; /* 0x13C/0x140: widest aperture (smallest F-number) */
    uint32_t lens_min_aperture_nom, lens_min_aperture_denom; /* 0x144/0x148: smallest aperture (largest F-number) */
    uint32_t focused_af_point;
    uint32_t focal_length_nom, focal_length_denom;
    int32_t  focus_distance;
} k200d_status_t;

/* Parses a PENTAX_STATUS_LEN-byte (or longer) big-endian status buffer.
 * Returns false if `len` is too short to cover the fields above. */
bool pentax_parse_k200d_status(const uint8_t *buf, size_t len, k200d_status_t *out);

#ifdef __cplusplus
}
#endif
