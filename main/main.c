#include "esp_log.h"
#include "nvs_flash.h"

#include "wifi5.h"
#include "calibration.h"
#include "diagnostics.h"
#include "realtime.h"

static const char *TAG = "c5vrx2";

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
    const esp_err_t err = c5vrx2_av_pal_diagnostic_start(
        CONFIG_C5VRX2_MODE_AV_PAL_COLOR);
    if (err != ESP_OK) ESP_LOGE(TAG, "PAL diagnostic failed: %s",
                                esp_err_to_name(err));
    return;
#else
    ESP_LOGW(TAG, "C5VRX-2: continuous adjacent-IQ FM receiver boot");

    esp_err_t err = init_nvs();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
        return;
    }

    c5vrx2_calibration_load();

    err = c5vrx2_wifi5_start_a1();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "A1 RF init failed: %s", esp_err_to_name(err));
        return;
    }

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
        return;
    }
    ESP_LOGI(TAG, "selected receiver mode completed/started; main task released");
#endif
}
