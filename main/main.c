#include <stdint.h>

#include "esp_log.h"

#include "iq_decode.h"

static const char *TAG = "c5vrx2";

void app_main(void)
{
    /*
     * Deliberately no analyzer, NO_RF gate, USB gate or repeated vendor
     * adctrig() loop here. The next commit on this branch wires the proven
     * C5 RF init/tune + direct 16K restart primitive into the producer, then
     * feeds a minimal discriminator into PARLIO.
     *
     * Keep this entry point inert rather than silently falling back to the old
     * finite capture architecture.
     */
    ESP_LOGW(TAG,
             "C5VRX-2 realtime branch: RF producer donor not wired yet; refusing finite-capture fallback");
}
