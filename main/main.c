#include "esp_log.h"

#include "wifi5.h"
#include "rf_dump.h"
#include "realtime.h"

static const char *TAG = "c5vrx2";

void app_main(void)
{
    ESP_LOGW(TAG, "C5VRX-2: raw realtime A1 receiver boot");

    esp_err_t err = c5vrx2_wifi5_start_a1();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "A1 RF init failed: %s", esp_err_to_name(err));
        return;
    }

    err = c5vrx2_rf_dump_prepare_mode0();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mode0 IQ prepare failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGW(TAG,
             "IQ producer prepared: 16384-word mode0, VTX-independent; starting direct LP rearm + PARLIO");
    err = c5vrx2_realtime_start();
    ESP_LOGE(TAG, "realtime service returned unexpectedly: %s", esp_err_to_name(err));
}
