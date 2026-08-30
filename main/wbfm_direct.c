#include "wbfm_direct.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include "driver/bitscrambler.h"
#include "esp_heap_caps.h"
#include "soc/soc_caps.h"

BITSCRAMBLER_PROGRAM(c5vrx2_wbfm_direct6_4to1_program,
                    "c5vrx2_wbfm_direct6_4to1");

#define LUT_WORDS 1024u
#define PI_F 3.14159265358979323846f
#define CVBS_ZERO_CODE 20u
#define CVBS_GAIN_SHIFT 1u
#define PHASE_BIAS (CVBS_ZERO_CODE << CVBS_GAIN_SHIFT)

static float coarse_center(unsigned code5)
{
    const int signed5 = (code5 & 0x10u) ? (int)code5 - 32 : (int)code5;
    return (float)signed5 * 32.0f + 15.5f;
}

static void build_phase8_lut(uint16_t lut[LUT_WORDS])
{
    for (unsigned i5 = 0; i5 < 32u; ++i5) {
        for (unsigned q5 = 0; q5 < 32u; ++q5) {
            float p = atan2f(coarse_center(q5), coarse_center(i5));
            if (p < 0.0f) p += 2.0f * PI_F;
            const uint8_t phase = (uint8_t)lrintf(p * (256.0f / (2.0f * PI_F)));
            /* Inverted wrapped discriminator: bias + previous phase - current
             * phase. Emitting bits 1..6 maps zero deviation to DAC code 20 at
             * 2x gain, leaving headroom above video while sync approaches 0. */
            lut[(i5 << 5) | q5] =
                (uint16_t)(uint8_t)(0u - phase) |
                ((uint16_t)(uint8_t)(PHASE_BIAS + phase) << 8);
        }
    }
}

esp_err_t c5vrx2_wbfm_direct_create(bitscrambler_handle_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    uint16_t *lut = heap_caps_malloc(LUT_WORDS * sizeof(uint16_t),
                                     MALLOC_CAP_INTERNAL);
    if (!lut) return ESP_ERR_NO_MEM;
    build_phase8_lut(lut);

    const bitscrambler_config_t cfg = {
        .dir = BITSCRAMBLER_DIR_TX,
        .attach_to = SOC_BITSCRAMBLER_ATTACH_PARL_IO,
    };
    bitscrambler_handle_t bs = NULL;
    esp_err_t err = bitscrambler_new(&cfg, &bs);
    if (err == ESP_OK) err = bitscrambler_enable(bs);
    if (err == ESP_OK)
        err = bitscrambler_load_program(bs, c5vrx2_wbfm_direct6_4to1_program);
    if (err == ESP_OK)
        err = bitscrambler_load_lut(bs, lut, LUT_WORDS * sizeof(uint16_t));
    if (err == ESP_OK) err = bitscrambler_reset(bs);
    if (err == ESP_OK) err = bitscrambler_start(bs);
    free(lut);

    if (err != ESP_OK) {
        if (bs) {
            (void)bitscrambler_disable(bs);
            bitscrambler_free(bs);
        }
        return err;
    }
    *out = bs;
    return ESP_OK;
}

void c5vrx2_wbfm_direct_destroy(bitscrambler_handle_t handle)
{
    if (!handle) return;
    (void)bitscrambler_disable(handle);
    bitscrambler_free(handle);
}
