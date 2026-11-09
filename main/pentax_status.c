#include "pentax_status.h"

#include <string.h>

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

bool pentax_parse_k200d_status(const uint8_t *buf, size_t len, k200d_status_t *out)
{
    if (len < PENTAX_STATUS_LEN) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    out->bufmask = rd16(buf + 0x16);

    out->set_shutter_nom    = rd32(buf + 0x2C);
    out->set_shutter_denom  = rd32(buf + 0x30);
    out->set_aperture_nom   = rd32(buf + 0x34);
    out->set_aperture_denom = rd32(buf + 0x38);
    out->ev_comp_nom        = (int32_t)rd32(buf + 0x3C);
    out->ev_comp_denom      = (int32_t)rd32(buf + 0x40);

    out->iso          = rd32(buf + 0x60);
    out->auto_iso_min = rd32(buf + 0x64);
    out->auto_iso_max = rd32(buf + 0x68);

    out->image_format      = rd32(buf + 0x78);
    out->jpeg_res_idx       = rd32(buf + 0x7C);
    {
        int32_t stars = 3 - (int32_t)rd32(buf + 0x80);
        out->jpeg_stars = (stars >= 1 && stars <= 3) ? (uint32_t)stars : 0u;
    }
    out->raw_format         = rd32(buf + 0x84);
    out->custom_image_tone  = rd32(buf + 0x88);
    out->saturation         = (int32_t)rd32(buf + 0x8C) - 4;
    out->sharpness          = (int32_t)rd32(buf + 0x90) - 4;
    out->contrast           = (int32_t)rd32(buf + 0x94) - 4;

    out->exposure_mode = rd32(buf + 0xAC);
    out->af_mode        = rd32(buf + 0xB8);
    out->af_area_select = rd32(buf + 0xBC);
    out->af_point_mask  = rd32(buf + 0xC0);
    out->drive_mode      = rd32(buf + 0xCC);
    out->shake_reduction = rd32(buf + 0xDA) != 0;

    out->jpeg_hue = (int32_t)rd32(buf + 0xF4) - 4;

    out->meter_value_nom   = rd32(buf + 0x104);
    out->meter_value_denom = rd32(buf + 0x108);

    out->lens_max_aperture_nom   = rd32(buf + 0x13C);
    out->lens_max_aperture_denom = rd32(buf + 0x140);
    out->lens_min_aperture_nom   = rd32(buf + 0x144);
    out->lens_min_aperture_denom = rd32(buf + 0x148);

    out->focused_af_point = rd32(buf + 0x150);

    out->focal_length_nom   = rd32(buf + 0x17C);
    out->focal_length_denom = rd32(buf + 0x180);
    out->focus_distance     = (int32_t)rd32(buf + 0x184);

    return true;
}
