#include "parlio_direct.h"

#include <stdint.h>

#include "driver/parlio_tx.h"
#include "hal/parlio_ll.h"
#include "soc/parl_io_struct.h"

#include "rf_dump.h"
#include "wbfm_direct.h"

static parlio_tx_unit_handle_t s_tx;
static bitscrambler_handle_t s_bs;

esp_err_t c5vrx2_parlio_direct_prepare(void)
{
    if (s_tx || s_bs) return ESP_ERR_INVALID_STATE;
    esp_err_t err = c5vrx2_wbfm_direct_create(&s_bs);
    if (err != ESP_OK) return err;

    /* XIAO ESP32-C5 D4..D9, LSB-first DAC ordering proven by C5VRX. */
    const parlio_tx_unit_config_t cfg = {
        .clk_src = PARLIO_CLK_SRC_DEFAULT,
        .clk_in_gpio_num = -1,
        .input_clk_src_freq_hz = 0,
        .output_clk_freq_hz = 20000000u,
        .data_width = 8,
        .data_gpio_nums = {23, 24, 11, 12, 8, 9, -1, -1},
        .clk_out_gpio_num = -1,
        .valid_gpio_num = -1,
        .valid_start_delay = 0,
        .valid_stop_delay = 0,
        .trans_queue_depth = 1,
        .max_transfer_size = C5VRX2_RF_WORDS * sizeof(uint32_t),
        .dma_burst_size = 32,
        .shift_edge = PARLIO_SHIFT_EDGE_NEG,
        .bit_pack_order = PARLIO_BIT_PACK_ORDER_LSB,
    };
    if ((err = parlio_new_tx_unit(&cfg, &s_tx)) != ESP_OK) goto fail;
    if ((err = parlio_tx_unit_enable(s_tx)) != ESP_OK) goto fail;

    /* Driver enable starts the source clock; quiesce it immediately. The LP
     * producer is the sole owner of the RF/PARLIO start boundary. */
    parlio_ll_enable_interrupt(&PARL_IO, PARLIO_LL_EVENT_TX_FIFO_EMPTY, false);
    parlio_ll_tx_enable_clock(&PARL_IO, false);
    parlio_ll_clear_interrupt_status(&PARL_IO, PARLIO_LL_EVENT_TX_FIFO_EMPTY);

    const parlio_transmit_config_t tx_cfg = {
        .idle_value = 14u,
        .bitscrambler_program = NULL,
        .flags.loop_transmission = true,
    };
    err = parlio_tx_unit_transmit(
        s_tx,
        (const void *)(uintptr_t)C5VRX2_RF_DUMP_BASE,
        C5VRX2_RF_WORDS * sizeof(uint32_t) * 8u,
        &tx_cfg);

    /* transmit() also starts its clock. Pause again; descriptors, GDMA and the
     * BitScrambler remain armed/backpressured until LP has real writer lead. */
    parlio_ll_tx_enable_clock(&PARL_IO, false);
    parlio_ll_clear_interrupt_status(&PARL_IO, PARLIO_LL_EVENT_TX_FIFO_EMPTY);
    if (err != ESP_OK) goto fail;
    return ESP_OK;

fail:
    c5vrx2_parlio_direct_destroy();
    return err;
}

void c5vrx2_parlio_direct_destroy(void)
{
    parlio_ll_tx_enable_clock(&PARL_IO, false);
    if (s_tx) {
        (void)parlio_tx_unit_disable(s_tx);
        (void)parlio_del_tx_unit(s_tx);
        s_tx = NULL;
    }
    c5vrx2_wbfm_direct_destroy(s_bs);
    s_bs = NULL;
}
