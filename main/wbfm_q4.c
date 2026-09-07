#include "wbfm_q4.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include "esp_heap_caps.h"

#include "calibration.h"

BITSCRAMBLER_PROGRAM(c5vrx2_wbfm_q4_2to1_program,
                    "c5vrx2_wbfm_q4_2to1");

#define LUT_ITEMS 1024u
#define LUT_BYTES (LUT_ITEMS * sizeof(uint16_t))
#define PI_F 3.14159265358979323846f

static float signed_bucket_center(unsigned code, unsigned bits)
{
    const unsigned width = 1u << (10u - bits);
    float center = (float)(code * width) + ((float)width - 1.0f) * 0.5f;
    if (center >= 512.0f) center -= 1024.0f;
    return center;
}

static int q4_phase8(unsigned packed)
{
    const float q = signed_bucket_center(packed & 0x0fu, 4u);
    const float i = signed_bucket_center(packed >> 4u, 4u);
    int phase8 = (int)lrintf(atan2f(q, i) *
                             (256.0f / (2.0f * PI_F)));
    if (phase8 < -128) phase8 = -128;
    if (phase8 > 127) phase8 = 127;
    return phase8;
}

static void build_lut(uint16_t lut[LUT_ITEMS])
{
    const c5vrx2_calibration_t *cal = c5vrx2_calibration_get();
    int gain = ((int)cal->discriminator_gain + 1) / 2;
    if (gain < 1) gain = 1;

    for (unsigned index = 0; index < LUT_ITEMS; ++index) {
        int phase = q4_phase8(index & 0xffu) * gain;
        if (cal->polarity == C5VRX2_POLARITY_PREVIOUS_MINUS_CURRENT)
            phase = -phase;
        const uint8_t phase_mod = (uint8_t)phase;
        const uint8_t biased_negative =
            (uint8_t)((unsigned)cal->pedestal_code - phase_mod);
        lut[index] = (uint16_t)phase_mod |
                     ((uint16_t)biased_negative << 8u);
    }
}

esp_err_t c5vrx2_wbfm_q4_configure(bitscrambler_handle_t handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    uint16_t *lut = heap_caps_malloc(LUT_BYTES, MALLOC_CAP_INTERNAL);
    if (!lut) return ESP_ERR_NO_MEM;
    build_lut(lut);
    esp_err_t err = bitscrambler_load_program(
        handle, c5vrx2_wbfm_q4_2to1_program);
    if (err == ESP_OK) err = bitscrambler_load_lut(handle, lut, LUT_BYTES);
    free(lut);
    return err;
}

const void *c5vrx2_wbfm_q4_program(void)
{
    return c5vrx2_wbfm_q4_2to1_program;
}
