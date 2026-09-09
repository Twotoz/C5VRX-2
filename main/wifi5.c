#include "wifi5.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

static const char *TAG = "c5vrx2_wifi5";

esp_err_t c5vrx2_wifi5_start_a1(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if ((err = esp_wifi_init(&cfg)) != ESP_OK) return err;
    if ((err = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK) return err;
    if ((err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) return err;
    if ((err = esp_wifi_start()) != ESP_OK) return err;

#if CONFIG_SOC_WIFI_SUPPORT_5G
    if ((err = esp_wifi_set_band_mode(WIFI_BAND_MODE_5G_ONLY)) != ESP_OK)
        return err;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif

    (void)esp_wifi_set_ps(WIFI_PS_NONE);
    wifi_protocols_t protocols = {
        .ghz_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G |
                  WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX,
        .ghz_5g = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N,
    };
    if ((err = esp_wifi_set_protocols(WIFI_IF_STA, &protocols)) != ESP_OK)
        return err;

    wifi_bandwidths_t bandwidths = {
        .ghz_2g = WIFI_BW20,
        .ghz_5g = WIFI_BW40,
    };
    err = esp_wifi_set_bandwidths(WIFI_IF_STA, &bandwidths);
    if (err != ESP_OK) {
        bandwidths.ghz_5g = WIFI_BW20;
        if ((err = esp_wifi_set_bandwidths(WIFI_IF_STA, &bandwidths)) != ESP_OK)
            return err;
    }

    /* A1 is exactly Wi-Fi channel 173 = 5865 MHz on the C5 public driver. */
    if ((err = esp_wifi_set_channel(173, WIFI_SECOND_CHAN_NONE)) != ESP_OK)
        return err;
    if ((err = esp_wifi_set_promiscuous(true)) != ESP_OK) return err;

    uint8_t primary = 0;
    wifi_second_chan_t secondary = WIFI_SECOND_CHAN_NONE;
    if ((err = esp_wifi_get_channel(&primary, &secondary)) != ESP_OK) return err;
    if (primary != 173) return ESP_ERR_INVALID_STATE;

    ESP_LOGW(TAG, "A1 RF frontend active: 5865 MHz/ch173; IQ producer is not VTX-gated");
    return ESP_OK;
}
