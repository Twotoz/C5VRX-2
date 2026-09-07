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

static int scale_phase(int phase, unsigned calibration_gain)
{
    /* At the coherent 40-MS/s input the adjacent phase step is twice the
     * historical 80-MS/s step. Preserve the half-step represented by an
     * even calibration setting instead of truncating 3/2 back to 1. */
    const int numerator = (int)calibration_gain + 1;
    const int magnitude = abs(phase);
    const int scaled = (magnitude * numerator + 1) / 2;
    return phase < 0 ? -scaled : scaled;
}

static void build_lut(uint16_t lut[LUT_ITEMS])
{
    const c5vrx2_calibration_t *cal = c5vrx2_calibration_get();
    for (unsigned index = 0; index < LUT_ITEMS; ++index) {
        if (index < 0x100u) {
            int phase = scale_phase(q4_phase8(index),
                                    cal->discriminator_gain);
            if (cal->polarity == C5VRX2_POLARITY_PREVIOUS_MINUS_CURRENT)
                phase = -phase;
            const uint8_t phase_mod = (uint8_t)phase;
            const uint8_t negative_phase = (uint8_t)(0u - phase_mod);
            lut[index] = (uint16_t)phase_mod |
                         ((uint16_t)negative_phase << 8u);
        } else if (index < 0x200u) {
            int sum = (int)(index & 0xffu);
            if (sum >= 128) sum -= 256;
            int code = (int)cal->pedestal_code + sum / 2;
            if (code < 0) code = 0;
            if (code > 63) code = 63;
            lut[index] = (uint16_t)(uint8_t)code;
        } else {
            lut[index] = 0u;
        }
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
