#include "wbfm_direct.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include "driver/bitscrambler.h"
#include "esp_heap_caps.h"
#include "soc/soc_caps.h"

#include "calibration.h"

BITSCRAMBLER_PROGRAM(c5vrx2_wbfm_direct6_4to1_program,
                    "c5vrx2_wbfm_direct6_4to1");

#define LUT_BYTES 2048u
#define PI_F 3.14159265358979323846f

/* Top-two-bit Q/I quantization keeps the complete adjacent-sample operation
 * in one 8-bit LUT lookup.  The LUT address is current[3:0], previous[3:0],
 * plus three replicated don't-care address bits. */
static float signed2_center(unsigned code)
{
    static const float centers[4] = {127.5f, 383.5f, -384.5f, -128.5f};
    return centers[code & 3u];
}

static float compact_phase(unsigned compact)
{
    const float q = signed2_center(compact & 3u);
    const float i = signed2_center((compact >> 2u) & 3u);
    return atan2f(q, i);
}

static int adjacent_delta(unsigned previous, unsigned current)
{
    float delta = compact_phase(current) - compact_phase(previous);
    while (delta >= PI_F) delta -= 2.0f * PI_F;
    while (delta < -PI_F) delta += 2.0f * PI_F;
    int phase8 = (int)lrintf(delta * (256.0f / (2.0f * PI_F)));
    if (phase8 < -128) phase8 = -128;
    if (phase8 > 127) phase8 = 127;
    return phase8;
}

static void build_adjacent_lut(uint8_t lut[LUT_BYTES])
{
    const c5vrx2_calibration_t *cal = c5vrx2_calibration_get();
    const int pedestal_correction = (int)cal->pedestal_code - 20;

    for (unsigned current = 0; current < 16u; ++current) {
        for (unsigned previous = 0; previous < 16u; ++previous) {
            int delta = adjacent_delta(previous, current);
            if (cal->polarity == C5VRX2_POLARITY_PREVIOUS_MINUS_CURRENT)
                delta = -delta;

            /* FM first: every adjacent phase step contributes at full
             * precision.  A2..A7 divides the sum of four real discriminator
             * samples by four only when producing the AV-rate sample. */
            int contribution = delta * (int)cal->discriminator_gain;
            contribution += pedestal_correction;
            /* Counter A starts at 4*20.  Keep every term inside the legal
             * six-bit DAC excursion so the unsigned accumulator cannot wrap
             * before the final divide-by-four. */
            if (contribution < -20) contribution = -20;
            if (contribution > 43) contribution = 43;

            for (unsigned ignored = 0; ignored < 8u; ++ignored) {
                const unsigned index =
                    (current << 7u) | (previous << 3u) | ignored;
                lut[index] = (uint8_t)(int8_t)contribution;
            }
        }
    }
}

esp_err_t c5vrx2_wbfm_direct_create(bitscrambler_handle_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = NULL;

    uint8_t *lut = heap_caps_malloc(LUT_BYTES, MALLOC_CAP_INTERNAL);
    if (!lut) return ESP_ERR_NO_MEM;
    build_adjacent_lut(lut);

    const bitscrambler_config_t cfg = {
        .dir = BITSCRAMBLER_DIR_TX,
        .attach_to = SOC_BITSCRAMBLER_ATTACH_PARL_IO,
    };
    bitscrambler_handle_t bs = NULL;
    esp_err_t err = bitscrambler_new(&cfg, &bs);
    if (err == ESP_OK) err = bitscrambler_enable(bs);
    if (err == ESP_OK)
        err = bitscrambler_load_program(bs,
                                        c5vrx2_wbfm_direct6_4to1_program);
    if (err == ESP_OK) err = bitscrambler_load_lut(bs, lut, LUT_BYTES);
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
