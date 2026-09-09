#pragma once

#include "driver/bitscrambler.h"
#include "esp_err.h"

esp_err_t c5vrx2_wbfm_direct_create(bitscrambler_handle_t *out);
void c5vrx2_wbfm_direct_destroy(bitscrambler_handle_t handle);
