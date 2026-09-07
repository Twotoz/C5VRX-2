#pragma once

#include "driver/bitscrambler.h"
#include "esp_err.h"

esp_err_t c5vrx2_wbfm_q4_configure(bitscrambler_handle_t handle);
const void *c5vrx2_wbfm_q4_program(void);
