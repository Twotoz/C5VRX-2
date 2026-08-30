#pragma once

#include "esp_err.h"

/* Starts LP-owned IQ+PARLIO and parks HP while RF dump SRAM belongs to MAC.
 * Healthy continuous operation intentionally does not return. */
esp_err_t c5vrx2_realtime_start(void);
