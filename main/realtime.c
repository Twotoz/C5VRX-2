#include "realtime.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_bit_defs.h"
#include "esp_log.h"
#include "hal/apm_hal.h"
#include "soc/apm_defs.h"
#include "ulp_lp_core.h"

#include "c5vrx2_lp.h"
#include "parlio_direct.h"

#define CMD_CONTINUOUS 1u
#define STATE_READY    1u
#define STATE_RUNNING  2u
#define STATE_ERROR    3u
#define STATE_STOPPED  4u

static const char *TAG = "c5vrx2_rt";
static portMUX_TYPE s_park_mux = portMUX_INITIALIZER_UNLOCKED;

static bool IRAM_ATTR park_hp_until_terminal(void)
{
    bool saw_running = false;
    portENTER_CRITICAL(&s_park_mux);
    ulp_c5vrx2_command = CMD_CONTINUOUS;
    for (;;) {
        const uint32_t state = ulp_c5vrx2_state;
        if (state == STATE_RUNNING) saw_running = true;
        if (state == STATE_ERROR || state == STATE_STOPPED) break;
    }
    portEXIT_CRITICAL(&s_park_mux);
    return saw_running;
}

esp_err_t c5vrx2_realtime_start(void)
{
    esp_err_t err = ulp_lp_core_load_binary(
        c5vrx2_lp_bin_start,
        (size_t)(c5vrx2_lp_bin_end - c5vrx2_lp_bin_start));
    if (err != ESP_OK) return err;

    const ulp_lp_core_cfg_t lp_cfg = {
        .wakeup_source = ULP_LP_CORE_WAKEUP_SOURCE_HP_CPU,
    };
    err = ulp_lp_core_run((ulp_lp_core_cfg_t *)&lp_cfg);
    if (err != ESP_OK) return err;

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(100u);
    while (ulp_c5vrx2_state != STATE_READY && xTaskGetTickCount() < deadline)
        vTaskDelay(1);
    if (ulp_c5vrx2_state != STATE_READY) return ESP_ERR_TIMEOUT;

    /* LP touches only the timing-critical registers required to own/rearm the
     * dump writer and release the already-prepared PARLIO clock. */
    const uint64_t lp_rw =
        BIT64(APM_TEE_HP_PERIPH_MODEM) |
        BIT64(APM_TEE_HP_PERIPH_SYSTEM_REG) |
        BIT64(APM_TEE_HP_PERIPH_PCR_REG) |
        BIT64(APM_TEE_HP_PERIPH_PARL_IO);
    apm_hal_set_master_sec_mode(BIT(APM_MASTER_LPCORE), APM_SEC_MODE_REE0);
    apm_hal_tee_set_peri_access(APM_TEE_CTRL_HP, lp_rw,
                                APM_SEC_MODE_REE0, APM_PERM_R | APM_PERM_W);

    /* Mount the looping RF-SRAM -> BitScrambler -> PARLIO transaction while
     * the RF window is still CPU-owned. Its source clock remains paused. */
    err = c5vrx2_parlio_direct_prepare();
    if (err != ESP_OK) return err;

    ESP_LOGW(TAG,
             "REALTIME ARM: A1 IQ -> direct phase-delta -> 20MS/s PARLIO; HP parks, USB is not in the datapath");

    /* Once LP lends 0x40830000..0x4083ffff to MAC dump, HP must not run normal
     * scheduler/interrupt code. Healthy realtime service stays in this IRAM
     * loop indefinitely; reset/power-cycle is the normal test stop action. */
    const bool ran = park_hp_until_terminal();

    /* A terminal LP state restores SRAM ownership before HP leaves the park. */
    c5vrx2_parlio_direct_destroy();
    ESP_LOGE(TAG,
             "REALTIME STOP state=%u ran=%u blocks=%u rearms=%u failures=%u gap_last=%u gap_max=%u fault=%u addr=0x%08x pc=0x%08x",
             (unsigned)ulp_c5vrx2_state, ran ? 1u : 0u,
             (unsigned)ulp_c5vrx2_blocks,
             (unsigned)ulp_c5vrx2_rearms,
             (unsigned)ulp_c5vrx2_rearm_failures,
             (unsigned)ulp_c5vrx2_gap_cycles_last,
             (unsigned)ulp_c5vrx2_gap_cycles_max,
             (unsigned)ulp_c5vrx2_fault_cause,
             (unsigned)ulp_c5vrx2_fault_address,
             (unsigned)ulp_c5vrx2_fault_pc);
    return ran ? ESP_FAIL : ESP_ERR_INVALID_STATE;
}
