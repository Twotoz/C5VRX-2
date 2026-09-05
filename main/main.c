#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wifi5.h"
#include "calibration.h"
#include "diagnostics.h"
#include "realtime.h"
#include "startup_trace.h"

static const char *TAG = "c5vrx2";

static void report_fatal_forever(const char *stage, esp_err_t err)
{
    for (;;) {
        ESP_LOGE(TAG, "STARTUP STOP stage=%s error=%s; device remains alive",
                 stage, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_init();
    }
    return err;
}

void app_main(void)
{
#if CONFIG_C5VRX2_MODE_AV_STATIC
    ESP_LOGW(TAG, "C5VRX-2: static resistor-DAC diagnostic boot");
    const esp_err_t err = c5vrx2_av_static_diagnostic_start(
        CONFIG_C5VRX2_AV_STATIC_CODE);
    if (err != ESP_OK) ESP_LOGE(TAG, "AV diagnostic failed: %s",
                                esp_err_to_name(err));
    return;
#elif CONFIG_C5VRX2_MODE_AV_PAL_MONO || CONFIG_C5VRX2_MODE_AV_PAL_COLOR
    ESP_LOGW(TAG, "C5VRX-2: synthetic PAL diagnostic boot");
#if CONFIG_C5VRX2_MODE_AV_PAL_COLOR
    const bool colour = true;
#else
    const bool colour = false;
#endif
    const esp_err_t err = c5vrx2_av_pal_diagnostic_start(
        colour);
    if (err != ESP_OK) ESP_LOGE(TAG, "PAL diagnostic failed: %s",
                                esp_err_to_name(err));
    return;
#else
    ESP_LOGW(TAG, "C5VRX-2: continuous adjacent-IQ FM receiver boot");

    esp_err_t err = init_nvs();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
        report_fatal_forever("nvs", err);
    }

    c5vrx2_calibration_load();
    c5vrx2_trace_begin();
    c5vrx2_trace_stage(2u, ESP_OK);

    err = c5vrx2_wifi5_start_a1();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "A1 RF init failed: %s", esp_err_to_name(err));
        report_fatal_forever("wifi5_start_a1", err);
    }
    c5vrx2_trace_stage(3u, ESP_OK);

#if CONFIG_C5VRX2_MODE_RF_ORACLE
    err = c5vrx2_rf_oracle_diagnostic_start();
#elif CONFIG_C5VRX2_MODE_RF_WRAP
    err = c5vrx2_rf_wrap_diagnostic_run();
#else
    err = c5vrx2_realtime_start();
#endif
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "selected receiver mode failed: %s",
                 esp_err_to_name(err));
#if CONFIG_C5VRX2_MODE_LIVE
        /* USB Serial/JTAG can disappear while the experimental RF SRAM
         * ownership is active. Use the already-proven AV path as an
         * unambiguous bring-up fault indicator: visible monochrome bars mean
         * live startup returned an error before a continuous output existed. */
        const esp_err_t av_err = c5vrx2_av_pal_diagnostic_start(false);
        if (av_err == ESP_OK) {
            ESP_LOGE(TAG, "LIVE START FAILED: showing PAL bars as fault code");
            return;
        }
        ESP_LOGE(TAG, "visual fault indicator failed: %s",
                 esp_err_to_name(av_err));
#endif
        report_fatal_forever("receiver", err);
    }
    ESP_LOGI(TAG, "selected receiver mode completed/started; main task released");
#endif
}
