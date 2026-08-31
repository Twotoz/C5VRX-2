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

    /* The LP core directly executes the writer rearm sequence. */
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
             "REALTIME ARM: A1 IQ -> DONE-cleared direct LP 16K rearm -> phase-delta -> 20MS/s PARLIO; HP parked");

    /* Once LP lends 0x40830000..0x4083ffff to MAC dump, HP must not run normal
     * scheduler/interrupt code. Healthy realtime service stays parked. */
    const bool ran = park_hp_until_terminal();

    /* FreeRTOS/driver objects may have been touched while their backing SRAM
     * was lent to the RF writer.  Calling the PARLIO driver's queue-based
     * teardown here triggered xTaskPriorityDisinherit on the physical XIAO.
     * The bounded diagnostic never starts a second transaction, so quiesce
     * hardware directly and keep the allocated objects intact for logging. */
    c5vrx2_parlio_direct_quiesce();
    const uint32_t blocks = ulp_c5vrx2_blocks;
    const uint32_t fill_avg = blocks != 0u
        ? ulp_c5vrx2_fill_cycles_total / blocks : 0u;
    const uint32_t gap_avg = ulp_c5vrx2_rearms != 0u
        ? ulp_c5vrx2_gap_cycles_total / ulp_c5vrx2_rearms : 0u;
    const uint32_t source_hz = fill_avg != 0u
        ? (uint32_t)((16384ull * 48000000ull) / fill_avg) : 0u;
    const uint32_t matched_parlio_hz = (fill_avg + gap_avg) != 0u
        ? (uint32_t)((4096ull * 48000000ull) /
                     (fill_avg + gap_avg)) : 0u;
    /* The native USB endpoint is intentionally unavailable while HP is
     * parked. Repeat the bounded result after SRAM ownership is restored so a
     * WebSerial terminal opened after re-enumeration can still collect it. */
    for (unsigned report = 1u; report <= 30u; ++report) {
        ESP_LOGE(TAG,
                 "CADENCE MEASURED report=%u/30 state=%u blocks=%u rearms=%u failures=%u fill_min=%u fill_avg=%u fill_last=%u fill_max=%u gap_min=%u gap_avg=%u gap_last=%u gap_max=%u source_hz=%u matched_parlio_hz=%u fail_reason=%u fail_ctrl=0x%08x fail_ptr_mode=0x%08x arm_ctrl=0x%08x arm_ptr_pre=0x%08x arm_ptr_post=0x%08x arm_fmt_pre=0x%08x arm_fmt_post=0x%08x",
                 report,
                 (unsigned)ulp_c5vrx2_state,
                 (unsigned)blocks,
                 (unsigned)ulp_c5vrx2_rearms,
                 (unsigned)ulp_c5vrx2_rearm_failures,
                 (unsigned)ulp_c5vrx2_fill_cycles_min,
                 (unsigned)fill_avg,
                 (unsigned)ulp_c5vrx2_fill_cycles_last,
                 (unsigned)ulp_c5vrx2_fill_cycles_max,
                 (unsigned)ulp_c5vrx2_gap_cycles_min,
                 (unsigned)gap_avg,
                 (unsigned)ulp_c5vrx2_gap_cycles_last,
                 (unsigned)ulp_c5vrx2_gap_cycles_max,
                 (unsigned)source_hz,
                 (unsigned)matched_parlio_hz,
                 (unsigned)ulp_c5vrx2_fail_reason,
                 (unsigned)ulp_c5vrx2_fail_control,
                 (unsigned)ulp_c5vrx2_fail_pointer_mode,
                 (unsigned)ulp_c5vrx2_start_control,
                 (unsigned)ulp_c5vrx2_arm_pointer_mode,
                 (unsigned)ulp_c5vrx2_started_pointer_mode,
                 (unsigned)ulp_c5vrx2_arm_format,
                 (unsigned)ulp_c5vrx2_started_format);
        if (report != 30u) vTaskDelay(pdMS_TO_TICKS(1000u));
    }
    ESP_LOGE(TAG,
             "REALTIME STOP state=%u ran=%u blocks=%u rearms=%u failures=%u fault=%u addr=0x%08x pc=0x%08x",
             (unsigned)ulp_c5vrx2_state, ran ? 1u : 0u,
             (unsigned)blocks,
             (unsigned)ulp_c5vrx2_rearms,
             (unsigned)ulp_c5vrx2_rearm_failures,
             (unsigned)ulp_c5vrx2_fault_cause,
             (unsigned)ulp_c5vrx2_fault_address,
             (unsigned)ulp_c5vrx2_fault_pc);
    return ran ? ESP_FAIL : ESP_ERR_INVALID_STATE;
}
