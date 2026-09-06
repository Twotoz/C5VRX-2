#include "realtime.h"

#include "esp_log.h"

static const char *TAG = "c5vrx2_rt";

esp_err_t c5vrx2_realtime_start(void)
{
    /* Ordinary CPU/AHB-GDMA sees a stale image of the active MAC-owned dump
     * SRAM. Keep this guard at the API boundary as well as in app_main(), so
     * no future caller can accidentally re-enable the known-bad direct path.
     * The replacement will be MODEM_DIAG -> source-clocked PARLIO RX ->
     * adjacent FM -> real resampler -> continuous PARLIO TX. */
    ESP_LOGE(TAG,
             "LIVE unavailable: simultaneous IQ source and source clock "
             "are not yet proven");
    return ESP_ERR_NOT_SUPPORTED;
}
