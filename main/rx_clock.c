#include "rx_clock.h"

#include "esp_clock_output.h"
#include "esp_rom_gpio.h"
#include "modem/modem_syscon_reg.h"
#include "soc/clk_tree_defs.h"
#include "soc/gpio_sig_map.h"
#include "soc/soc.h"

#if CONFIG_C5VRX2_PARLIO_RX_CLOCK_PLL_F40
static esp_clock_output_mapping_handle_t s_clock_output;
#elif CONFIG_C5VRX2_PARLIO_RX_CLOCK_MODEM_DEBUG40
static uint32_t s_saved_test_conf;
static bool s_saved_test_conf_valid;
#endif

bool c5vrx2_rx_clock_is_external(void)
{
#if CONFIG_C5VRX2_PARLIO_RX_CLOCK_PLL_F40 || \
    CONFIG_C5VRX2_PARLIO_RX_CLOCK_MODEM_DEBUG40
    return true;
#else
    return false;
#endif
}

const char *c5vrx2_rx_clock_name(void)
{
#if CONFIG_C5VRX2_PARLIO_RX_CLOCK_PLL_F40
    return "pll-f40-gpio2";
#elif CONFIG_C5VRX2_PARLIO_RX_CLOCK_MODEM_DEBUG40
    return "modem-debug40-gpio2";
#else
    return "parlio-internal-f40";
#endif
}

uint32_t c5vrx2_rx_clock_diag_signal(void)
{
#if CONFIG_C5VRX2_PARLIO_RX_CLOCK_MODEM_DEBUG40
    return CONFIG_C5VRX2_MODEM_DEBUG40_DIAG_SIGNAL;
#else
    return UINT32_MAX;
#endif
}

esp_err_t c5vrx2_rx_clock_start(void)
{
#if CONFIG_C5VRX2_PARLIO_RX_CLOCK_PLL_F40
    return esp_clock_output_start(CLKOUT_SIG_PLL_F40M,
                                  C5VRX2_RX_CLOCK_GPIO, &s_clock_output);
#elif CONFIG_C5VRX2_PARLIO_RX_CLOCK_MODEM_DEBUG40
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << C5VRX2_RX_CLOCK_GPIO,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) return err;

    s_saved_test_conf = REG_READ(MODEM_SYSCON_TEST_CONF_REG);
    s_saved_test_conf_valid = true;
    uint32_t test_conf = s_saved_test_conf;
    test_conf &= ~(MODEM_SYSCON_FPGA_DEBUG_CLK80_M |
                   MODEM_SYSCON_FPGA_DEBUG_CLK20_M |
                   MODEM_SYSCON_FPGA_DEBUG_CLK10_M);
    test_conf |= MODEM_SYSCON_FPGA_DEBUG_CLKSWITCH_M |
                 MODEM_SYSCON_FPGA_DEBUG_CLK40_M;
    REG_WRITE(MODEM_SYSCON_TEST_CONF_REG, test_conf);
    esp_rom_gpio_connect_out_signal(
        C5VRX2_RX_CLOCK_GPIO,
        MODEM_DIAG0_IDX + CONFIG_C5VRX2_MODEM_DEBUG40_DIAG_SIGNAL,
        false, false);
    __asm__ __volatile__("fence iorw, iorw" ::: "memory");
    return REG_READ(MODEM_SYSCON_TEST_CONF_REG) == test_conf ?
           ESP_OK : ESP_ERR_INVALID_RESPONSE;
#else
    return ESP_OK;
#endif
}

void c5vrx2_rx_clock_stop(void)
{
#if CONFIG_C5VRX2_PARLIO_RX_CLOCK_PLL_F40
    if (s_clock_output) {
        (void)esp_clock_output_stop(s_clock_output);
        s_clock_output = NULL;
    }
#elif CONFIG_C5VRX2_PARLIO_RX_CLOCK_MODEM_DEBUG40
    if (s_saved_test_conf_valid) {
        REG_WRITE(MODEM_SYSCON_TEST_CONF_REG, s_saved_test_conf);
        __asm__ __volatile__("fence iorw, iorw" ::: "memory");
        s_saved_test_conf_valid = false;
    }
    (void)gpio_reset_pin(C5VRX2_RX_CLOCK_GPIO);
#endif
}
