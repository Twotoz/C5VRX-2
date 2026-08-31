#pragma once

#include <stdbool.h>
#include "esp_err.h"

#define C5VRX2_RF_DUMP_BASE 0x40830000u
#define C5VRX2_RF_WORDS     16384u

bool c5vrx2_rf_dump_memory_reserved(void);
esp_err_t c5vrx2_rf_dump_prepare_mode0(void);
