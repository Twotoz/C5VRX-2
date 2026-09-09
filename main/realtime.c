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
#include "regdma_rearm.h"

/* ulp_embed_binary() exposes the LP image through linker symbols, matching the
 * proven C5VRX LP-core donor. The generated c5vrx2_lp.h only declares the LP
 * shared variables, not these binary bounds. */
extern const uint8_t c5vrx2_lp_bin_start[]
    asm("_binary_c5vrx2_lp_bin_start");
extern const uint8_t c5vrx2_lp_bin_end[]
    asm("_binary_c5vrx2_lp_bin_end");

#define CMD_CONTINUOUS 1u
#define STATE_READY    1u
#define STATE_RUNNING  2u
#define STATE_ERROR    3u
#define STATE_STOPPED  4u
#define TELEMETRY_MAGIC 0x43355232u

static const char *TAG = "c5vrx2_rt";

static bool IRAM_ATTR __attribute__((noinline)) park_hp_until_terminal(void)
{
    const uint32_t mie = 0x8u;
    uint32_t saved_mstatus;
    bool saw_running = false;

    /* FreeRTOS critical sections leave higher-level CLIC interrupts eligible.
     * Once LP grants the RF writer HP SRAM, even one such ISR can touch flash,
     * a driver object or a reassigned SRAM bank. Mask MSTATUS.MIE directly and
     * make no calls until LP has restored ownership on a terminal state. */
    __asm__ __volatile__("csrrc %0, mstatus, %1"
                         : "=r"(saved_mstatus) : "r"(mie) : "memory");
    ulp_c5vrx2_command = CMD_CONTINUOUS;
    for (;;) {
        const uint32_t state = ulp_c5vrx2_state;
        if (state == STATE_RUNNING) saw_running = true;
        if (state == STATE_ERROR || state == STATE_STOPPED) break;
    }
    if ((saved_mstatus & mie) != 0u) {
        __asm__ __volatile__("csrs mstatus, %0" :: "r"(mie) : "memory");
    }
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

    /* LP detects the physical boundary and starts PAU. REGDMA performs the
     * four MODEM writes, so both independent masters need the audited REE0
     * access installed after ulp_lp_core_run() resets LP's identity. */
    const uint64_t lp_rw =
        BIT64(APM_TEE_HP_PERIPH_MODEM) |
        BIT64(APM_TEE_HP_PERIPH_REGDMA) |
        BIT64(APM_TEE_HP_PERIPH_SYSTEM_REG) |
        BIT64(APM_TEE_HP_PERIPH_PCR_REG) |
        BIT64(APM_TEE_HP_PERIPH_PARL_IO);
    apm_hal_set_master_sec_mode(BIT(APM_MASTER_LPCORE) |
                                BIT(APM_MASTER_REGDMA),
                                APM_SEC_MODE_REE0);
    apm_hal_tee_set_peri_access(APM_TEE_CTRL_HP, lp_rw,
                                APM_SEC_MODE_REE0, APM_PERM_R | APM_PERM_W);

    /* ESP32-C5 has one always-on REGDMA entry address. Point it at the donor's
     * four masked WRITE nodes in LP SRAM before HP SRAM changes ownership. */
    err = c5vrx2_regdma_arm(ulp_c5vrx2_regdma_link_root);
    if (err != ESP_OK) return err;

    /* Mount the cyclic RF-SRAM -> BitScrambler -> PARLIO transaction while HP
     * still owns the SRAM. Its source clock stays paused until LP has acquired
     * a half-block of real IQ lead. */
    err = c5vrx2_parlio_direct_prepare();
    if (err != ESP_OK) return err;

    ESP_LOGW(TAG,
             "AV TEST ARM: A1 IQ -> REGDMA 16K rearm -> donor-scale phase6 delta -> 20MS/s PARLIO; HP parked");

    /* Once LP lends 0x40830000..0x4083ffff to MAC dump, HP must not run normal
     * scheduler/interrupt code. Healthy realtime service stays parked. */
    const bool ran = park_hp_until_terminal();

    c5vrx2_parlio_direct_destroy();

    const uint32_t blocks = ulp_c5vrx2_blocks;
    const uint32_t fill_avg = blocks != 0u
        ? ulp_c5vrx2_fill_cycles_total / blocks : 0u;
    const uint32_t gap_avg = ulp_c5vrx2_rearms != 0u
        ? ulp_c5vrx2_gap_cycles_total / ulp_c5vrx2_rearms : 0u;
    const uint32_t average_block_source_hz = fill_avg != 0u
        ? (uint32_t)((16384ull * 48000000ull) / fill_avg) : 0u;
    const uint32_t active_source_hz = blocks != 0u &&
                                      ulp_c5vrx2_fill_cycles_min != 0u
        ? (uint32_t)((16384ull * 48000000ull) /
                     ulp_c5vrx2_fill_cycles_min) : 0u;
    const uint32_t effective_source_hz = ulp_c5vrx2_run_cycles != 0u
        ? (uint32_t)(((uint64_t)ulp_c5vrx2_observed_words * 48000000ull) /
                     ulp_c5vrx2_run_cycles) : 0u;
    /* The native USB endpoint is intentionally unavailable while HP is
     * parked. Repeat the bounded result after SRAM ownership is restored so a
     * WebSerial terminal opened after re-enumeration can still collect it. */
    for (unsigned report = 1u; report <= 30u; ++report) {
        ESP_LOGE(TAG,
                 "PRODUCER ACTIVITY report=%u/30 state=%u blocks=%u rearms=%u failures=%u words=%u ptr=%u changes=%u pauses=%u resumes=%u pause_active=%u pause_last=%u pause_max=%u run_cyc=%u fill_min=%u fill_avg=%u fill_last=%u fill_max=%u reset_cyc=%u reset_ptr=%u gap_min=%u gap_avg=%u gap_last=%u gap_max=%u advance_ptr=%u active_source_hz=%u average_block_source_hz=%u effective_source_hz=%u fail_reason=%u fail_ctrl=0x%08x fail_ptr_mode=0x%08x start_ctrl=0x%08x pau_conf=0x%08x pau_raw=0x%08x pau_link=0x%08x pau_peri=0x%08x pau_mem=0x%08x pau_timeout=%u pau_flow=%u",
                 report,
                 (unsigned)ulp_c5vrx2_state,
                 (unsigned)blocks,
                 (unsigned)ulp_c5vrx2_rearms,
                 (unsigned)ulp_c5vrx2_rearm_failures,
                 (unsigned)ulp_c5vrx2_observed_words,
                 (unsigned)ulp_c5vrx2_last_pointer,
                 (unsigned)ulp_c5vrx2_pointer_changes,
                 (unsigned)ulp_c5vrx2_activity_pauses,
                 (unsigned)ulp_c5vrx2_activity_resumes,
                 (unsigned)ulp_c5vrx2_pause_active,
                 (unsigned)ulp_c5vrx2_pause_cycles_last,
                 (unsigned)ulp_c5vrx2_pause_cycles_max,
                 (unsigned)ulp_c5vrx2_run_cycles,
                 (unsigned)ulp_c5vrx2_fill_cycles_min,
                 (unsigned)fill_avg,
                 (unsigned)ulp_c5vrx2_fill_cycles_last,
                 (unsigned)ulp_c5vrx2_fill_cycles_max,
                 (unsigned)ulp_c5vrx2_reset_cycles,
                 (unsigned)ulp_c5vrx2_reset_pointer,
                 (unsigned)ulp_c5vrx2_gap_cycles_min,
                 (unsigned)gap_avg,
                 (unsigned)ulp_c5vrx2_gap_cycles_last,
                 (unsigned)ulp_c5vrx2_gap_cycles_max,
                 (unsigned)ulp_c5vrx2_advance_pointer,
                 (unsigned)active_source_hz,
                 (unsigned)average_block_source_hz,
                 (unsigned)effective_source_hz,
                 (unsigned)ulp_c5vrx2_fail_reason,
                 (unsigned)ulp_c5vrx2_fail_control,
                 (unsigned)ulp_c5vrx2_fail_pointer_mode,
                 (unsigned)ulp_c5vrx2_start_control,
                 (unsigned)ulp_c5vrx2_regdma_conf,
                 (unsigned)ulp_c5vrx2_regdma_int_raw,
                 (unsigned)ulp_c5vrx2_regdma_current_link,
                 (unsigned)ulp_c5vrx2_regdma_peri_addr,
                 (unsigned)ulp_c5vrx2_regdma_mem_addr,
                 (unsigned)ulp_c5vrx2_regdma_timed_out,
                 (unsigned)(ulp_c5vrx2_regdma_conf & 0x7u));
        if (report != 30u) vTaskDelay(pdMS_TO_TICKS(1000u));
    }
    ESP_LOGE(TAG,
             "REALTIME STOP state=%u ran=%u blocks=%u rearms=%u failures=%u words=%u pauses=%u resumes=%u pause_active=%u fault=%u addr=0x%08x pc=0x%08x",
             (unsigned)ulp_c5vrx2_state, ran ? 1u : 0u,
             (unsigned)blocks,
             (unsigned)ulp_c5vrx2_rearms,
             (unsigned)ulp_c5vrx2_rearm_failures,
             (unsigned)ulp_c5vrx2_observed_words,
             (unsigned)ulp_c5vrx2_activity_pauses,
             (unsigned)ulp_c5vrx2_activity_resumes,
             (unsigned)ulp_c5vrx2_pause_active,
             (unsigned)ulp_c5vrx2_fault_cause,
             (unsigned)ulp_c5vrx2_fault_address,
             (unsigned)ulp_c5vrx2_fault_pc);
    return ran ? ESP_FAIL : ESP_ERR_INVALID_STATE;
}
