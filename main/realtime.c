#include "realtime.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/bitscrambler.h"
#include "driver/gpio.h"
#include "driver/parlio_rx.h"
#include "driver/parlio_tx.h"
#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_log.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/gpio_sig_map.h"
#include "soc/soc_caps.h"

#include "calibration.h"
#include "continuous_iq.h"
#include "wbfm_q4.h"

#define MODEM_IQ_RATE_HZ 40000000u
#define CVBS_RATE_HZ     20000000u
#define CVBS_RING_BYTES      8192u

#define DUMP_CTRL       0x600a9004u
#define DUMP_PTR_MODE   0x600a9008u
#define CTRL_ENABLE     0x80000000u
#define CTRL_DONE       0x00040000u
#define PTR_MASK        0x00003fffu

static const char *TAG = "c5vrx2_rt";

/* Q[9:6], then I[9:6]. These MODEM_DIAG mappings were correlated against
 * the post-stop Q10/I10 ring on physical ESP32-C5 hardware. */
static const gpio_num_t s_iq_pins[8] = {
    GPIO_NUM_1, GPIO_NUM_0, GPIO_NUM_25, GPIO_NUM_7,
    GPIO_NUM_10, GPIO_NUM_5, GPIO_NUM_3, GPIO_NUM_4,
};
static const uint8_t s_iq_diag[8] = {6u, 7u, 8u, 9u, 16u, 17u, 18u, 19u};

/* RX-GDMA writes the BitScrambler's 20-MS/s real output here while TX-GDMA
 * reads the same ring at the exact same PLL-derived rate. 8192 bytes provide
 * about 410 us of elastic storage without adding a frame buffer. */
static DMA_ATTR __attribute__((aligned(64))) uint8_t s_cvbs_ring[CVBS_RING_BYTES];

static parlio_rx_unit_handle_t s_rx;
static parlio_rx_delimiter_handle_t s_rx_delimiter;
static parlio_tx_unit_handle_t s_tx;
static bitscrambler_handle_t s_rx_bs;

static inline uint32_t reg32(uint32_t address)
{
    return *(volatile uint32_t *)(uintptr_t)address;
}

static esp_err_t route_modem_iq(void)
{
    uint64_t mask = 0u;
    for (unsigned lane = 0u; lane < 8u; ++lane) mask |= 1ULL << s_iq_pins[lane];
    const gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) return err;
    for (unsigned lane = 0u; lane < 8u; ++lane) {
        esp_rom_gpio_connect_out_signal(s_iq_pins[lane],
                                        MODEM_DIAG0_IDX + s_iq_diag[lane],
                                        false, false);
    }
    __asm__ __volatile__("fence iorw, iorw" ::: "memory");
    return ESP_OK;
}

static esp_err_t prepare_rx(void)
{
    const parlio_rx_unit_config_t cfg = {
        .trans_queue_depth = 1u,
        .max_recv_size = sizeof(s_cvbs_ring),
        .dma_burst_size = 32u,
        .data_width = 8u,
        .clk_src = PARLIO_CLK_SRC_DEFAULT,
        .ext_clk_freq_hz = 0u,
        .exp_clk_freq_hz = MODEM_IQ_RATE_HZ,
        .clk_in_gpio_num = -1,
        .clk_out_gpio_num = -1,
        .valid_gpio_num = -1,
        .data_gpio_nums = {
            GPIO_NUM_1, GPIO_NUM_0, GPIO_NUM_25, GPIO_NUM_7,
            GPIO_NUM_10, GPIO_NUM_5, GPIO_NUM_3, GPIO_NUM_4,
        },
        .flags = {
            .free_clk = true,
            .clk_gate_en = false,
            .allow_pd = false,
        },
    };
    esp_err_t err = parlio_new_rx_unit(&cfg, &s_rx);
    if (err != ESP_OK) return err;

    const parlio_rx_soft_delimiter_config_t delimiter_cfg = {
        .sample_edge = PARLIO_SAMPLE_EDGE_POS,
        .bit_pack_order = PARLIO_BIT_PACK_ORDER_LSB,
        .eof_data_len = 0u,
        .timeout_ticks = 0u,
    };
    err = parlio_new_rx_soft_delimiter(&delimiter_cfg, &s_rx_delimiter);
    if (err != ESP_OK) return err;

    const bitscrambler_config_t bs_cfg = {
        .dir = BITSCRAMBLER_DIR_RX,
        .attach_to = SOC_BITSCRAMBLER_ATTACH_PARL_IO,
    };
    err = bitscrambler_new(&bs_cfg, &s_rx_bs);
    if (err != ESP_OK) return err;
    if ((err = bitscrambler_enable(s_rx_bs)) != ESP_OK) return err;
    if ((err = c5vrx2_wbfm_q4_configure(s_rx_bs)) != ESP_OK) return err;
    if ((err = bitscrambler_reset(s_rx_bs)) != ESP_OK) return err;
    if ((err = bitscrambler_start(s_rx_bs)) != ESP_OK) return err;
    return parlio_rx_unit_enable(s_rx, true);
}

static esp_err_t prepare_tx(void)
{
    const parlio_tx_unit_config_t cfg = {
        .clk_src = PARLIO_CLK_SRC_DEFAULT,
        .clk_in_gpio_num = -1,
        .input_clk_src_freq_hz = 0u,
        .output_clk_freq_hz = CVBS_RATE_HZ,
        .data_width = 8u,
        .data_gpio_nums = {23, 24, 11, 12, 8, 9, -1, -1},
        .clk_out_gpio_num = -1,
        .valid_gpio_num = -1,
        .valid_start_delay = 0,
        .valid_stop_delay = 0,
        .trans_queue_depth = 1u,
        .max_transfer_size = sizeof(s_cvbs_ring),
        .dma_burst_size = 32u,
        .shift_edge = PARLIO_SHIFT_EDGE_NEG,
        .bit_pack_order = PARLIO_BIT_PACK_ORDER_LSB,
    };
    esp_err_t err = parlio_new_tx_unit(&cfg, &s_tx);
    if (err != ESP_OK) return err;
    return parlio_tx_unit_enable(s_tx);
}

static esp_err_t start_rx_ring(void)
{
    esp_err_t err = parlio_rx_soft_delimiter_start_stop(
        s_rx, s_rx_delimiter, true);
    if (err != ESP_OK) return err;
    const parlio_receive_config_t cfg = {
        .delimiter = s_rx_delimiter,
        .flags = {
            .partial_rx_en = true,
            .indirect_mount = false,
        },
    };
    return parlio_rx_unit_receive(s_rx, s_cvbs_ring, sizeof(s_cvbs_ring),
                                  &cfg);
}

static esp_err_t start_tx_ring(void)
{
    const c5vrx2_calibration_t *cal = c5vrx2_calibration_get();
    const parlio_transmit_config_t cfg = {
        .idle_value = cal->pedestal_code,
        .bitscrambler_program = NULL,
        .flags.loop_transmission = true,
    };
    return parlio_tx_unit_transmit(s_tx, s_cvbs_ring,
                                   sizeof(s_cvbs_ring) * 8u, &cfg);
}

static void telemetry_task(void *argument)
{
    (void)argument;
    uint32_t previous = reg32(DUMP_PTR_MODE) & PTR_MASK;
    uint32_t stalls = 0u;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        const uint32_t current = reg32(DUMP_PTR_MODE) & PTR_MASK;
        const uint32_t control = reg32(DUMP_CTRL);
        if (current == previous) stalls++;
        previous = current;
        ESP_LOGI(TAG,
                 "LIVE iq_in=40M cvbs_out=20M ptr=%u enable=%u done=%u "
                 "stalls=%u starts=1 rearms=0",
                 (unsigned)current, (control & CTRL_ENABLE) != 0u,
                 (control & CTRL_DONE) != 0u, (unsigned)stalls);
    }
}

esp_err_t c5vrx2_realtime_start(void)
{
    const c5vrx2_calibration_t *cal = c5vrx2_calibration_get();
    memset(s_cvbs_ring, cal->pedestal_code, sizeof(s_cvbs_ring));
    (void)esp_cache_msync(s_cvbs_ring, sizeof(s_cvbs_ring),
                          ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    esp_err_t err = route_modem_iq();
    if (err != ESP_OK) return err;
    if ((err = prepare_rx()) != ESP_OK) return err;
    if ((err = prepare_tx()) != ESP_OK) return err;

    /* continuous_iq_start() performs the one-time RF setup and a 4-ms rate
     * measurement. Start the AV ring only afterwards, otherwise RX would
     * lap the buffer an unknown number of times before TX gets its phase
     * offset. This ordering does not rearm or interrupt the RF producer. */
    if ((err = continuous_iq_start()) != ESP_OK) return err;
    if ((err = start_rx_ring()) != ESP_OK) return err;

    /* Put the 20-MS/s TX consumer half a ring behind RX-GDMA. Both PARLIO
     * dividers share the same PLL source, so the distance remains fixed. */
    esp_rom_delay_us((CVBS_RING_BYTES / 2u) * 1000000u / CVBS_RATE_HZ);
    if ((err = start_tx_ring()) != ESP_OK) return err;

    BaseType_t created = xTaskCreate(telemetry_task, "iq_av_stat", 3072,
                                     NULL, 1u, NULL);
    if (created != pdPASS) return ESP_ERR_NO_MEM;

    ESP_LOGW(TAG,
             "LIVE ACTIVE: MODEM 80M -> coherent /2 Q4/I4 40M -> adjacent "
             "FM -> real LPF /2 -> CVBS 20M -> 6-bit DAC; measured_rf=%u "
             "pedestal=%u gain=%u polarity=%u",
             (unsigned)continuous_iq_sample_rate_hz(),
             cal->pedestal_code, cal->discriminator_gain,
             (unsigned)cal->polarity);
    return ESP_OK;
}
