#include "tinytest.h"
#include "pentax_status.h"

#include <string.h>

static void wr16(uint8_t *buf, size_t off, uint16_t v)
{
    buf[off] = (uint8_t)(v >> 8);
    buf[off + 1] = (uint8_t)v;
}

static void wr32(uint8_t *buf, size_t off, uint32_t v)
{
    buf[off] = (uint8_t)(v >> 24);
    buf[off + 1] = (uint8_t)(v >> 16);
    buf[off + 2] = (uint8_t)(v >> 8);
    buf[off + 3] = (uint8_t)v;
}

/* A buffer with every documented field set to a distinct, recognizable
 * value, so a field read from the wrong offset shows up immediately. */
static void build_reference_buffer(uint8_t buf[PENTAX_STATUS_LEN])
{
    memset(buf, 0, PENTAX_STATUS_LEN);
    wr16(buf, 0x16, 0x1234);

    wr32(buf, 0x2C, 1);
    wr32(buf, 0x30, 250);
    wr32(buf, 0x34, 56);
    wr32(buf, 0x38, 10);
    wr32(buf, 0x3C, (uint32_t)(int32_t)-2);
    wr32(buf, 0x40, 3);

    wr32(buf, 0x60, 400);
    wr32(buf, 0x64, 100);
    wr32(buf, 0x68, 1600);

    wr32(buf, 0x78, 1);
    wr32(buf, 0x7C, 2);
    wr32(buf, 0x80, 1);
    wr32(buf, 0x84, 1);
    wr32(buf, 0x88, 3);
    wr32(buf, 0x8C, 5);
    wr32(buf, 0x90, 2);
    wr32(buf, 0x94, 6);

    wr32(buf, 0xAC, 7);
    wr32(buf, 0xB8, 2);
    wr32(buf, 0xBC, 1);
    wr32(buf, 0xC0, 0x155);
    wr32(buf, 0xCC, 1);
    wr32(buf, 0xDA, 1);

    wr32(buf, 0xF4, 8);

    wr32(buf, 0x104, 11);
    wr32(buf, 0x108, 2);

    wr32(buf, 0x13C, 14);
    wr32(buf, 0x140, 10);
    wr32(buf, 0x144, 32);
    wr32(buf, 0x148, 10);

    wr32(buf, 0x150, 5);

    wr32(buf, 0x17C, 55);
    wr32(buf, 0x180, 1);
    wr32(buf, 0x184, 123);
}

static void test_reference_buffer_fields(void)
{
    uint8_t buf[PENTAX_STATUS_LEN];
    build_reference_buffer(buf);

    k200d_status_t s;
    CHECK(pentax_parse_k200d_status(buf, sizeof(buf), &s) == true);

    CHECK_EQ(s.bufmask, 0x1234);

    CHECK_EQ(s.set_shutter_nom, 1);
    CHECK_EQ(s.set_shutter_denom, 250);
    CHECK_EQ(s.set_aperture_nom, 56);
    CHECK_EQ(s.set_aperture_denom, 10);
    CHECK_EQ(s.ev_comp_nom, -2);
    CHECK_EQ(s.ev_comp_denom, 3);

    CHECK_EQ(s.iso, 400);
    CHECK_EQ(s.auto_iso_min, 100);
    CHECK_EQ(s.auto_iso_max, 1600);

    CHECK_EQ(s.image_format, 1);
    CHECK_EQ(s.jpeg_res_idx, 2);
    CHECK_EQ(s.jpeg_stars, 2);     /* 3 - hw(1) = 2 */
    CHECK_EQ(s.raw_format, 1);
    CHECK_EQ(s.custom_image_tone, 3);
    CHECK_EQ(s.saturation, 1);     /* hw(5) - 4 */
    CHECK_EQ(s.sharpness, -2);     /* hw(2) - 4 */
    CHECK_EQ(s.contrast, 2);       /* hw(6) - 4 */

    CHECK_EQ(s.exposure_mode, 7);
    CHECK_EQ(s.af_mode, 2);
    CHECK_EQ(s.af_area_select, 1);
    CHECK_EQ(s.af_point_mask, 0x155);
    CHECK_EQ(s.drive_mode, 1);
    CHECK(s.shake_reduction == true);

    CHECK_EQ(s.jpeg_hue, 4);       /* hw(8) - 4 */

    CHECK_EQ(s.meter_value_nom, 11);
    CHECK_EQ(s.meter_value_denom, 2);

    CHECK_EQ(s.lens_max_aperture_nom, 14);
    CHECK_EQ(s.lens_max_aperture_denom, 10);
    CHECK_EQ(s.lens_min_aperture_nom, 32);
    CHECK_EQ(s.lens_min_aperture_denom, 10);
    CHECK_EQ(s.focused_af_point, 5);

    CHECK_EQ(s.focal_length_nom, 55);
    CHECK_EQ(s.focal_length_denom, 1);
    CHECK_EQ(s.focus_distance, 123);
}

static void test_shake_reduction_boolean_coercion(void)
{
    uint8_t buf[PENTAX_STATUS_LEN];
    k200d_status_t s;

    build_reference_buffer(buf);
    wr32(buf, 0xDA, 0);
    CHECK(pentax_parse_k200d_status(buf, sizeof(buf), &s) == true);
    CHECK(s.shake_reduction == false);

    wr32(buf, 0xDA, 0xFFFFFFFFu);
    CHECK(pentax_parse_k200d_status(buf, sizeof(buf), &s) == true);
    CHECK(s.shake_reduction == true);
}

/* jpeg_stars = 3 - hw, clamped to the empty/invalid state (0) outside 1..3 */
static void test_jpeg_stars_clamping(void)
{
    static const struct { uint32_t hw; uint32_t expect; } cases[] = {
        {0, 3}, {1, 2}, {2, 1}, {3, 0}, {4, 0}, {5, 0},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t buf[PENTAX_STATUS_LEN];
        build_reference_buffer(buf);
        wr32(buf, 0x80, cases[i].hw);
        k200d_status_t s;
        CHECK(pentax_parse_k200d_status(buf, sizeof(buf), &s) == true);
        CHECK_EQ(s.jpeg_stars, cases[i].expect);
    }
}

static void test_signed_field_conversion_range(void)
{
    static const struct { uint32_t hw; int32_t expect; } cases[] = {
        {0, -4}, {4, 0}, {8, 4}, {9, 5},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t buf[PENTAX_STATUS_LEN];
        build_reference_buffer(buf);
        wr32(buf, 0x8C, cases[i].hw); /* saturation */
        k200d_status_t s;
        CHECK(pentax_parse_k200d_status(buf, sizeof(buf), &s) == true);
        CHECK_EQ(s.saturation, cases[i].expect);
    }
}

/* --- Fault injection: truncated / malformed buffers --- */

static void test_rejects_short_buffer(void)
{
    uint8_t buf[PENTAX_STATUS_LEN];
    build_reference_buffer(buf);

    k200d_status_t s;
    memset(&s, 0xAA, sizeof(s)); /* sentinel: must stay untouched on failure */

    CHECK(pentax_parse_k200d_status(buf, 0, &s) == false);
    CHECK(pentax_parse_k200d_status(buf, 1, &s) == false);
    CHECK(pentax_parse_k200d_status(buf, PENTAX_STATUS_LEN - 1, &s) == false);

    /* Confirm the "reject before touching *out*" contract: callers that
     * check the return value before reading `s` are safe either way, but a
     * caller that doesn't should still see unmodified memory, not a
     * half-zeroed struct. */
    uint8_t *raw = (uint8_t *)&s;
    bool all_sentinel = true;
    for (size_t i = 0; i < sizeof(s); i++) {
        if (raw[i] != 0xAA) {
            all_sentinel = false;
            break;
        }
    }
    CHECK(all_sentinel == true);
}

static void test_accepts_exact_and_longer_buffer(void)
{
    uint8_t buf[PENTAX_STATUS_LEN + 16];
    memset(buf, 0, sizeof(buf));
    build_reference_buffer(buf);
    /* garbage past the documented region must not affect parsing */
    memset(buf + PENTAX_STATUS_LEN, 0xFF, 16);

    k200d_status_t s;
    CHECK(pentax_parse_k200d_status(buf, PENTAX_STATUS_LEN, &s) == true);
    CHECK_EQ(s.iso, 400);

    CHECK(pentax_parse_k200d_status(buf, sizeof(buf), &s) == true);
    CHECK_EQ(s.iso, 400);
}

static void test_all_zero_buffer_parses_as_zeroed_struct(void)
{
    uint8_t buf[PENTAX_STATUS_LEN];
    memset(buf, 0, sizeof(buf));

    k200d_status_t s;
    memset(&s, 0xAA, sizeof(s));
    CHECK(pentax_parse_k200d_status(buf, sizeof(buf), &s) == true);
    CHECK_EQ(s.iso, 0);
    CHECK_EQ(s.jpeg_stars, 3);   /* hw=0 -> 3-0=3, a valid star count */
    CHECK_EQ(s.saturation, -4);
    CHECK(s.shake_reduction == false);
}

int main(void)
{
    RUN(test_reference_buffer_fields);
    RUN(test_shake_reduction_boolean_coercion);
    RUN(test_jpeg_stars_clamping);
    RUN(test_signed_field_conversion_range);
    RUN(test_rejects_short_buffer);
    RUN(test_accepts_exact_and_longer_buffer);
    RUN(test_all_zero_buffer_parses_as_zeroed_struct);
    return tt_summary();
}
