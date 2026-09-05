#include "realtime.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "calibration.h"
#include "continuous_iq.h"
#include "parlio_direct.h"
#include "rf_dump.h"
#include "startup_trace.h"
#include "wifi5.h"

#define START_LEAD_WORDS      (C5VRX2_RF_WORDS / 2u)
#define START_TOLERANCE_WORDS 512u
#define START_TIMEOUT_US      20000u
#define TELEMETRY_PERIOD_MS   1000u
#define SUPERVISOR_PERIOD_MS  5u

static const char *TAG = "c5vrx2_rt";
static volatile uint32_t s_supervisor_faults;

static void supervisor_task(void *argument)
{
    (void)argument;
    uint32_t previous_pointer = UINT32_MAX;
    unsigned stationary_checks = 0u;
    bool reported = false;

    for (;;) {
        continuous_iq_stats_t stats;
        continuous_iq_get_stats(&stats);
        stationary_checks = stats.writer_pointer == previous_pointer ?
                            stationary_checks + 1u : 0u;
        previous_pointer = stats.writer_pointer;
        const bool healthy =
            (stats.dump_control & 0x80040000u) == 0x80000000u &&
            stationary_checks < 2u &&
            c5vrx2_wifi5_tx_is_quiescent() &&
            c5vrx2_rf_dump_guards_valid();
        if (!healthy && !reported) {
            s_supervisor_faults++;
            ESP_LOGE(TAG,
                     "SUPERVISOR FAULT count=%u ctrl=0x%08x ptr=%u "
                     "stationary=%u tx_quiet=%u guards=%u; no periodic rearm",
                     (unsigned)s_supervisor_faults,
                     (unsigned)stats.dump_control,
                     (unsigned)stats.writer_pointer,
                     stationary_checks,
                     c5vrx2_wifi5_tx_is_quiescent(),
                     c5vrx2_rf_dump_guards_valid());
            reported = true;
        } else if (healthy) {
            reported = false;
        }
        vTaskDelay(pdMS_TO_TICKS(SUPERVISOR_PERIOD_MS));
    }
}

static void telemetry_task(void *argument)
{
    (void)argument;
    uint32_t previous_pointer = UINT32_MAX;
    uint32_t unchanged_reports = 0u;

    for (;;) {
        continuous_iq_stats_t stats;
        continuous_iq_get_stats(&stats);
        if (stats.writer_pointer == previous_pointer)
            unchanged_reports++;
        else
            unchanged_reports = 0u;
        previous_pointer = stats.writer_pointer;

        ESP_LOGI(TAG,
                 "LIVE rf_hz=%u av_hz=%u ptr=%u wraps=%u ctrl=0x%08x "
                 "dump_enable=%u done=%u starts=%u rearms=%u triggers=%u "
                 "unchanged=%u faults=%u tx_quiet=%u "
                 "guards=%u overruns=%u ambiguous=%u",
                 (unsigned)stats.rf_sample_rate_hz,
                 (unsigned)c5vrx2_parlio_direct_rate_hz(),
                 (unsigned)stats.writer_pointer,
                 (unsigned)stats.physical_wraps,
                 (unsigned)stats.dump_control,
                 (stats.dump_control & 0x80000000u) != 0u,
                 (stats.dump_control & 0x00040000u) != 0u,
                 (unsigned)stats.producer_start_count,
                 (unsigned)stats.rearm_count,
                 (unsigned)stats.trigger_count,
                 (unsigned)unchanged_reports,
                 (unsigned)s_supervisor_faults,
                 c5vrx2_wifi5_tx_is_quiescent(),
                 c5vrx2_rf_dump_guards_valid(),
                 (unsigned)stats.overruns,
                 (unsigned)stats.ambiguous_wraps);
        vTaskDelay(pdMS_TO_TICKS(TELEMETRY_PERIOD_MS));
    }
}

esp_err_t c5vrx2_realtime_start(void)
{
    const c5vrx2_calibration_t *cal = c5vrx2_calibration_get();
    c5vrx2_trace_stage(10u, ESP_OK);
    esp_err_t err = continuous_iq_start();
    if (err != ESP_OK) {
        c5vrx2_trace_stage(10u, err);
        ESP_LOGE(TAG, "continuous IQ start failed: %s", esp_err_to_name(err));
        return err;
    }
    c5vrx2_trace_stage(11u, ESP_OK);

    const uint32_t rf_rate_hz = continuous_iq_sample_rate_hz();
    const uint32_t av_rate_hz = (rf_rate_hz + 2u) / 4u;
    if (rf_rate_hz < 20000000u || rf_rate_hz > 100000000u ||
        av_rate_hz < 5000000u || av_rate_hz > 25000000u) {
        ESP_LOGE(TAG, "implausible measured RF cadence: %u Hz",
                 (unsigned)rf_rate_hz);
        c5vrx2_trace_stage(12u, ESP_ERR_INVALID_RESPONSE);
        (void)continuous_iq_stop();
        return ESP_ERR_INVALID_RESPONSE;
    }
    c5vrx2_trace_stage(12u, ESP_OK);

    err = c5vrx2_parlio_direct_prepare(av_rate_hz);
    if (err != ESP_OK) {
        c5vrx2_trace_stage(13u, err);
        (void)continuous_iq_stop();
        return err;
    }
    c5vrx2_trace_stage(13u, ESP_OK);

    /* Start cyclic GDMA half a physical ring behind the autonomous writer.
     * The descriptor and BitScrambler state then loop forever; neither side
     * is restarted at the 16K address wrap. */
    err = continuous_iq_wait_base_lead(START_LEAD_WORDS,
                                       START_TOLERANCE_WORDS,
                                       START_TIMEOUT_US);
    c5vrx2_trace_stage(14u, err);
    if (err == ESP_OK) {
        c5vrx2_trace_stage(15u, ESP_OK);
        err = c5vrx2_parlio_direct_start();
        c5vrx2_trace_stage(16u, err);
    }
    if (err != ESP_OK) {
        c5vrx2_parlio_direct_destroy();
        (void)continuous_iq_stop();
        return err;
    }

    ESP_LOGW(TAG,
             "LIVE START: pre-trigger IQ ring -> adjacent FM -> real 4:1 "
             "boxcar -> PARLIO loop; rf=%u Hz av=%u Hz pedestal=%u gain=%ux "
             "polarity=%s; USB remains scheduled",
             (unsigned)rf_rate_hz, (unsigned)av_rate_hz,
             cal->pedestal_code, cal->discriminator_gain,
             cal->polarity == C5VRX2_POLARITY_CURRENT_MINUS_PREVIOUS ?
                 "current-minus-previous" : "previous-minus-current");

    if (xTaskCreate(telemetry_task, "c5vrx2_diag", 3072u, NULL,
                    tskIDLE_PRIORITY + 1u, NULL) != pdPASS) {
        ESP_LOGW(TAG, "telemetry task unavailable; realtime path remains live");
    }
    if (xTaskCreate(supervisor_task, "c5vrx2_watch", 2048u, NULL,
                    tskIDLE_PRIORITY + 2u, NULL) != pdPASS) {
        ESP_LOGW(TAG, "supervisor unavailable; realtime path remains live");
    }
    return ESP_OK;
}
